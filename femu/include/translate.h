/*
 * translate.h — AOT (Ahead-Of-Time) translation pipeline
 *
 * Pipeline:
 *   Guest binary
 *       ↓  (loader)
 *   Raw bytes in memory
 *       ↓  (decoder: arch-specific)
 *   IR (femu Intermediate Representation)
 *       ↓  (ir_opt: optimizer)
 *   Optimised IR
 *       ↓  (codegen: arch-specific)
 *   Host binary blob
 *       ↓  (writer)
 *   Output ELF / raw binary
 */

#ifndef FEMU_TRANSLATE_H
#define FEMU_TRANSLATE_H

#include "femu.h"
#include "ir.h"
#include "memory.h"
#include "cpu.h"

/* ─── Translation context ─────────────────────────────────────────── */
typedef struct FEMUTranslator {
    FEMUConfig   *cfg;
    FEMUMemory   *mem;
    FEMUIRUnit   *unit;

    /* Decode state */
    uint64_t      decode_pc;   /* current decode position           */
    uint64_t      decode_end;  /* end of decode range               */

    /* Code buffer for host binary output */
    uint8_t      *code_buf;
    size_t        code_size;
    size_t        code_cap;

    /* Statistics */
    uint64_t      guest_insns;   /* total guest instructions decoded */
    uint64_t      host_insns;    /* total host instructions emitted  */
    uint64_t      blocks;        /* basic blocks generated           */
} FEMUTranslator;

/* ─── Decoder function type ───────────────────────────────────────── */
/* Each arch implements one of these.
 * Returns number of guest bytes consumed, or -1 on error. */
typedef int (*DecodeFn)(FEMUTranslator *tr, IRBlock *block,
                        uint64_t pc, const uint8_t *bytes, size_t len);

/* ─── Codegen function type ───────────────────────────────────────── */
/* Each target arch implements one of these.
 * Returns FEMU_OK or error. */
typedef int (*CodegenFn)(FEMUTranslator *tr, const FEMUIRUnit *unit);

/* ─── AOT Translator API ──────────────────────────────────────────── */

FEMUTranslator *translator_create(FEMUConfig *cfg, FEMUMemory *mem);
void            translator_destroy(FEMUTranslator *tr);

/* Main entry: translate entire loaded binary AOT */
int  translator_run(FEMUTranslator *tr,
                    uint64_t entry_pc, uint64_t text_end);

/* Write translated code to output file */
int  translator_write_output(FEMUTranslator *tr, const char *path);

/* Print stats */
void translator_print_stats(const FEMUTranslator *tr, FILE *out);

/* Code buffer helpers (used by codegen) */
int  tr_emit_byte(FEMUTranslator *tr, uint8_t b);
int  tr_emit_u16(FEMUTranslator *tr, uint16_t v);
int  tr_emit_u32(FEMUTranslator *tr, uint32_t v);
int  tr_emit_u64(FEMUTranslator *tr, uint64_t v);
int  tr_emit_bytes(FEMUTranslator *tr, const uint8_t *data, size_t len);

/* ─── IR Optimizer API ────────────────────────────────────────────── */
int ir_opt_run(FEMUIRUnit *unit);   /* run all optimization passes   */
int ir_opt_const_fold(IRBlock *b);  /* constant folding              */
int ir_opt_dead_code(IRBlock *b);   /* dead instruction elimination  */
int ir_opt_copy_prop(IRBlock *b);   /* copy propagation              */

/* ─── Architecture decoders (guest → IR) ─────────────────────────── */
int decode_x86_64(FEMUTranslator *tr, IRBlock *b,
                  uint64_t pc, const uint8_t *bytes, size_t len);
int decode_x86   (FEMUTranslator *tr, IRBlock *b,
                  uint64_t pc, const uint8_t *bytes, size_t len);
int decode_arm64 (FEMUTranslator *tr, IRBlock *b,
                  uint64_t pc, const uint8_t *bytes, size_t len);
int decode_arm   (FEMUTranslator *tr, IRBlock *b,
                  uint64_t pc, const uint8_t *bytes, size_t len);
int decode_riscv64(FEMUTranslator *tr, IRBlock *b,
                   uint64_t pc, const uint8_t *bytes, size_t len);
int decode_riscv32(FEMUTranslator *tr, IRBlock *b,
                   uint64_t pc, const uint8_t *bytes, size_t len);

/* ─── Architecture codegens (IR → host binary) ───────────────────── */
int codegen_x86_64(FEMUTranslator *tr, const FEMUIRUnit *unit);
int codegen_arm64 (FEMUTranslator *tr, const FEMUIRUnit *unit);
int codegen_riscv64(FEMUTranslator *tr, const FEMUIRUnit *unit);

/* Lookup correct decoder / codegen for an arch */
DecodeFn   translator_get_decoder(FEMUArch arch);
CodegenFn  translator_get_codegen(FEMUArch arch);

#endif /* FEMU_TRANSLATE_H */
