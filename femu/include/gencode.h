/*
 * gencode.h — femu gencode language manifest types
 *
 * A .gencode file describes a programming language compiler frontend:
 *
 *   connect parser "parser.c"
 *   connect lexer  "lexer.c"
 *
 *   cpu_lang:
 *   aarch64
 *   x86_64
 *   risc-v
 *
 * femu-compile reads this manifest, links the parser + lexer,
 * and emits a native binary for each listed cpu_lang target.
 */

#ifndef FEMU_GENCODE_H
#define FEMU_GENCODE_H

#include "femu.h"

/* ─── Limits ──────────────────────────────────────────────────────── */
#define GENCODE_MAX_CONNECTS  16   /* max connect directives         */
#define GENCODE_MAX_TARGETS   16   /* max cpu_lang entries           */
#define GENCODE_MAX_PATH      512  /* max file path length           */
#define GENCODE_MAX_LINE      1024 /* max source line                */

/* ─── connect types ──────────────────────────────────────────────── */
typedef enum {
    GC_CONNECT_PARSER  = 0,
    GC_CONNECT_LEXER   = 1,
    GC_CONNECT_CODEGEN = 2,  /* optional: custom codegen backend    */
    GC_CONNECT_RUNTIME = 3,  /* optional: runtime support lib       */
    GC_CONNECT_UNKNOWN = 99,
} GCConnectKind;

typedef struct {
    GCConnectKind kind;
    char          path[GENCODE_MAX_PATH]; /* quoted file path */
} GCConnect;

/* ─── Parsed gencode manifest ─────────────────────────────────────── */
typedef struct {
    /* connect directives */
    GCConnect connects[GENCODE_MAX_CONNECTS];
    int       n_connects;

    /* cpu_lang targets */
    FEMUArch  targets[GENCODE_MAX_TARGETS];
    char      target_names[GENCODE_MAX_TARGETS][64]; /* original strings */
    int       n_targets;

    /* output base name (derived from .gencode filename) */
    char      output_base[GENCODE_MAX_PATH];

    /* source file that was parsed */
    char      source_file[GENCODE_MAX_PATH];
} GCManifest;

/* ─── Compilation result per target ──────────────────────────────── */
typedef struct {
    FEMUArch  arch;
    char      output_file[GENCODE_MAX_PATH];
    bool      success;
    int       error_code;
    size_t    output_bytes;
} GCTargetResult;

typedef struct {
    GCTargetResult results[GENCODE_MAX_TARGETS];
    int            n_results;
    int            n_success;
    int            n_failed;
} GCCompileResult;

/* ─── API ─────────────────────────────────────────────────────────── */

/* Parse a .gencode file and fill in manifest. Returns FEMU_OK on success. */
int  gencode_parse(const char *path, GCManifest *out);

/* Print the parsed manifest to a FILE (for -v / debug). */
void gencode_dump_manifest(const GCManifest *m, FILE *f);

/*
 * Compile using the manifest:
 *   - validate connected files exist
 *   - for each cpu_lang target, run AOT pipeline → write output binary
 * Returns FEMU_OK if all targets succeeded.
 */
int  gencode_compile(const GCManifest *m, GCCompileResult *out,
                     bool verbose, bool dump_ir);

/* Print a compilation summary table. */
void gencode_print_results(const GCCompileResult *r, FILE *f);

/* Map a cpu_lang string to FEMUArch (handles "risc-v" → ARCH_RISCV64). */
FEMUArch gencode_arch_from_string(const char *s);

#endif /* FEMU_GENCODE_H */
