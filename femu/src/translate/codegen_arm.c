/*
 * codegen_arm.c — IR → AArch64 host binary code generator
 *
 * AArch64 is a clean fixed-width 32-bit instruction ISA.
 * Encoding convention: bits [31:0], little-endian on disk.
 *
 * Register allocation:
 *   X0  = accumulator / result
 *   X1  = operand B
 *   X2  = scratch
 *   X9–X15 = caller-saved temporaries (used for vreg cache)
 *   X19-X28 = callee-saved (frame, base pointer)
 *
 * All vregs are spilled to a stack frame:
 *   [SP + vreg * 8]  (SP is 16-byte aligned)
 */

#include "translate.h"

/* ─── AArch64 instruction encoding helpers ───────────────────────── */

/* Emit a 32-bit AArch64 instruction (little-endian) */
static int emit_insn(FEMUTranslator *tr, uint32_t insn) {
    return tr_emit_u32(tr, insn);
}

/* NOP */
static int emit_nop(FEMUTranslator *tr) {
    return emit_insn(tr, 0xD503201F);
}

/* RET (return via X30/LR) */
static int emit_ret(FEMUTranslator *tr) {
    return emit_insn(tr, 0xD65F03C0);
}

/* MOV X<dst>, X<src>  (encoded as ORR Xd, XZR, Xm) */
static int emit_mov_reg(FEMUTranslator *tr, int dst, int src) {
    /* ORR (shifted register): sf=1, opc=01, shift=00, N=0, Rm=src, imm6=0, Rn=31(XZR), Rd=dst */
    uint32_t insn = 0xAA0003E0
                  | (uint32_t)(src << 16)
                  | (uint32_t)(dst);
    return emit_insn(tr, insn);
}

/* MOVZ X<dst>, imm16 [, LSL #shift*16]
 * For 64-bit immediate we use 4 MOVZ/MOVK instructions. */
static int emit_movz(FEMUTranslator *tr, int dst, uint16_t imm, int hw) {
    /* sf=1, opc=10, hw, imm16, Rd */
    uint32_t insn = 0xD2800000
                  | (uint32_t)(hw << 21)
                  | (uint32_t)((imm & 0xFFFF) << 5)
                  | (uint32_t)(dst & 0x1F);
    return emit_insn(tr, insn);
}

static int emit_movk(FEMUTranslator *tr, int dst, uint16_t imm, int hw) {
    uint32_t insn = 0xF2800000
                  | (uint32_t)(hw << 21)
                  | (uint32_t)((imm & 0xFFFF) << 5)
                  | (uint32_t)(dst & 0x1F);
    return emit_insn(tr, insn);
}

/* Emit 64-bit immediate into X<dst> */
static void emit_movi64(FEMUTranslator *tr, int dst, uint64_t val) {
    emit_movz(tr, dst, (uint16_t)(val >>  0), 0);
    if (val >> 16) emit_movk(tr, dst, (uint16_t)(val >> 16), 1);
    if (val >> 32) emit_movk(tr, dst, (uint16_t)(val >> 32), 2);
    if (val >> 48) emit_movk(tr, dst, (uint16_t)(val >> 48), 3);
}

/* LDR X<dst>, [SP, #off] — 64-bit unsigned offset */
static int emit_ldr_sp(FEMUTranslator *tr, int dst, uint32_t off) {
    /* LDR (unsigned offset) 64-bit: size=11, V=0, opc=01, imm12, Rn=SP(31), Rt=dst */
    uint32_t imm12 = (off >> 3) & 0xFFF; /* scale by 8 */
    uint32_t insn  = 0xF9400000
                   | (imm12 << 10)
                   | (31   << 5)   /* Rn = SP */
                   | (uint32_t)(dst & 0x1F);
    return emit_insn(tr, insn);
}

/* STR X<src>, [SP, #off] */
static int emit_str_sp(FEMUTranslator *tr, int src, uint32_t off) {
    uint32_t imm12 = (off >> 3) & 0xFFF;
    uint32_t insn  = 0xF9000000
                   | (imm12 << 10)
                   | (31   << 5)
                   | (uint32_t)(src & 0x1F);
    return emit_insn(tr, insn);
}

/* ADD X<dst>, X<a>, X<b> */
static int emit_add_reg(FEMUTranslator *tr, int dst, int a, int b) {
    uint32_t insn = 0x8B000000
                  | (uint32_t)(b   << 16)
                  | (uint32_t)(a   << 5)
                  | (uint32_t)(dst & 0x1F);
    return emit_insn(tr, insn);
}

/* SUB X<dst>, X<a>, X<b> */
static int emit_sub_reg(FEMUTranslator *tr, int dst, int a, int b) {
    uint32_t insn = 0xCB000000
                  | (uint32_t)(b   << 16)
                  | (uint32_t)(a   << 5)
                  | (uint32_t)(dst & 0x1F);
    return emit_insn(tr, insn);
}

/* AND X<dst>, X<a>, X<b> */
static int emit_and_reg(FEMUTranslator *tr, int dst, int a, int b) {
    uint32_t insn = 0x8A000000
                  | (uint32_t)(b   << 16)
                  | (uint32_t)(a   << 5)
                  | (uint32_t)(dst & 0x1F);
    return emit_insn(tr, insn);
}

/* ORR X<dst>, X<a>, X<b> */
static int emit_orr_reg(FEMUTranslator *tr, int dst, int a, int b) {
    uint32_t insn = 0xAA000000
                  | (uint32_t)(b   << 16)
                  | (uint32_t)(a   << 5)
                  | (uint32_t)(dst & 0x1F);
    return emit_insn(tr, insn);
}

/* EOR X<dst>, X<a>, X<b> */
static int emit_eor_reg(FEMUTranslator *tr, int dst, int a, int b) {
    uint32_t insn = 0xCA000000
                  | (uint32_t)(b   << 16)
                  | (uint32_t)(a   << 5)
                  | (uint32_t)(dst & 0x1F);
    return emit_insn(tr, insn);
}

/* MUL X<dst>, X<a>, X<b> (MADD with XZR) */
static int emit_mul_reg(FEMUTranslator *tr, int dst, int a, int b) {
    uint32_t insn = 0x9B007C00
                  | (uint32_t)(b   << 16)
                  | (uint32_t)(a   << 5)
                  | (uint32_t)(dst & 0x1F);
    return emit_insn(tr, insn);
}

/* SVC #0 */
static int emit_svc(FEMUTranslator *tr) {
    return emit_insn(tr, 0xD4000001);
}

/* B (unconditional branch, placeholder offset 0) */
static int emit_b(FEMUTranslator *tr) {
    return emit_insn(tr, 0x14000000);
}

/* B.EQ / B.NE placeholder */
static int emit_bcond(FEMUTranslator *tr, int cond) {
    uint32_t insn = 0x54000000 | (cond & 0xF);
    return emit_insn(tr, insn);
}

/* ─── Vreg ↔ stack slot ──────────────────────────────────────────── */
#define VREG_SP_SLOT(v) ((uint32_t)((v) * 8))

static void load_vreg(FEMUTranslator *tr, int host_reg, IRReg v) {
    emit_ldr_sp(tr, host_reg, VREG_SP_SLOT(v));
}

static void store_vreg(FEMUTranslator *tr, IRReg v, int host_reg) {
    emit_str_sp(tr, host_reg, VREG_SP_SLOT(v));
}

/* ─── Prologue / epilogue ────────────────────────────────────────── */
static void emit_prologue_arm64(FEMUTranslator *tr, uint32_t nregs) {
    /* STP X29, X30, [SP, #-16]! */
    emit_insn(tr, 0xA9BF7BFD);
    /* MOV X29, SP */
    emit_insn(tr, 0x910003FD);
    /* SUB SP, SP, #frame  (frame = nregs * 8, 16-aligned) */
    uint32_t frame = FEMU_ALIGN(nregs * 8, 16);
    if (frame > 0 && frame <= 4095) {
        uint32_t sub = 0xD1000000 | (frame << 10) | (31 << 5) | 31;
        emit_insn(tr, sub);
    }
}

static void emit_epilogue_arm64(FEMUTranslator *tr) {
    /* MOV SP, X29 */
    emit_insn(tr, 0x910003BF);
    /* LDP X29, X30, [SP], #16 */
    emit_insn(tr, 0xA8C17BFD);
    emit_ret(tr);
}

/* ─── Per-instruction codegen ────────────────────────────────────── */
static int codegen_arm64_insn(FEMUTranslator *tr, const IRInsn *i) {
    /* Host registers: X0=acc, X1=B, X2=scratch */
    switch (i->op) {
        case IR_NOP:
        case IR_LABEL:
            emit_nop(tr);
            break;

        case IR_MOVI:
            emit_movi64(tr, 0, (uint64_t)i->src[0].imm);
            if (i->dst != IR_REG_INVALID) store_vreg(tr, i->dst, 0);
            break;

        case IR_MOV:
            if (i->src[0].is_imm) {
                emit_movi64(tr, 0, (uint64_t)i->src[0].imm);
            } else {
                load_vreg(tr, 0, i->src[0].reg);
            }
            if (i->dst != IR_REG_INVALID) store_vreg(tr, i->dst, 0);
            break;

        case IR_ADD:
        case IR_ADDI: {
            if (i->src[0].is_imm) emit_movi64(tr, 0, (uint64_t)i->src[0].imm);
            else                  load_vreg(tr, 0, i->src[0].reg);
            if (i->src[1].is_imm) emit_movi64(tr, 1, (uint64_t)i->src[1].imm);
            else                  load_vreg(tr, 1, i->src[1].reg);
            emit_add_reg(tr, 0, 0, 1);
            if (i->dst != IR_REG_INVALID) store_vreg(tr, i->dst, 0);
            break;
        }

        case IR_SUB:
        case IR_SUBI: {
            if (i->src[0].is_imm) emit_movi64(tr, 0, (uint64_t)i->src[0].imm);
            else                  load_vreg(tr, 0, i->src[0].reg);
            if (i->src[1].is_imm) emit_movi64(tr, 1, (uint64_t)i->src[1].imm);
            else                  load_vreg(tr, 1, i->src[1].reg);
            emit_sub_reg(tr, 0, 0, 1);
            if (i->dst != IR_REG_INVALID) store_vreg(tr, i->dst, 0);
            break;
        }

        case IR_AND: {
            load_vreg(tr, 0, i->src[0].reg);
            load_vreg(tr, 1, i->src[1].reg);
            emit_and_reg(tr, 0, 0, 1);
            if (i->dst != IR_REG_INVALID) store_vreg(tr, i->dst, 0);
            break;
        }

        case IR_OR: {
            load_vreg(tr, 0, i->src[0].reg);
            load_vreg(tr, 1, i->src[1].reg);
            emit_orr_reg(tr, 0, 0, 1);
            if (i->dst != IR_REG_INVALID) store_vreg(tr, i->dst, 0);
            break;
        }

        case IR_XOR: {
            load_vreg(tr, 0, i->src[0].reg);
            load_vreg(tr, 1, i->src[1].reg);
            emit_eor_reg(tr, 0, 0, 1);
            if (i->dst != IR_REG_INVALID) store_vreg(tr, i->dst, 0);
            break;
        }

        case IR_MUL: {
            load_vreg(tr, 0, i->src[0].reg);
            load_vreg(tr, 1, i->src[1].reg);
            emit_mul_reg(tr, 0, 0, 1);
            if (i->dst != IR_REG_INVALID) store_vreg(tr, i->dst, 0);
            break;
        }

        case IR_SYSCALL:
            emit_svc(tr);
            break;

        case IR_JMP:
            emit_b(tr);
            break;

        case IR_BEQ: emit_bcond(tr, 0x0); break; /* B.EQ */
        case IR_BNE: emit_bcond(tr, 0x1); break; /* B.NE */
        case IR_BLT: emit_bcond(tr, 0xB); break; /* B.LT */
        case IR_BGE: emit_bcond(tr, 0xA); break; /* B.GE */
        case IR_BLTU:emit_bcond(tr, 0x3); break; /* B.CC */
        case IR_BGEU:emit_bcond(tr, 0x2); break; /* B.CS */

        case IR_RET:
            emit_epilogue_arm64(tr);
            break;

        default:
            emit_nop(tr);
            break;
    }
    return FEMU_OK;
}

/* ─── Main AArch64 codegen entry ─────────────────────────────────── */
int codegen_arm64(FEMUTranslator *tr, const FEMUIRUnit *unit) {
    for (const IRBlock *b = unit->blocks; b; b = b->next) {
        emit_prologue_arm64(tr, b->next_vreg + 4);

        for (const IRInsn *i = b->head; i; i = i->next) {
            int r = codegen_arm64_insn(tr, i);
            if (r != FEMU_OK) return r;
            tr->host_insns++;
        }

        if (!b->tail || b->tail->op != IR_RET)
            emit_epilogue_arm64(tr);
    }
    return FEMU_OK;
}
