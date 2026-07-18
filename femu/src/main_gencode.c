/*
 * main_gencode.c — femu-compile entry point
 *
 * Reads a .gencode manifest and compiles the connected parser/lexer
 * source files into native binaries for each listed cpu_lang target.
 *
 * Usage:
 *   femu-compile program.gencode
 *   femu-compile -v -dump-ir program.gencode
 *
 * .gencode file format:
 *   connect parser "parser.c"
 *   connect lexer  "lexer.c"
 *
 *   cpu_lang:
 *   aarch64
 *   x86_64
 *   risc-v
 *
 * Flags:
 *   -v          Verbose output
 *   -d          Debug output
 *   -dump-ir    Dump IR before codegen
 *   -version    Print version
 *   -h, -help   This help
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/femu.h"
#include "../include/gencode.h"

/* ─── Global flags ───────────────────────────────────────────────── */
bool femu_verbose = false;
bool femu_debug   = false;

/* ─── Usage ──────────────────────────────────────────────────────── */
static void print_usage(const char *prog)
{
    fprintf(stderr,
        "femu-compile v%s — gencode multi-arch compiler\n\n"
        "Usage:\n"
        "  %s [options] <file.gencode>\n\n"
        "Options:\n"
        "  -v           Verbose output\n"
        "  -d           Debug output (very noisy)\n"
        "  -dump-ir     Dump IR before code generation\n"
        "  -version     Print version and exit\n"
        "  -h, -help    Show this help\n\n"
        "gencode file format:\n"
        "  connect parser  \"parser.c\"    # frontend parser source\n"
        "  connect lexer   \"lexer.c\"     # tokeniser source\n"
        "  connect runtime \"runtime.c\"   # (optional) runtime library\n"
        "\n"
        "  cpu_lang:                      # target architectures\n"
        "  aarch64\n"
        "  x86_64\n"
        "  risc-v\n\n"
        "Outputs:\n"
        "  One native binary per cpu_lang entry:\n"
        "    <basename>.aarch64\n"
        "    <basename>.x86_64\n"
        "    <basename>.risc-v\n\n"
        "Example:\n"
        "  %s -v mylang.gencode\n"
        "    → mylang.aarch64  mylang.x86_64  mylang.risc-v\n",
        FEMU_VERSION_STR, prog, prog);
}

static void print_version(void)
{
    printf("femu-compile %s\n", FEMU_VERSION_STR);
    printf("  Host arch : %s\n", femu_arch_name(femu_host_arch()));
    printf("  Targets   : x86, x86_64, arm, aarch64, riscv32, riscv64\n");
    printf("  Connects  : parser, lexer, codegen, runtime\n");
}

/* ─── main ──────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *input_file = NULL;
    bool verbose  = false;
    bool debug    = false;
    bool dump_ir  = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "-h") == 0 || strcmp(a, "-help") == 0) {
            print_usage(argv[0]); return 0;
        }
        if (strcmp(a, "-version") == 0) { print_version(); return 0; }
        if (strcmp(a, "-v") == 0) { verbose = femu_verbose = true; continue; }
        if (strcmp(a, "-d") == 0) { debug   = femu_debug   = true; continue; }
        if (strcmp(a, "-dump-ir") == 0) { dump_ir = true; continue; }

        if (a[0] != '-') {
            if (input_file) {
                femu_err("Multiple input files specified");
                return 1;
            }
            input_file = a;
            continue;
        }

        femu_err("Unknown option '%s' (try -help)", a);
        return 1;
    }

    if (!input_file) {
        femu_err("No .gencode file specified");
        print_usage(argv[0]);
        return 1;
    }

    femu_log("femu-compile: reading '%s'", input_file);

    /* ── Parse manifest ── */
    GCManifest manifest;
    int r = gencode_parse(input_file, &manifest);
    if (r != FEMU_OK) {
        femu_err("Failed to parse gencode manifest");
        return 1;
    }

    if (verbose) gencode_dump_manifest(&manifest, stdout);

    /* ── Compile ── */
    GCCompileResult result;
    r = gencode_compile(&manifest, &result, verbose, dump_ir);

    gencode_print_results(&result, stdout);

    return (r == FEMU_OK) ? 0 : 1;
}
