/*
 * main_emulate.c — femu-emulate entry point
 *
 * Usage:
 *   femu-emulate -os ubuntu.iso -hda disk.qcow2 -vga virtio -cpu aarch64
 *   femu-emulate -cpu x86_64 -m 512M -smp 2 -hda win11.qcow2 -vga std
 *
 * Flags
 * ─────
 *   -os  <file>       Boot OS image (ISO, kernel binary, raw)
 *   -hda <file>       Primary hard-disk image (qcow2, raw)
 *   -hdb <file>       Secondary hard-disk image
 *   -vga <type>       Display adapter: none | std | vga | cirrus | virtio | qxl
 *   -cpu <arch>       Guest CPU: x86_64 | aarch64 | risc-v | arm | x86
 *   -m   <size>       RAM: 128M (default), 512M, 1G, …
 *   -smp <n>          Number of vCPUs (default: 1)
 *   -nographic        Disable graphical window
 *   -no-reboot        Exit instead of rebooting
 *   -v                Verbose
 *   -d                Debug
 *   -version          Print version
 *   -h, -help         This help
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "../include/femu.h"
#include "../include/emulate.h"
#include "../include/gencode.h"
#include "../include/loader.h"

/* ─── Global flags (used by femu.h logging macros) ──────────────── */
bool femu_verbose = false;
bool femu_debug   = false;

/* ─── Usage ──────────────────────────────────────────────────────── */
static void print_usage(const char *prog)
{
    fprintf(stderr,
        "femu-emulate v%s — OS / system emulation\n\n"
        "Usage:\n"
        "  %s [options]\n\n"
        "Storage:\n"
        "  -os  <file>    Boot OS image (ISO / kernel / raw binary)\n"
        "  -hda <file>    Primary hard disk (qcow2 or raw)\n"
        "  -hdb <file>    Secondary hard disk\n\n"
        "Machine:\n"
        "  -cpu <arch>    Guest CPU architecture\n"
        "                 Supported: x86_64  aarch64  risc-v\n"
        "                            x86     arm\n"
        "  -m   <size>    RAM size  (e.g. 128M, 512M, 1G)  [default: 128M]\n"
        "  -smp <n>       Number of vCPUs                  [default: 1]\n\n"
        "Display:\n"
        "  -vga <type>    Display adapter\n"
        "                 none | std | vga | cirrus | virtio | qxl\n"
        "                 [default: std]\n"
        "  -nographic     Disable graphical window\n\n"
        "Misc:\n"
        "  -no-reboot     Exit instead of rebooting\n"
        "  -v             Verbose output\n"
        "  -d             Debug output\n"
        "  -version       Print version and exit\n"
        "  -h, -help      Show this help\n\n"
        "Examples:\n"
        "  %s -cpu aarch64 -os ubuntu-22.04-arm.iso -m 2G -vga virtio\n"
        "  %s -cpu x86_64  -hda win11.qcow2 -m 4G -smp 4 -vga std\n"
        "  %s -cpu risc-v  -os riscv-linux.iso -m 512M\n",
        FEMU_VERSION_STR, prog, prog, prog, prog);
}

static void print_version(void)
{
    printf("femu-emulate %s\n", FEMU_VERSION_STR);
    printf("  Host arch : %s\n", femu_arch_name(femu_host_arch()));
    printf("  Guests    : x86, x86_64, arm, aarch64, risc-v\n");
    printf("  VGA types : none, std, vga, cirrus, virtio, qxl\n");
}

/* ─── Parse memory size string ──────────────────────────────────── */
static uint64_t parse_mem(const char *s)
{
    char *end;
    uint64_t val = strtoull(s, &end, 10);
    if (!end || !*end) return val;
    switch (*end) {
        case 'k': case 'K': return val << 10;
        case 'm': case 'M': return val << 20;
        case 'g': case 'G': return val << 30;
        default:
            femu_err("Unknown memory suffix '%c'", *end);
            exit(1);
    }
}

/* ─── Add a drive to config ─────────────────────────────────────── */
static void add_drive(FEMUEmulateConfig *cfg,
                      const char *path, const char *id)
{
    if (cfg->n_drives >= EMULATE_MAX_DRIVES) {
        femu_warn("Too many drives, ignoring '%s'", path);
        return;
    }
    int i = cfg->n_drives++;
    cfg->drives[i].path   = path;
    cfg->drives[i].dtype  = emulate_disk_type_from_path(path);
    cfg->drives[i].readonly = (cfg->drives[i].dtype == DISK_ISO);
    strncpy(cfg->drives[i].id, id, 7);
}

/* ─── main ──────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    FEMUEmulateConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.ram_size  = FEMU_MB(128);
    cfg.smp_cores = 1;
    cfg.vga       = VGA_STD;
    cfg.cpu_arch  = ARCH_NONE;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

#define NEXT_ARG(flag) \
    (++i < argc ? argv[i] : (femu_err(flag " requires an argument"), exit(1), (char*)NULL))

        if (strcmp(a, "-h") == 0 || strcmp(a, "-help") == 0) {
            print_usage(argv[0]); return 0;
        }
        if (strcmp(a, "-version") == 0) { print_version(); return 0; }
        if (strcmp(a, "-v") == 0) { cfg.verbose = femu_verbose = true; continue; }
        if (strcmp(a, "-d") == 0) { cfg.debug   = femu_debug   = true; continue; }
        if (strcmp(a, "-nographic") == 0) { cfg.nographic = true; cfg.vga = VGA_NONE; continue; }
        if (strcmp(a, "-no-reboot") == 0) { cfg.no_reboot = true; continue; }

        if (strcmp(a, "-os") == 0) {
            cfg.os_image = NEXT_ARG("-os");
            add_drive(&cfg, cfg.os_image, "cdrom");
            continue;
        }
        if (strcmp(a, "-hda") == 0) {
            add_drive(&cfg, NEXT_ARG("-hda"), "hda");
            continue;
        }
        if (strcmp(a, "-hdb") == 0) {
            add_drive(&cfg, NEXT_ARG("-hdb"), "hdb");
            continue;
        }
        if (strcmp(a, "-vga") == 0) {
            const char *vga_str = NEXT_ARG("-vga");
            cfg.vga_name = vga_str;
            cfg.vga = emulate_vga_from_string(vga_str);
            continue;
        }
        if (strcmp(a, "-cpu") == 0) {
            const char *cpu_str = NEXT_ARG("-cpu");
            cfg.cpu_model = cpu_str;
            /* map cpu_lang-style strings too (risc-v → riscv64) */
            cfg.cpu_arch = gencode_arch_from_string(cpu_str);
            if (cfg.cpu_arch == ARCH_NONE)
                cfg.cpu_arch = femu_arch_from_string(cpu_str);
            if (cfg.cpu_arch == ARCH_NONE) {
                femu_err("Unknown CPU arch '%s'", cpu_str);
                return 1;
            }
            continue;
        }
        if (strcmp(a, "-m") == 0) {
            cfg.ram_size = parse_mem(NEXT_ARG("-m"));
            continue;
        }
        if (strcmp(a, "-smp") == 0) {
            cfg.smp_cores = atoi(NEXT_ARG("-smp"));
            continue;
        }

        femu_err("Unknown option '%s' (try -help)", a);
        return 1;

#undef NEXT_ARG
    }

    /* Validate: need at least one boot image */
    if (!cfg.os_image && cfg.n_drives == 0) {
        femu_err("No boot image specified. Use -os <file> or -hda <file>.");
        print_usage(argv[0]);
        return 1;
    }

    /* Auto-detect CPU arch from image if not given */
    if (cfg.cpu_arch == ARCH_NONE) {
        const char *img = cfg.os_image ? cfg.os_image
                        : cfg.drives[0].path;
        cfg.cpu_arch = loader_detect_arch(img);
        if (cfg.cpu_arch == ARCH_NONE) {
            femu_warn("Could not detect CPU arch; defaulting to x86_64");
            cfg.cpu_arch = ARCH_X86_64;
        } else {
            femu_info("Auto-detected CPU arch: %s", femu_arch_name(cfg.cpu_arch));
        }
    }

    return emulate_run(&cfg) == FEMU_OK ? 0 : 1;
}
