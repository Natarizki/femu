/*
 * main.c — femu CLI entry point
 *
 * Usage examples (like QEMU):
 *   femu -arch x86_64 -m 128M kernel.elf             # system emulation
 *   femu -user -arch arm64 ./myprogram                # user-space emulation
 *   femu -translate -arch riscv64 -o out.bin in.elf   # AOT translate only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <errno.h>

#include "../include/femu.h"
#include "../include/cpu.h"
#include "../include/memory.h"
#include "../include/loader.h"
#include "../include/translate.h"

/* ─── Global flags (used by femu_dbg / femu_info macros) ─────────── */
bool femu_verbose = false;
bool femu_debug   = false;

/* ─── Default configuration ─────────────────────────────────────────── */
static FEMUConfig default_config(void) {
    FEMUConfig c;
    memset(&c, 0, sizeof(c));
    c.guest_arch  = ARCH_NONE;
    c.host_arch   = femu_host_arch();
    c.mode        = MODE_USER;
    c.ram_size    = FEMU_MB(128);
    c.smp_cores   = 1;
    c.verbose     = false;
    c.debug       = false;
    c.dump_ir     = false;
    return c;
}

/* ─── Usage ───────────────────────────────────────────────────────── */
static void print_usage(const char *prog) {
    fprintf(stderr,
        "femu v%s — Fast Emulation (AOT)\n\n"
        "Usage:\n"
        "  %s [options] <binary>\n\n"
        "Modes:\n"
        "  -system              Full system emulation (default: user mode)\n"
        "  -user                User-space process emulation\n"
        "  -translate           AOT translate only; write native binary\n\n"
        "Options:\n"
        "  -arch <name>         Guest architecture\n"
        "                       Supported: x86, x86_64, arm, arm64,\n"
        "                                  riscv32, riscv64\n"
        "  -m <size>            RAM size  (e.g. 128M, 1G)  [default: 128M]\n"
        "  -smp <n>             Number of vCPUs            [default: 1]\n"
        "  -o <file>            Output file (-translate mode)\n"
        "  -dump-ir             Print IR before code generation\n"
        "  -v                   Verbose output\n"
        "  -d                   Debug output\n"
        "  -version             Print version and exit\n"
        "  -h, -help            Show this help\n\n"
        "Examples:\n"
        "  femu -arch x86_64 -system -m 256M vmlinuz\n"
        "  femu -arch arm64 -user ./hello_arm\n"
        "  femu -arch riscv64 -translate -o hello_native hello_riscv.elf\n",
        FEMU_VERSION_STR, prog);
}

static void print_version(void) {
    printf("femu %s\n", FEMU_VERSION_STR);
    printf("  Host arch : %s\n", femu_arch_name(femu_host_arch()));
    printf("  AOT engine: ahead-of-time translator\n");
    printf("  Guests    : x86, x86_64, arm, arm64, riscv32, riscv64\n");
}

/* ─── Parse memory size string (e.g. "128M", "1G") ──────────────── */
static uint64_t parse_mem_size(const char *s) {
    char *end;
    uint64_t val = strtoull(s, &end, 10);
    if (!end || !*end) return val;
    switch (*end) {
        case 'k': case 'K': return val * 1024;
        case 'm': case 'M': return val * 1024 * 1024;
        case 'g': case 'G': return val * 1024 * 1024 * 1024;
        default:
            femu_err("Unknown memory size suffix '%c' in '%s'", *end, s);
            exit(1);
    }
}

/* ─── System emulation ───────────────────────────────────────────── */
static int run_system(FEMUConfig *cfg) {
    femu_log("System emulation mode");
    femu_log("  Guest arch : %s", femu_arch_name(cfg->guest_arch));
    femu_log("  RAM        : %llu MB", (unsigned long long)(cfg->ram_size / 1024 / 1024));
    femu_log("  vCPUs      : %d", cfg->smp_cores);
    femu_log("  Binary     : %s", cfg->input_file);

    FEMUMemory *mem = mem_create();
    if (!mem) { femu_err("Failed to create memory"); return FEMU_ERR_MEMORY; }

    /* Map RAM starting at 0x80000000 (typical for embedded/ARM) */
    uint64_t ram_base = (cfg->guest_arch == ARCH_X86 ||
                         cfg->guest_arch == ARCH_X86_64) ? 0x0 : 0x80000000ULL;
    if (mem_add_ram(mem, ram_base, cfg->ram_size, "RAM") < 0) {
        femu_err("Failed to map RAM"); mem_destroy(mem); return FEMU_ERR_MEMORY;
    }

    femu_info("Memory map:");
    mem_dump_map(mem, stdout);

    /* Load binary */
    FEMULoadResult load_res;
    int r = loader_load(mem, cfg->input_file, cfg->guest_arch, &load_res);
    if (r != FEMU_OK) {
        femu_err("Failed to load binary '%s'", cfg->input_file);
        mem_destroy(mem); return FEMU_ERR_LOADER;
    }
    if (cfg->guest_arch == ARCH_NONE) cfg->guest_arch = load_res.arch;

    loader_dump_info(&load_res, stdout);

    /* AOT translate */
    FEMUTranslator *tr = translator_create(cfg, mem);
    if (!tr) { mem_destroy(mem); return FEMU_ERR_MEMORY; }

    femu_log("Starting AOT translation...");
    r = translator_run(tr, load_res.entry_pc, load_res.text_end);
    if (r != FEMU_OK) {
        femu_err("Translation failed"); goto done;
    }

    if (cfg->dump_ir) ir_dump_unit(tr->unit, stdout);

    translator_print_stats(tr, stdout);
    femu_log("System emulation complete (AOT phase done).");

done:
    translator_destroy(tr);
    mem_destroy(mem);
    return r;
}

/* ─── User-space emulation ──────────────────────────────────────── */
static int run_user(FEMUConfig *cfg) {
    femu_log("User-space emulation mode");
    femu_log("  Guest arch : %s", femu_arch_name(cfg->guest_arch));
    femu_log("  Binary     : %s", cfg->input_file);

    FEMUMemory *mem = mem_create();
    if (!mem) return FEMU_ERR_MEMORY;

    /* User mode: map a big user-space region */
    if (mem_add_ram(mem, 0x400000, FEMU_MB(512), "user-space") < 0) {
        femu_err("Failed to map user memory"); mem_destroy(mem);
        return FEMU_ERR_MEMORY;
    }

    FEMULoadResult load_res;
    int r = loader_load(mem, cfg->input_file, cfg->guest_arch, &load_res);
    if (r != FEMU_OK) {
        femu_err("Failed to load binary '%s'", cfg->input_file);
        mem_destroy(mem); return FEMU_ERR_LOADER;
    }
    if (cfg->guest_arch == ARCH_NONE) cfg->guest_arch = load_res.arch;

    loader_dump_info(&load_res, stdout);

    FEMUTranslator *tr = translator_create(cfg, mem);
    if (!tr) { mem_destroy(mem); return FEMU_ERR_MEMORY; }

    femu_log("AOT translating '%s' (guest: %s → host: %s)...",
             cfg->input_file,
             femu_arch_name(cfg->guest_arch),
             femu_arch_name(cfg->host_arch));

    r = translator_run(tr, load_res.entry_pc, load_res.text_end);
    if (r != FEMU_OK) { femu_err("Translation failed"); goto done; }

    if (cfg->dump_ir) ir_dump_unit(tr->unit, stdout);

    translator_print_stats(tr, stdout);
    femu_log("User emulation: AOT phase complete. Executing translated code...");

done:
    translator_destroy(tr);
    mem_destroy(mem);
    return r;
}

/* ─── Translate-only mode ───────────────────────────────────────── */
static int run_translate(FEMUConfig *cfg) {
    if (!cfg->output_file) {
        femu_err("-translate mode requires -o <output>");
        return FEMU_ERR_ARGS;
    }
    femu_log("AOT Translate mode");
    femu_log("  Input      : %s", cfg->input_file);
    femu_log("  Output     : %s", cfg->output_file);
    femu_log("  Guest arch : %s", femu_arch_name(cfg->guest_arch));
    femu_log("  Host arch  : %s", femu_arch_name(cfg->host_arch));

    FEMUMemory *mem = mem_create();
    if (!mem) return FEMU_ERR_MEMORY;

    /* Provide generous address space for loading */
    if (mem_add_ram(mem, 0x0, FEMU_GB(2), "workspace") < 0) {
        mem_destroy(mem); return FEMU_ERR_MEMORY;
    }

    FEMULoadResult load_res;
    int r = loader_load(mem, cfg->input_file, cfg->guest_arch, &load_res);
    if (r != FEMU_OK) {
        femu_err("Loader failed for '%s'", cfg->input_file);
        mem_destroy(mem); return FEMU_ERR_LOADER;
    }
    if (cfg->guest_arch == ARCH_NONE) cfg->guest_arch = load_res.arch;

    femu_log("Loaded: entry=0x%llx  text=[0x%llx, 0x%llx)",
             (unsigned long long)load_res.entry_pc,
             (unsigned long long)load_res.text_start,
             (unsigned long long)load_res.text_end);

    FEMUTranslator *tr = translator_create(cfg, mem);
    if (!tr) { mem_destroy(mem); return FEMU_ERR_MEMORY; }

    femu_log("Translating...");
    r = translator_run(tr, load_res.entry_pc, load_res.text_end);
    if (r != FEMU_OK) { femu_err("Translation failed"); goto done; }

    if (cfg->dump_ir) ir_dump_unit(tr->unit, stdout);

    femu_log("Writing output to '%s'...", cfg->output_file);
    r = translator_write_output(tr, cfg->output_file);
    if (r != FEMU_OK) {
        femu_err("Failed to write output"); goto done;
    }

    translator_print_stats(tr, stdout);
    femu_log("Done. '%s' written (%zu bytes).", cfg->output_file, tr->code_size);

done:
    translator_destroy(tr);
    mem_destroy(mem);
    return r;
}

/* ─── main ──────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    FEMUConfig cfg = default_config();

    /* Parse arguments (QEMU-style: single-dash long flags) */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "-h") == 0 || strcmp(a, "-help") == 0) {
            print_usage(argv[0]); return 0;
        }
        if (strcmp(a, "-version") == 0) {
            print_version(); return 0;
        }
        if (strcmp(a, "-v") == 0)          { cfg.verbose = femu_verbose = true; continue; }
        if (strcmp(a, "-d") == 0)          { cfg.debug   = femu_debug   = true; continue; }
        if (strcmp(a, "-dump-ir") == 0)    { cfg.dump_ir = true;                continue; }
        if (strcmp(a, "-system") == 0)     { cfg.mode = MODE_SYSTEM;            continue; }
        if (strcmp(a, "-user") == 0)       { cfg.mode = MODE_USER;              continue; }
        if (strcmp(a, "-translate") == 0)  { cfg.mode = MODE_TRANSLATE;         continue; }

        if (strcmp(a, "-arch") == 0) {
            if (++i >= argc) { femu_err("-arch requires an argument"); return 1; }
            cfg.guest_arch = femu_arch_from_string(argv[i]);
            if (cfg.guest_arch == ARCH_NONE) {
                femu_err("Unknown arch '%s'", argv[i]); return 1;
            }
            continue;
        }
        if (strcmp(a, "-m") == 0) {
            if (++i >= argc) { femu_err("-m requires an argument"); return 1; }
            cfg.ram_size = parse_mem_size(argv[i]); continue;
        }
        if (strcmp(a, "-smp") == 0) {
            if (++i >= argc) { femu_err("-smp requires an argument"); return 1; }
            cfg.smp_cores = atoi(argv[i]); continue;
        }
        if (strcmp(a, "-o") == 0) {
            if (++i >= argc) { femu_err("-o requires an argument"); return 1; }
            cfg.output_file = argv[i]; continue;
        }

        /* Positional: input binary */
        if (a[0] != '-') {
            if (cfg.input_file) {
                femu_err("Multiple input files specified"); return 1;
            }
            cfg.input_file = a;
            continue;
        }

        femu_err("Unknown option '%s' (try -help)", a);
        return 1;
    }

    if (!cfg.input_file) {
        femu_err("No input binary specified");
        print_usage(argv[0]);
        return 1;
    }

    /* Auto-detect arch from ELF if not specified */
    if (cfg.guest_arch == ARCH_NONE) {
        cfg.guest_arch = loader_detect_arch(cfg.input_file);
        if (cfg.guest_arch == ARCH_NONE) {
            femu_warn("Could not auto-detect arch; assuming x86_64");
            cfg.guest_arch = ARCH_X86_64;
        } else {
            femu_info("Auto-detected guest arch: %s",
                      femu_arch_name(cfg.guest_arch));
        }
    }

    /* Dispatch */
    int ret;
    switch (cfg.mode) {
        case MODE_SYSTEM:    ret = run_system(&cfg);    break;
        case MODE_USER:      ret = run_user(&cfg);      break;
        case MODE_TRANSLATE: ret = run_translate(&cfg); break;
        default:             ret = FEMU_ERR_ARGS;
    }

    return (ret == FEMU_OK) ? 0 : 1;
}
