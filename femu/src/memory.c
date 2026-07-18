/*
 * memory.c — Memory subsystem implementation
 */

#include <sys/mman.h>
#include <errno.h>
#include "../include/memory.h"

/* ─── Lifecycle ─────────────────────────────────────────────────── */

FEMUMemory *mem_create(void) {
    FEMUMemory *mem = calloc(1, sizeof(FEMUMemory));
    if (!mem) { femu_err("mem_create: OOM"); return NULL; }
    return mem;
}

void mem_destroy(FEMUMemory *mem) {
    if (!mem) return;
    for (int i = 0; i < mem->num_regions; i++) {
        FEMUMemRegion *r = &mem->regions[i];
        if (r->host_ptr && !(r->flags & MEM_MMIO)) {
            munmap(r->host_ptr, r->size);
        }
    }
    free(mem);
}

/* ─── Internal: add a region ─────────────────────────────────────── */
static FEMUMemRegion *region_alloc(FEMUMemory *mem) {
    if (mem->num_regions >= MEM_MAX_REGIONS) {
        femu_err("Too many memory regions (max %d)", MEM_MAX_REGIONS);
        return NULL;
    }
    return &mem->regions[mem->num_regions++];
}

/* ─── Add RAM ────────────────────────────────────────────────────── */
int mem_add_ram(FEMUMemory *mem, uint64_t base, uint64_t size, const char *name) {
    FEMUMemRegion *r = region_alloc(mem);
    if (!r) return -1;

    size = FEMU_ALIGN(size, FEMU_PAGE_SIZE);
    r->host_ptr = mmap(NULL, size,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (r->host_ptr == MAP_FAILED) {
        femu_err("mem_add_ram: mmap failed for %llu bytes: %s",
                 (unsigned long long)size, strerror(errno));
        mem->num_regions--;
        return -1;
    }

    r->base  = base;
    r->size  = size;
    r->flags = MEM_RAM;
    r->name  = name ? name : "RAM";
    mem->total_ram += size;

    femu_dbg("RAM region '%s': [0x%llx, 0x%llx) host=%p",
             r->name,
             (unsigned long long)base,
             (unsigned long long)(base + size),
             (void*)r->host_ptr);
    return 0;
}

/* ─── Add ROM (read-only, pre-populated) ─────────────────────────── */
int mem_add_rom(FEMUMemory *mem, uint64_t base, const void *data,
                uint64_t size, const char *name) {
    FEMUMemRegion *r = region_alloc(mem);
    if (!r) return -1;

    size = FEMU_ALIGN(size, FEMU_PAGE_SIZE);
    /* Map anonymous, then copy data in, then mprotect read-only */
    r->host_ptr = mmap(NULL, size,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (r->host_ptr == MAP_FAILED) {
        femu_err("mem_add_rom: mmap failed"); mem->num_regions--; return -1;
    }
    if (data) memcpy(r->host_ptr, data, size);
    mprotect(r->host_ptr, size, PROT_READ);

    r->base  = base;
    r->size  = size;
    r->flags = MEM_ROM;
    r->name  = name ? name : "ROM";
    return 0;
}

/* ─── Add MMIO ───────────────────────────────────────────────────── */
int mem_add_mmio(FEMUMemory *mem, uint64_t base, uint64_t size,
                 mmio_read_fn rfn, mmio_write_fn wfn,
                 void *opaque, const char *name) {
    FEMUMemRegion *r = region_alloc(mem);
    if (!r) return -1;

    r->base     = base;
    r->size     = size;
    r->host_ptr = NULL;
    r->flags    = MEM_MMIO;
    r->name     = name ? name : "MMIO";
    r->read_cb  = rfn;
    r->write_cb = wfn;
    r->opaque   = opaque;
    return 0;
}

/* ─── Region lookup ──────────────────────────────────────────────── */
FEMUMemRegion *mem_find_region(FEMUMemory *mem, uint64_t addr) {
    for (int i = 0; i < mem->num_regions; i++) {
        FEMUMemRegion *r = &mem->regions[i];
        if (addr >= r->base && addr < r->base + r->size)
            return r;
    }
    return NULL;
}

/* ─── Read ───────────────────────────────────────────────────────── */
int mem_read(FEMUMemory *mem, uint64_t gpa, void *out, size_t len) {
    /* Handle reads that may span regions */
    uint8_t *dst = (uint8_t*)out;
    size_t   done = 0;
    while (done < len) {
        FEMUMemRegion *r = mem_find_region(mem, gpa + done);
        if (!r) {
            femu_warn("mem_read: unmapped addr 0x%llx",
                      (unsigned long long)(gpa + done));
            return -1;
        }
        size_t avail = (size_t)((r->base + r->size) - (gpa + done));
        size_t chunk = (avail < len - done) ? avail : len - done;

        if (r->flags & MEM_MMIO) {
            /* MMIO: byte-by-byte */
            for (size_t j = 0; j < chunk; j++) {
                if (r->read_cb)
                    dst[done + j] = (uint8_t)r->read_cb(r->opaque, gpa + done + j, 1);
                else
                    dst[done + j] = 0xFF;
            }
        } else {
            uint64_t off = gpa + done - r->base;
            memcpy(dst + done, r->host_ptr + off, chunk);
        }
        done += chunk;
    }
    return 0;
}

/* ─── Write ──────────────────────────────────────────────────────── */
int mem_write(FEMUMemory *mem, uint64_t gpa, const void *data, size_t len) {
    const uint8_t *src = (const uint8_t*)data;
    size_t done = 0;
    while (done < len) {
        FEMUMemRegion *r = mem_find_region(mem, gpa + done);
        if (!r) {
            femu_warn("mem_write: unmapped addr 0x%llx",
                      (unsigned long long)(gpa + done));
            return -1;
        }
        if (r->flags & MEM_ROM) {
            femu_warn("mem_write: attempt to write ROM at 0x%llx",
                      (unsigned long long)(gpa + done));
            return -1;
        }
        size_t avail = (size_t)((r->base + r->size) - (gpa + done));
        size_t chunk = (avail < len - done) ? avail : len - done;

        if (r->flags & MEM_MMIO) {
            for (size_t j = 0; j < chunk; j++)
                if (r->write_cb)
                    r->write_cb(r->opaque, gpa + done + j, src[done + j], 1);
        } else {
            uint64_t off = gpa + done - r->base;
            memcpy(r->host_ptr + off, src + done, chunk);
        }
        done += chunk;
    }
    return 0;
}

/* ─── Typed helpers ──────────────────────────────────────────────── */
#define MEM_READ_TYPED(bits)                                            \
uint##bits##_t mem_read##bits(FEMUMemory *mem, uint64_t gpa) {         \
    uint##bits##_t v = 0;                                               \
    mem_read(mem, gpa, &v, sizeof(v));                                  \
    return v;                                                           \
}
MEM_READ_TYPED(8)
MEM_READ_TYPED(16)
MEM_READ_TYPED(32)
MEM_READ_TYPED(64)

#define MEM_WRITE_TYPED(bits)                                           \
void mem_write##bits(FEMUMemory *mem, uint64_t gpa, uint##bits##_t v) {\
    mem_write(mem, gpa, &v, sizeof(v));                                 \
}
MEM_WRITE_TYPED(8)
MEM_WRITE_TYPED(16)
MEM_WRITE_TYPED(32)
MEM_WRITE_TYPED(64)

/* ─── Host pointer helper ────────────────────────────────────────── */
uint8_t *mem_get_host_ptr(FEMUMemory *mem, uint64_t gpa, size_t len,
                          uint32_t flags) {
    FEMUMemRegion *r = mem_find_region(mem, gpa);
    if (!r) return NULL;
    if (r->flags & MEM_MMIO) return NULL;
    if ((r->flags & flags) != flags) return NULL;
    if (gpa + len > r->base + r->size) return NULL;
    return r->host_ptr + (gpa - r->base);
}

/* ─── Dump ───────────────────────────────────────────────────────── */
void mem_dump_map(const FEMUMemory *mem, FILE *out) {
    fprintf(out, "  %-16s  %-18s  %-18s  %s\n",
            "Name", "Start", "End", "Flags");
    fprintf(out, "  %-16s  %-18s  %-18s  %s\n",
            "----", "-----", "---", "-----");
    for (int i = 0; i < mem->num_regions; i++) {
        const FEMUMemRegion *r = &mem->regions[i];
        char flags[8] = "---";
        if (r->flags & MEM_READ)  flags[0] = 'R';
        if (r->flags & MEM_WRITE) flags[1] = 'W';
        if (r->flags & MEM_EXEC)  flags[2] = 'X';
        if (r->flags & MEM_MMIO) { flags[0]='M'; flags[1]='M'; flags[2]='I'; flags[3]='O'; flags[4]='\0'; }
        fprintf(out, "  %-16s  0x%016llx  0x%016llx  %s\n",
                r->name,
                (unsigned long long)r->base,
                (unsigned long long)(r->base + r->size),
                flags);
    }
    fprintf(out, "  Total RAM: %llu MB\n",
            (unsigned long long)(mem->total_ram / 1024 / 1024));
}
