/*
 * ir.h — femu Intermediate Representation (IR)
 *
 * A simple, flat 3-address IR used as the bridge between
 * guest decode and host code generation in the AOT pipeline.
 *
 *   Guest binary → Decoder → IR → Optimizer → Codegen → Host binary
 */

#ifndef FEMU_IR_H
#define FEMU_IR_H

#include "femu.h"

/* ─── IR virtual register ─────────────────────────────────────────── */
/* Each decoded guest register maps to an IR virtual register (vreg).
 * vregs are SSA-like temporaries; the codegen allocates real regs. */
typedef uint32_t IRReg;
#define IR_REG_INVALID  UINT32_MAX
#define IR_REG_CONST    (UINT32_MAX - 1) /* operand is immediate */

/* ─── IR opcodes ──────────────────────────────────────────────────── */
typedef enum {
    /* Data movement */
    IR_MOV,        /* dst = src                               */
    IR_MOVI,       /* dst = imm                               */

    /* Arithmetic */
    IR_ADD,        /* dst = a + b                             */
    IR_ADDI,       /* dst = a + imm                           */
    IR_SUB,        /* dst = a - b                             */
    IR_SUBI,       /* dst = a - imm                           */
    IR_MUL,        /* dst = a * b                             */
    IR_DIV,        /* dst = a / b (signed)                    */
    IR_DIVU,       /* dst = a / b (unsigned)                  */
    IR_MOD,        /* dst = a % b (signed)                    */
    IR_NEG,        /* dst = -a                                */

    /* Bitwise */
    IR_AND,        /* dst = a & b                             */
    IR_OR,         /* dst = a | b                             */
    IR_XOR,        /* dst = a ^ b                             */
    IR_NOT,        /* dst = ~a                                */
    IR_SHL,        /* dst = a << b                            */
    IR_SHR,        /* dst = a >> b (logical)                  */
    IR_SAR,        /* dst = a >> b (arithmetic)               */

    /* Memory */
    IR_LOAD8,      /* dst = *(uint8_t*)addr                   */
    IR_LOAD16,     /* dst = *(uint16_t*)addr                  */
    IR_LOAD32,     /* dst = *(uint32_t*)addr                  */
    IR_LOAD64,     /* dst = *(uint64_t*)addr                  */
    IR_STORE8,     /* *(uint8_t*)addr = src                   */
    IR_STORE16,    /* *(uint16_t*)addr = src                  */
    IR_STORE32,    /* *(uint32_t*)addr = src                  */
    IR_STORE64,    /* *(uint64_t*)addr = src                  */

    /* Control flow */
    IR_JMP,        /* unconditional jump to label             */
    IR_BEQ,        /* branch if a == b                        */
    IR_BNE,        /* branch if a != b                        */
    IR_BLT,        /* branch if a <  b (signed)               */
    IR_BGE,        /* branch if a >= b (signed)               */
    IR_BLTU,       /* branch if a <  b (unsigned)             */
    IR_BGEU,       /* branch if a >= b (unsigned)             */

    /* Function calls */
    IR_CALL,       /* call target function (by label)         */
    IR_RET,        /* return                                  */

    /* Comparisons → boolean vreg */
    IR_CMP_EQ,     /* dst = (a == b) ? 1 : 0                 */
    IR_CMP_NE,     /* dst = (a != b) ? 1 : 0                 */
    IR_CMP_LT,     /* dst = (a <  b) ? 1 : 0 (signed)        */
    IR_CMP_GE,     /* dst = (a >= b) ? 1 : 0 (signed)        */
    IR_CMP_LTU,    /* unsigned                                */
    IR_CMP_GEU,    /* unsigned                                */

    /* Type conversions */
    IR_SEXT8,      /* sign-extend 8→64                        */
    IR_SEXT16,     /* sign-extend 16→64                       */
    IR_SEXT32,     /* sign-extend 32→64                       */
    IR_ZEXT8,      /* zero-extend 8→64                        */
    IR_ZEXT16,
    IR_ZEXT32,

    /* Host syscall passthrough */
    IR_SYSCALL,    /* guest syscall → host syscall shim        */

    /* Housekeeping */
    IR_LABEL,      /* defines a branch target label           */
    IR_NOP,
    IR_COUNT
} IROp;

/* ─── IR operand ──────────────────────────────────────────────────── */
typedef struct {
    bool    is_imm;   /* true → use imm, false → use reg       */
    IRReg   reg;
    int64_t imm;
} IROperand;

#define IR_OP_REG(r)  ((IROperand){ .is_imm = false, .reg = (r) })
#define IR_OP_IMM(i)  ((IROperand){ .is_imm = true,  .imm = (int64_t)(i) })

/* ─── IR instruction ──────────────────────────────────────────────── */
typedef struct IRInsn {
    IROp      op;
    IRReg     dst;        /* destination vreg (IR_REG_INVALID if none) */
    IROperand src[2];     /* up to two source operands                 */
    uint32_t  label;      /* for IR_LABEL / branch targets             */
    uint64_t  guest_pc;   /* original guest PC for this insn           */
    struct IRInsn *next;
} IRInsn;

/* ─── IR basic block ──────────────────────────────────────────────── */
typedef struct IRBlock {
    uint64_t  guest_pc_start;  /* guest PC of first insn             */
    uint64_t  guest_pc_end;    /* guest PC of last insn + 1          */
    IRInsn   *head;
    IRInsn   *tail;
    uint32_t  num_insns;
    uint32_t  next_label;      /* label counter inside this block    */
    uint32_t  next_vreg;       /* vreg allocator                     */
    struct IRBlock *next;      /* linked list of all blocks          */
} IRBlock;

/* ─── IR translation unit ─────────────────────────────────────────── */
typedef struct FEMUIRUnit {
    IRBlock  *blocks;           /* linked list head                  */
    uint32_t  num_blocks;
    FEMUArch  guest_arch;
    FEMUArch  host_arch;
} FEMUIRUnit;

/* ─── IR API ──────────────────────────────────────────────────────── */

/* Unit / block lifecycle */
FEMUIRUnit *ir_unit_create(FEMUArch guest, FEMUArch host);
void        ir_unit_destroy(FEMUIRUnit *unit);
IRBlock    *ir_block_create(FEMUIRUnit *unit, uint64_t guest_pc);

/* Vreg / label allocation */
IRReg       ir_alloc_vreg(IRBlock *b);
uint32_t    ir_alloc_label(IRBlock *b);

/* Instruction emission */
IRInsn *ir_emit(IRBlock *b, IROp op, IRReg dst, IROperand s0, IROperand s1);
IRInsn *ir_emit_movi(IRBlock *b, IRReg dst, int64_t imm);
IRInsn *ir_emit_mov(IRBlock *b, IRReg dst, IRReg src);
IRInsn *ir_emit_load(IRBlock *b, IROp op, IRReg dst, IRReg addr, int64_t off);
IRInsn *ir_emit_store(IRBlock *b, IROp op, IRReg src, IRReg addr, int64_t off);
IRInsn *ir_emit_branch(IRBlock *b, IROp op, IRReg a, IRReg b_reg, uint32_t lbl);
IRInsn *ir_emit_jmp(IRBlock *b, uint32_t label);
IRInsn *ir_emit_label(IRBlock *b, uint32_t label);
IRInsn *ir_emit_ret(IRBlock *b);
IRInsn *ir_emit_syscall(IRBlock *b, IRReg num);

/* Dump */
void ir_dump_unit(const FEMUIRUnit *unit, FILE *out);
void ir_dump_block(const IRBlock *block, FILE *out);
const char *ir_op_name(IROp op);

#endif /* FEMU_IR_H */
