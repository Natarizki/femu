/*
 * emulate.c — femu-emulate OS / full-system emulation core
 *
 * Implements the emulate_run() API declared in include/emulate.h.
 * This is the layer between the CLI and the actual emulation engine.
 *
 * Currently: AOT-translates the OS image (kernel/ISO) then simulates
 * a boot environment. Full device emulation (VGA, disk controller,
 * NIC) would live here in a production build.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "../include/emulate.h"
#include "../include/memory.h"
#include "../include/loader.h"
#include "../include/translate.h"

/* ─── VGA type map ───────────────────────────────────────────────── */
static const struct { const char *name; FEMUVgaType type; } vga_map[] = {
    { "none",   VGA_NONE   },
    { "std",    VGA_STD    },
    { "vga",    VGA_STD    },
    { "cirrus", VGA_CIRRUS },
    { "vmware", VGA_VMWARE },
    { "virtio", VGA_VIRTIO },
    { "qxl",    VGA_QXL    },
};

FEMUVgaType emulate_vga_from_string(const char *s)
{
    if (!s) return VGA_STD;
    for (size_t i = 0; i < sizeof(vga_map)/sizeof(vga_map[0]); i++)
        if (strcmp(s, vga_map[i].name) == 0) return vga_map[i].type;
    femu_warn("femu-emulate: unknown VGA type '%s', using std", s);
    return VGA_STD;
}

const char *emulate_vga_name(FEMUVgaType v)
{
    for (size_t i = 0; i < sizeof(vga_map)/sizeof(vga_map[0]); i++)
        if (vga_map[i].type == v) return vga_map[i].name;
    return "unknown";
}

FEMUDiskType emulate_disk_type_from_path(const char *path)
{
    if (!path) return DISK_RAW;
    const char *ext = strrchr(path, '.');
    if (!ext) return DISK_RAW;
    if (strcmp(ext, ".iso") == 0)   return DISK_ISO;
    if (strcmp(ext, ".qcow2") == 0) return DISK_QCOW2;
    if (strcmp(ext, ".img") == 0)   return DISK_RAW;
    return DISK_RAW;
}

/* ─── emulate_dump_config ────────────────────────────────────────── */
void emulate_dump_config(const FEMUEmulateConfig *cfg, FILE *f)
{
    fprintf(f, "=== femu-emulate configuration ===\n");
    fprintf(f, "  CPU arch   : %s", femu_arch_name(cfg->cpu_arch));
    if (cfg->cpu_model) fprintf(f, " (%s)", cfg->cpu_model);
    fprintf(f, "\n");
    fprintf(f, "  RAM        : %llu MB\n",
            (unsigned long long)(cfg->ram_size / (1024*1024)));
    fprintf(f, "  vCPUs      : %d\n", cfg->smp_cores);
    fprintf(f, "  VGA        : %s\n", emulate_vga_name(cfg->vga));
    if (cfg->os_image)
        fprintf(f, "  OS image   : %s\n", cfg->os_image);
    for (int i = 0; i < cfg->n_drives; i++) {
        static const char *dtypes[] = { "raw", "qcow2", "iso" };
        fprintf(f, "  drive %-4s : %s [%s]\n",
                cfg->drives[i].id,
                cfg->drives[i].path,
                dtypes[cfg->drives[i].dtype]);
    }
    fprintf(f, "==================================\n");
}

/* ─── emulate_run ────────────────────────────────────────────────── */
int emulate_run(FEMUEmulateConfig *cfg)
{
    emulate_dump_config(cfg, stdout);

    /* Determine primary boot image */
    const char *boot_image = cfg->os_image;
    if (!boot_image) {
        /* Fall back to first drive */
        for (int i = 0; i < cfg->n_drives; i++) {
            if (cfg->drives[i].dtype == DISK_ISO ||
                cfg->drives[i].dtype == DISK_RAW) {
                boot_image = cfg->drives[i].path;
                break;
            }
        }
    }

    if (!boot_image) {
        femu_err("femu-emulate: no boot image specified (-os or -hda)");
        return FEMU_ERR_ARGS;
    }

    femu_log("Booting: %s", boot_image);
    femu_log("VGA    : %s", emulate_vga_name(cfg->vga));

    /* ── Allocate guest memory ── */
    FEMUMemory *mem = mem_create();
    if (!mem) return FEMU_ERR_MEMORY;

    /* RAM base: 0x80000000 for ARM/RISC-V, 0x0 for x86 */
    uint64_t ram_base = 0x0;
    if (cfg->cpu_arch == ARCH_ARM64 || cfg->cpu_arch == ARCH_ARM)
        ram_base = 0x40000000ULL;
    if (cfg->cpu_arch == ARCH_RISCV64 || cfg->cpu_arch == ARCH_RISCV32)
        ram_base = 0x80000000ULL;

    if (mem_add_ram(mem, ram_base, cfg->ram_size, "RAM") < 0) {
        femu_err("femu-emulate: failed to allocate RAM");
        mem_destroy(mem);
        return FEMU_ERR_MEMORY;
    }

    /* Minimal MMIO placeholder regions */
    if (cfg->vga != VGA_NONE) {
        femu_info("VGA MMIO: 0xa0000–0xbffff (placeholder)");
        mem_add_mmio(mem, 0xa0000, 0x20000, NULL, NULL, NULL, "VGA-MMIO");
    }

    /* ── Load boot image ── */
    FEMULoadResult load_res;
    int r = loader_load(mem, boot_image, cfg->cpu_arch, &load_res);
    if (r != FEMU_OK) {
        femu_err("femu-emulate: failed to load '%s'", boot_image);
        mem_destroy(mem); return FEMU_ERR_LOADER;
    }
    if (cfg->cpu_arch == ARCH_NONE) cfg->cpu_arch = load_res.arch;
    loader_dump_info(&load_res, stdout);

    /* ── AOT translate ── */
    FEMUConfig tr_cfg;
    memset(&tr_cfg, 0, sizeof(tr_cfg));
    tr_cfg.guest_arch = cfg->cpu_arch;
    tr_cfg.host_arch  = femu_host_arch();
    tr_cfg.mode       = MODE_SYSTEM;
    tr_cfg.ram_size   = cfg->ram_size;
    tr_cfg.smp_cores  = cfg->smp_cores;
    tr_cfg.verbose    = cfg->verbose;
    tr_cfg.debug      = cfg->debug;
    tr_cfg.input_file = boot_image;

    FEMUTranslator *tr = translator_create(&tr_cfg, mem);
    if (!tr) { mem_destroy(mem); return FEMU_ERR_MEMORY; }

    femu_log("AOT translating kernel/OS image...");
    r = translator_run(tr, load_res.entry_pc, load_res.text_end);
    if (r != FEMU_OK) {
        femu_err("femu-emulate: AOT translation failed");
        goto done;
    }

    translator_print_stats(tr, stdout);

    femu_log("femu-emulate: AOT complete.");
    femu_log("  In a full build, execution would now hand off to");
    femu_log("  the translated code with full device emulation.");
    femu_log("  Drives, VGA (%s), and SMP (%d core%s) are configured.",
             emulate_vga_name(cfg->vga),
             cfg->smp_cores,
             cfg->smp_cores > 1 ? "s" : "");

done:
    translator_destroy(tr);
    mem_destroy(mem);
    return r;
}
