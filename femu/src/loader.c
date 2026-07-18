/*
 * loader.c — ELF + raw binary loader
 */

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <elf.h>

#include "../include/loader.h"
#include "../include/memory.h"

/* ─── Internal helpers ───────────────────────────────────────────── */
static long file_size(int fd) {
    struct stat st;
    if (fstat(fd, &st) < 0) return -1;
    return (long)st.st_size;
}

static int read_file(const char *path, uint8_t **out, size_t *out_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { femu_err("Cannot open '%s': %s", path, strerror(errno)); return -1; }
    long sz = file_size(fd);
    if (sz < 0) { close(fd); return -1; }
    *out = malloc(sz);
    if (!*out) { close(fd); return -1; }
    ssize_t n = read(fd, *out, sz);
    close(fd);
    if (n != sz) { free(*out); return -1; }
    *out_len = (size_t)sz;
    return 0;
}

/* ─── ELF machine → FEMUArch ─────────────────────────────────────── */
static FEMUArch elf_machine_to_arch(uint16_t machine, uint8_t class_) {
    switch (machine) {
        case EM_386:    return ARCH_X86;
        case EM_X86_64: return ARCH_X86_64;
        case EM_ARM:    return ARCH_ARM;
        case EM_AARCH64:return ARCH_ARM64;
        case EM_RISCV:
            return (class_ == ELF_CLASS64) ? ARCH_RISCV64 : ARCH_RISCV32;
        default:        return ARCH_NONE;
    }
}

/* ─── Arch auto-detect from ELF header ───────────────────────────── */
FEMUArch loader_detect_arch(const char *path) {
    uint8_t *data; size_t len;
    if (read_file(path, &data, &len) < 0) return ARCH_NONE;
    if (len < 20 || memcmp(data, ELF_MAGIC, 4) != 0) {
        free(data); return ARCH_NONE;
    }
    uint8_t  class_   = data[4];
    uint16_t machine;
    if (class_ == ELF_CLASS64) {
        if (len < sizeof(Elf64_Ehdr)) { free(data); return ARCH_NONE; }
        machine = ((Elf64_Ehdr*)data)->e_machine;
    } else {
        if (len < sizeof(Elf32_Ehdr)) { free(data); return ARCH_NONE; }
        machine = ((Elf32_Ehdr*)data)->e_machine;
    }
    FEMUArch arch = elf_machine_to_arch(machine, class_);
    free(data);
    return arch;
}

/* ─── Load ELF64 ─────────────────────────────────────────────────── */
static int load_elf64(FEMUMemory *mem, const uint8_t *data, size_t len,
                      FEMULoadResult *res) {
    if (len < sizeof(Elf64_Ehdr)) {
        femu_err("ELF64: file too small"); return FEMU_ERR_LOADER;
    }
    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr*)data;

    res->arch      = elf_machine_to_arch(ehdr->e_machine, ELF_CLASS64);
    res->entry_pc  = ehdr->e_entry;
    res->is_elf    = true;
    res->text_start = UINT64_MAX;
    res->text_end   = 0;

    /* Walk program headers */
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (ehdr->e_phoff + (i + 1) * sizeof(Elf64_Phdr) > len) break;
        const Elf64_Phdr *phdr =
            (const Elf64_Phdr*)(data + ehdr->e_phoff + i * sizeof(Elf64_Phdr));

        if (phdr->p_type != PT_LOAD) continue;
        if (phdr->p_filesz == 0)     continue;

        uint64_t vaddr  = phdr->p_vaddr;
        uint64_t memsz  = phdr->p_memsz;
        uint64_t filesz = phdr->p_filesz;
        uint64_t offset = phdr->p_offset;

        if (offset + filesz > len) {
            femu_err("ELF64: segment out of file"); return FEMU_ERR_LOADER;
        }

        /* Write segment into guest memory */
        int r = mem_write(mem, vaddr, data + offset, filesz);
        if (r < 0) {
            femu_err("ELF64: failed to load segment at 0x%llx",
                     (unsigned long long)vaddr);
            return FEMU_ERR_LOADER;
        }
        /* Zero BSS */
        if (memsz > filesz) {
            uint8_t *zbuf = calloc(1, memsz - filesz);
            if (zbuf) {
                mem_write(mem, vaddr + filesz, zbuf, memsz - filesz);
                free(zbuf);
            }
        }

        femu_dbg("Loaded PT_LOAD segment vaddr=0x%llx size=0x%llx",
                 (unsigned long long)vaddr, (unsigned long long)memsz);

        /* Track executable segment bounds for AOT */
        if (phdr->p_flags & 1 /* PF_X */) {
            if (vaddr < res->text_start) res->text_start = vaddr;
            if (vaddr + memsz > res->text_end) res->text_end = vaddr + memsz;
        }
    }

    if (res->text_start == UINT64_MAX) {
        /* No explicit exec segment — use entire load range */
        res->text_start = res->entry_pc;
        res->text_end   = res->entry_pc + 0x10000; /* default 64KB */
    }
    return FEMU_OK;
}

/* ─── Load ELF32 ─────────────────────────────────────────────────── */
static int load_elf32(FEMUMemory *mem, const uint8_t *data, size_t len,
                      FEMULoadResult *res) {
    if (len < sizeof(Elf32_Ehdr)) {
        femu_err("ELF32: file too small"); return FEMU_ERR_LOADER;
    }
    const Elf32_Ehdr *ehdr = (const Elf32_Ehdr*)data;

    res->arch      = elf_machine_to_arch(ehdr->e_machine, ELF_CLASS32);
    res->entry_pc  = ehdr->e_entry;
    res->is_elf    = true;
    res->text_start = UINT64_MAX;
    res->text_end   = 0;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (ehdr->e_phoff + (i + 1) * sizeof(Elf32_Phdr) > len) break;
        const Elf32_Phdr *phdr =
            (const Elf32_Phdr*)(data + ehdr->e_phoff + i * sizeof(Elf32_Phdr));

        if (phdr->p_type != PT_LOAD) continue;
        if (phdr->p_filesz == 0)     continue;

        uint32_t vaddr  = phdr->p_vaddr;
        uint32_t memsz  = phdr->p_memsz;
        uint32_t filesz = phdr->p_filesz;
        uint32_t offset = phdr->p_offset;

        if (offset + filesz > len) {
            femu_err("ELF32: segment out of file"); return FEMU_ERR_LOADER;
        }
        mem_write(mem, vaddr, data + offset, filesz);
        if (memsz > filesz) {
            uint8_t *zbuf = calloc(1, memsz - filesz);
            if (zbuf) { mem_write(mem, vaddr + filesz, zbuf, memsz - filesz); free(zbuf); }
        }
        if (phdr->p_flags & 1 /* PF_X */) {
            if (vaddr < res->text_start) res->text_start = vaddr;
            if (vaddr + memsz > res->text_end) res->text_end = vaddr + memsz;
        }
    }

    if (res->text_start == UINT64_MAX) {
        res->text_start = res->entry_pc;
        res->text_end   = res->entry_pc + 0x10000;
    }
    return FEMU_OK;
}

/* ─── Public API ─────────────────────────────────────────────────── */
int loader_load_elf(FEMUMemory *mem, const char *path, FEMULoadResult *result) {
    uint8_t *data; size_t len;
    if (read_file(path, &data, &len) < 0) return FEMU_ERR_IO;

    if (len < 5 || memcmp(data, ELF_MAGIC, 4) != 0) {
        femu_err("'%s' is not an ELF binary", path);
        free(data); return FEMU_ERR_LOADER;
    }

    int r;
    if (data[4] == ELF_CLASS64)
        r = load_elf64(mem, data, len, result);
    else
        r = load_elf32(mem, data, len, result);

    free(data);
    return r;
}

int loader_load_raw(FEMUMemory *mem, const char *path,
                    uint64_t load_addr, FEMULoadResult *result) {
    uint8_t *data; size_t len;
    if (read_file(path, &data, &len) < 0) return FEMU_ERR_IO;

    int r = mem_write(mem, load_addr, data, len);
    free(data);
    if (r < 0) return FEMU_ERR_LOADER;

    result->entry_pc   = load_addr;
    result->text_start = load_addr;
    result->text_end   = load_addr + len;
    result->arch       = ARCH_NONE;
    result->is_elf     = false;
    return FEMU_OK;
}

int loader_load(FEMUMemory *mem, const char *path,
                FEMUArch hint_arch, FEMULoadResult *result) {
    /* Probe for ELF magic */
    int fd = open(path, O_RDONLY);
    if (fd < 0) { femu_err("Cannot open '%s': %s", path, strerror(errno)); return FEMU_ERR_IO; }
    uint8_t magic[4] = {0};
    read(fd, magic, 4);
    close(fd);

    if (memcmp(magic, ELF_MAGIC, 4) == 0) {
        int r = loader_load_elf(mem, path, result);
        if (r == FEMU_OK && result->arch == ARCH_NONE)
            result->arch = hint_arch;
        return r;
    }
    /* Fallback: raw binary at address 0 */
    femu_warn("'%s' has no ELF magic — loading as raw binary at 0x0", path);
    int r = loader_load_raw(mem, path, 0x0, result);
    if (r == FEMU_OK) result->arch = hint_arch;
    return r;
}

void loader_dump_info(const FEMULoadResult *res, FILE *out) {
    fprintf(out, "[loader] Loaded binary:\n");
    fprintf(out, "  Format     : %s\n", res->is_elf ? "ELF" : "raw");
    fprintf(out, "  Guest arch : %s\n", femu_arch_name(res->arch));
    fprintf(out, "  Entry point: 0x%016llx\n", (unsigned long long)res->entry_pc);
    fprintf(out, "  Text range : [0x%016llx, 0x%016llx)\n",
            (unsigned long long)res->text_start,
            (unsigned long long)res->text_end);
    fprintf(out, "  Text size  : %llu bytes\n",
            (unsigned long long)(res->text_end - res->text_start));
}
