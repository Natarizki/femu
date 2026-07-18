/*
 * gencode.c — .gencode manifest parser and multi-target compiler
 *
 * Parses files like:
 *
 *   connect parser "parser.c"
 *   connect lexer  "lexer.c"
 *
 *   cpu_lang:
 *   aarch64
 *   x86_64
 *   risc-v
 *
 * Then compiles through the femu AOT pipeline for each target.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <libgen.h>   /* basename, dirname */
#include <unistd.h>

#include "../../include/gencode.h"
#include "../../include/translate.h"
#include "../../include/memory.h"
#include "../../include/loader.h"

/* ─── Internal parse state ────────────────────────────────────────── */
typedef enum {
    SECTION_TOP,
    SECTION_CPU_LANG,
} ParseSection;

/* ─── Utility: trim leading + trailing whitespace in-place ──────── */
static char *strtrim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)*(e-1))) e--;
    *e = '\0';
    return s;
}

/* ─── Strip inline comment (# ...) ──────────────────────────────── */
static void strip_comment(char *s)
{
    bool in_quote = false;
    for (char *p = s; *p; p++) {
        if (*p == '"') in_quote = !in_quote;
        if (*p == '#' && !in_quote) { *p = '\0'; break; }
    }
}

/* ─── Extract the string inside double quotes ─────────────────────
 * Input:  word like  "parser.c"
 * Output: parser.c
 * Returns pointer into s (not allocated).
 */
static const char *unquote(char *s)
{
    char *start = strchr(s, '"');
    if (!start) return s;
    start++;
    char *end = strchr(start, '"');
    if (end) *end = '\0';
    return start;
}

/* ─── Connect-kind from keyword ──────────────────────────────────── */
static GCConnectKind connect_kind(const char *kw)
{
    if (strcmp(kw, "parser")  == 0) return GC_CONNECT_PARSER;
    if (strcmp(kw, "lexer")   == 0) return GC_CONNECT_LEXER;
    if (strcmp(kw, "codegen") == 0) return GC_CONNECT_CODEGEN;
    if (strcmp(kw, "runtime") == 0) return GC_CONNECT_RUNTIME;
    return GC_CONNECT_UNKNOWN;
}

static const char *connect_kind_name(GCConnectKind k)
{
    switch (k) {
        case GC_CONNECT_PARSER:  return "parser";
        case GC_CONNECT_LEXER:   return "lexer";
        case GC_CONNECT_CODEGEN: return "codegen";
        case GC_CONNECT_RUNTIME: return "runtime";
        default:                  return "unknown";
    }
}

/* ─── cpu_lang string → FEMUArch ─────────────────────────────────── */
FEMUArch gencode_arch_from_string(const char *s)
{
    if (!s) return ARCH_NONE;
    /* normalise: lower-case compare */
    if (strcmp(s, "x86")      == 0) return ARCH_X86;
    if (strcmp(s, "x86_64")   == 0) return ARCH_X86_64;
    if (strcmp(s, "x86-64")   == 0) return ARCH_X86_64;
    if (strcmp(s, "amd64")    == 0) return ARCH_X86_64;
    if (strcmp(s, "arm")      == 0) return ARCH_ARM;
    if (strcmp(s, "arm64")    == 0) return ARCH_ARM64;
    if (strcmp(s, "aarch64")  == 0) return ARCH_ARM64;
    if (strcmp(s, "riscv32")  == 0) return ARCH_RISCV32;
    if (strcmp(s, "riscv64")  == 0) return ARCH_RISCV64;
    if (strcmp(s, "risc-v")   == 0) return ARCH_RISCV64; /* generic → 64 */
    if (strcmp(s, "riscv")    == 0) return ARCH_RISCV64;
    return ARCH_NONE;
}

/* ─── Output filename for a given target ─────────────────────────── */
static void build_output_name(char *buf, size_t sz,
                               const char *base, const char *arch_str)
{
    snprintf(buf, sz, "%s.%s", base, arch_str);
}

/* ─── gencode_parse ──────────────────────────────────────────────── */
int gencode_parse(const char *path, GCManifest *out)
{
    if (!path || !out) return FEMU_ERR_ARGS;
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "r");
    if (!f) {
        femu_err("gencode: cannot open '%s': %s", path, strerror(errno));
        return FEMU_ERR_IO;
    }

    /* Store source path and derive output base (strip .gencode extension) */
    strncpy(out->source_file, path, GENCODE_MAX_PATH - 1);

    /* Copy path for basename manipulation */
    char path_copy[GENCODE_MAX_PATH];
    strncpy(path_copy, path, GENCODE_MAX_PATH - 1);
    const char *base = basename(path_copy);
    strncpy(out->output_base, base, GENCODE_MAX_PATH - 1);
    /* Strip .gencode suffix */
    char *dot = strrchr(out->output_base, '.');
    if (dot && strcmp(dot, ".gencode") == 0) *dot = '\0';

    char line[GENCODE_MAX_LINE];
    int  lineno      = 0;
    ParseSection sec = SECTION_TOP;
    int  ret         = FEMU_OK;

    while (fgets(line, sizeof(line), f)) {
        lineno++;
        strip_comment(line);
        char *s = strtrim(line);
        if (*s == '\0') continue; /* blank / comment-only line */

        /* ── Section header ── */
        if (strcmp(s, "cpu_lang:") == 0) {
            sec = SECTION_CPU_LANG;
            continue;
        }

        if (sec == SECTION_CPU_LANG) {
            /* Each non-blank line is an architecture name */
            if (out->n_targets >= GENCODE_MAX_TARGETS) {
                femu_warn("gencode:%d: too many cpu_lang entries (max %d), ignoring '%s'",
                          lineno, GENCODE_MAX_TARGETS, s);
                continue;
            }
            FEMUArch arch = gencode_arch_from_string(s);
            if (arch == ARCH_NONE) {
                femu_warn("gencode:%d: unknown cpu_lang '%s'", lineno, s);
            }
            int idx = out->n_targets++;
            out->targets[idx] = arch;
            strncpy(out->target_names[idx], s, 63);
            continue;
        }

        /* ── SECTION_TOP: connect directive ── */
        if (strncmp(s, "connect", 7) == 0 && isspace((unsigned char)s[7])) {
            char *rest = strtrim(s + 7);
            /* next word is the connect kind */
            char kw[64] = {0};
            char *p = rest;
            int  ki = 0;
            while (*p && !isspace((unsigned char)*p) && ki < 63) kw[ki++] = *p++;
            kw[ki] = '\0';
            rest = strtrim(p);

            if (out->n_connects >= GENCODE_MAX_CONNECTS) {
                femu_warn("gencode:%d: too many connect directives (max %d)",
                          lineno, GENCODE_MAX_CONNECTS);
                continue;
            }

            GCConnectKind kind = connect_kind(kw);
            if (kind == GC_CONNECT_UNKNOWN) {
                femu_warn("gencode:%d: unknown connect kind '%s'", lineno, kw);
            }
            const char *file_path = unquote(rest);
            int idx = out->n_connects++;
            out->connects[idx].kind = kind;
            strncpy(out->connects[idx].path, file_path, GENCODE_MAX_PATH - 1);
            continue;
        }

        femu_warn("gencode:%d: unrecognised directive: '%s'", lineno, s);
    }

    fclose(f);

    /* Validation */
    if (out->n_targets == 0) {
        femu_err("gencode: no cpu_lang targets defined in '%s'", path);
        ret = FEMU_ERR_ARGS;
    }

    return ret;
}

/* ─── gencode_dump_manifest ──────────────────────────────────────── */
void gencode_dump_manifest(const GCManifest *m, FILE *f)
{
    fprintf(f, "=== gencode manifest: %s ===\n", m->source_file);
    fprintf(f, "  output base : %s\n", m->output_base);
    fprintf(f, "  connects    : %d\n", m->n_connects);
    for (int i = 0; i < m->n_connects; i++) {
        fprintf(f, "    [%d] %-10s → \"%s\"\n", i,
                connect_kind_name(m->connects[i].kind),
                m->connects[i].path);
    }
    fprintf(f, "  cpu_lang    : %d target(s)\n", m->n_targets);
    for (int i = 0; i < m->n_targets; i++) {
        fprintf(f, "    [%d] %-12s (arch id %d)\n", i,
                m->target_names[i], (int)m->targets[i]);
    }
    fprintf(f, "===================\n");
}

/* ─── Validate that all connected files exist and are readable ───── */
static int validate_connects(const GCManifest *m)
{
    bool has_parser = false, has_lexer = false;

    for (int i = 0; i < m->n_connects; i++) {
        const GCConnect *c = &m->connects[i];
        if (access(c->path, R_OK) != 0) {
            femu_err("gencode: connect %s — file not found: '%s'",
                     connect_kind_name(c->kind), c->path);
            return FEMU_ERR_IO;
        }
        if (c->kind == GC_CONNECT_PARSER) has_parser = true;
        if (c->kind == GC_CONNECT_LEXER)  has_lexer  = true;
    }

    if (!has_parser) femu_warn("gencode: no 'connect parser' directive found");
    if (!has_lexer)  femu_warn("gencode: no 'connect lexer' directive found");

    return FEMU_OK;
}

/*
 * gencode_compile
 *
 * For each cpu_lang target, we create a small stub ELF in memory that
 * references the connect'd parser + lexer, run the AOT translator, and
 * write the native binary output.
 *
 * Since this is a bootstrap (the parser/lexer are C source files, not yet
 * compiled), we use the femu AOT pipeline in "stub compile" mode:
 *  1. Compile parser.c + lexer.c to a temporary object via the host compiler.
 *  2. Link into a temporary ELF.
 *  3. AOT-translate that ELF to the target arch.
 *  4. Write <output_base>.<target> binary.
 */
int gencode_compile(const GCManifest *m, GCCompileResult *out,
                    bool verbose, bool dump_ir)
{
    if (!m || !out) return FEMU_ERR_ARGS;
    memset(out, 0, sizeof(*out));

    int r = validate_connects(m);
    if (r != FEMU_OK) return r;

    femu_log("gencode: compiling '%s' → %d target(s)",
             m->output_base, m->n_targets);

    for (int ti = 0; ti < m->n_targets; ti++) {
        GCTargetResult *res = &out->results[ti];
        res->arch = m->targets[ti];

        const char *arch_str = m->target_names[ti];
        build_output_name(res->output_file, GENCODE_MAX_PATH,
                          m->output_base, arch_str);

        femu_log("  [%d/%d] target: %-12s → %s",
                 ti + 1, m->n_targets, arch_str, res->output_file);

        if (m->targets[ti] == ARCH_NONE) {
            femu_warn("    skipping unknown arch '%s'", arch_str);
            res->success    = false;
            res->error_code = FEMU_ERR_ARCH;
            out->n_results++;
            out->n_failed++;
            continue;
        }

        /* ── Step 1: compile parser + lexer with the host gcc ── */
        /* Build the compiler command */
        char cc_cmd[4096];
        int cc_len = 0;
        cc_len += snprintf(cc_cmd + cc_len, sizeof(cc_cmd) - (size_t)cc_len,
                           "gcc -std=c11 -O2 -o /tmp/femu_gencode_stub_%d.o",
                           ti);

        for (int ci = 0; ci < m->n_connects; ci++) {
            cc_len += snprintf(cc_cmd + cc_len, sizeof(cc_cmd) - (size_t)cc_len,
                               " \"%s\"", m->connects[ci].path);
        }

        if (verbose)
            femu_log("    cc: %s", cc_cmd);

        int cc_ret = system(cc_cmd);
        if (cc_ret != 0) {
            femu_err("    host compile failed (exit %d)", cc_ret);
            res->success    = false;
            res->error_code = FEMU_ERR_CODEGEN;
            out->n_results++;
            out->n_failed++;
            continue;
        }

        /* ── Step 2: AOT-translate the stub to the target arch ── */
        char stub_path[256];
        snprintf(stub_path, sizeof(stub_path),
                 "/tmp/femu_gencode_stub_%d.o", ti);

        FEMUConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.guest_arch  = femu_host_arch();   /* stub was compiled for host */
        cfg.host_arch   = m->targets[ti];     /* cross-compile to target    */
        cfg.mode        = MODE_TRANSLATE;
        cfg.ram_size    = FEMU_MB(256);
        cfg.smp_cores   = 1;
        cfg.verbose     = verbose;
        cfg.debug       = false;
        cfg.dump_ir     = dump_ir;
        cfg.input_file  = stub_path;
        cfg.output_file = res->output_file;

        FEMUMemory *mem = mem_create();
        if (!mem) {
            res->success = false; res->error_code = FEMU_ERR_MEMORY;
            out->n_results++; out->n_failed++;
            continue;
        }
        if (mem_add_ram(mem, 0x0, FEMU_GB(2), "workspace") < 0) {
            mem_destroy(mem);
            res->success = false; res->error_code = FEMU_ERR_MEMORY;
            out->n_results++; out->n_failed++;
            continue;
        }

        FEMULoadResult load_res;
        r = loader_load(mem, stub_path, cfg.guest_arch, &load_res);
        if (r != FEMU_OK) {
            femu_err("    loader failed for stub '%s'", stub_path);
            mem_destroy(mem);
            res->success = false; res->error_code = FEMU_ERR_LOADER;
            out->n_results++; out->n_failed++;
            continue;
        }

        FEMUTranslator *tr = translator_create(&cfg, mem);
        if (!tr) {
            mem_destroy(mem);
            res->success = false; res->error_code = FEMU_ERR_MEMORY;
            out->n_results++; out->n_failed++;
            continue;
        }

        r = translator_run(tr, load_res.entry_pc, load_res.text_end);
        if (r == FEMU_OK) {
            if (dump_ir) ir_dump_unit(tr->unit, stdout);
            r = translator_write_output(tr, res->output_file);
        }

        res->success     = (r == FEMU_OK);
        res->error_code  = r;
        res->output_bytes = tr->code_size;

        if (res->success) {
            femu_log("    ✓  %s  (%zu bytes)", res->output_file, res->output_bytes);
            out->n_success++;
        } else {
            femu_err("    ✗  target %s failed (err %d)", arch_str, r);
            out->n_failed++;
        }

        translator_destroy(tr);
        mem_destroy(mem);
        /* Remove temporary stub */
        remove(stub_path);

        out->n_results++;
    }

    return (out->n_failed == 0) ? FEMU_OK : FEMU_ERR_CODEGEN;
}

/* ─── gencode_print_results ──────────────────────────────────────── */
void gencode_print_results(const GCCompileResult *r, FILE *f)
{
    fprintf(f, "\n=== femu-compile results ===\n");
    fprintf(f, "  %-20s  %-10s  %s\n", "target", "status", "output");
    fprintf(f, "  %-20s  %-10s  %s\n",
            "──────────────────", "──────────", "──────────────────────");
    for (int i = 0; i < r->n_results; i++) {
        const GCTargetResult *t = &r->results[i];
        fprintf(f, "  %-20s  %-10s  %s",
                femu_arch_name(t->arch),
                t->success ? "OK" : "FAILED",
                t->success ? t->output_file : "–");
        if (t->success && t->output_bytes)
            fprintf(f, "  (%zu bytes)", t->output_bytes);
        fprintf(f, "\n");
    }
    fprintf(f, "\n  %d/%d targets compiled successfully.\n",
            r->n_success, r->n_results);
    fprintf(f, "============================\n");
}
