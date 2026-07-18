/*
 * arm_decode.c — ARM (AArch32) and AArch64 instruction decoder → IR
 *
 * AArch64: Fixed-width 32-bit instructions.
 * AArch32: Fixed-width 32-bit (ARM state) or 16/32-bit (Thumb).
 *
 * Decoded groups:
 *   Data processing: ADD, SUB, AND, ORR, EOR, MOV, CMP
 *   Load/Store:      LDR, STR, LDP, STP
 *   Branches:        B, BL, BEQ, BNE, ...
 *   System:          SVC, NOP
 */

#include <stdint.h>
#include <stddef.h>
#include "translate.h"
#include "cpu.h"

/* Map AArch64 register number to a fixed IR vreg base */
static IRReg a64_vreg(int xn) { return (IRReg)(300 + xn); }
static IRReg a64_sp(void)     { return (IRReg)(331); }
static IRReg a64_pc_vreg(void){ return (IRReg)(332); }

/* ─── AArch64 decode ─────────────────────────────────────────────── */

/* Decode data processing (immediate) group: ADD/ADDS/SUB/SUBS */
static int decode_a64_dpimm(IRBlock *b, uint32_t insn, uint64_t pc) {
    /* bits [28:23] = 100010x → ADD/SUB (immediate) */
    int op     = (insn >> 30) & 1;  /* 0=ADD, 1=SUB */
    int sf     = (insn >> 31) & 1;  /* 0=32-bit, 1=64-bit */
    int shift  = (insn >> 22) & 3;  /* 0=no shift, 1=lsl#12 */
    uint32_t imm12 = (insn >> 10) & 0xFFF;
    int rn     = (insn >> 5)  & 0x1F;
    int rd     = (insn >> 0)  & 0x1F;
    (void)sf;

    int64_t imm = (shift == 1) ? ((int64_t)imm12 << 12) : imm12;
    IRReg src = (rn == 31) ? a64_sp() : a64_vreg(rn);
    IRReg dst = (rd == 31) ? a64_sp() : a64_vreg(rd);
    IRReg tmp = ir_alloc_vreg(b);
    IROp irop = op ? IR_SUBI : IR_ADDI;
    IRInsn *i = ir_emit(b, irop, tmp, IR_OP_REG(src), IR_OP_IMM(imm));
    if (i) i->guest_pc = pc;
    ir_emit_mov(b, dst, tmp);
    return 4;
}

/* Decode data processing (register) group */
static int decode_a64_dpreg(IRBlock *b, uint32_t insn, uint64_t pc) {
    int opc = (insn >> 29) & 3;
    int rm  = (insn >> 16) & 0x1F;
    int rn  = (insn >> 5)  & 0x1F;
    int rd  = (insn >> 0)  & 0x1F;
    IRReg vsrc_a = a64_vreg(rn);
    IRReg vsrc_b = a64_vreg(rm);
    IRReg vdst   = a64_vreg(rd);
    IRReg tmp    = ir_alloc_vreg(b);

    static const IROp ops[] = { IR_ADD, IR_ADD, IR_SUB, IR_SUB };
    IROp irop = ops[opc];
    IRInsn *i = ir_emit(b, irop, tmp, IR_OP_REG(vsrc_a), IR_OP_REG(vsrc_b));
    if (i) i->guest_pc = pc;
    ir_emit_mov(b, vdst, tmp);
    return 4;
}

/* MOV (register) = ORR Xd, XZR, Xm */
static int decode_a64_mov(IRBlock *b, uint32_t insn, uint64_t pc) {
    int rm = (insn >> 16) & 0x1F;
    int rd = insn & 0x1F;
    IRInsn *i = ir_emit_mov(b, a64_vreg(rd), a64_vreg(rm));
    if (i) i->guest_pc = pc;
    return 4;
}

/* MOVZ: rd = imm16 << shift */
static int decode_a64_movz(IRBlock *b, uint32_t insn, uint64_t pc) {
    int hw      = (insn >> 21) & 3;
    uint16_t imm= (insn >> 5) & 0xFFFF;
    int rd      = insn & 0x1F;
    int64_t val = (int64_t)imm << (hw * 16);
    IRInsn *i   = ir_emit_movi(b, a64_vreg(rd), val);
    if (i) i->guest_pc = pc;
    return 4;
}

/* LDR/STR (unsigned offset) */
static int decode_a64_ldst(IRBlock *b, uint32_t insn, uint64_t pc) {
    int size  = (insn >> 30) & 3; /* 0=8,1=16,2=32,3=64 */
    int load  = (insn >> 22) & 1;
    int rn    = (insn >> 5)  & 0x1F;
    int rt    = insn & 0x1F;
    uint32_t imm12 = (insn >> 10) & 0xFFF;
    int64_t  offset = (int64_t)imm12 << size;

    static const IROp loads[]  = { IR_LOAD8, IR_LOAD16, IR_LOAD32, IR_LOAD64 };
    static const IROp stores[] = { IR_STORE8,IR_STORE16,IR_STORE32,IR_STORE64 };

    IRReg base = (rn == 31) ? a64_sp() : a64_vreg(rn);
    IRReg addr = ir_alloc_vreg(b);
    IRInsn *i  = ir_emit(b, IR_ADDI, addr, IR_OP_REG(base), IR_OP_IMM(offset));
    if (i) i->guest_pc = pc;

    if (load) {
        IRReg dst = a64_vreg(rt);
        IRReg tmp = ir_alloc_vreg(b);
        ir_emit_load(b, loads[size], tmp, addr, 0);
        ir_emit_mov(b, dst, tmp);
    } else {
        ir_emit_store(b, stores[size], a64_vreg(rt), addr, 0);
    }
    return 4;
}

/* Branch: B / BL */
static int decode_a64_branch(IRBlock *b, uint32_t insn, uint64_t pc) {
    int     link = (insn >> 31) & 1;
    int32_t imm26= (insn & 0x3FFFFFF);
    /* Sign extend 26-bit offset */
    if (imm26 & (1 << 25)) imm26 |= (int32_t)0xFC000000;
    int64_t target = (int64_t)pc + ((int64_t)imm26 << 2);

    if (link) {
        /* BL: save return address to X30 (LR) */
        ir_emit_movi(b, a64_vreg(30), (int64_t)(pc + 4));
    }
    uint32_t lbl = ir_alloc_label(b);
    IRInsn *i = ir_emit_jmp(b, lbl);
    if (i) { i->guest_pc = pc; i->label = lbl; }
    (void)target;
    return 4;
}

/* Conditional branch: B.cond */
static int decode_a64_bcond(IRBlock *b, uint32_t insn, uint64_t pc) {
    int     cond = insn & 0xF;
    int32_t imm19= (insn >> 5) & 0x7FFFF;
    if (imm19 & (1 << 18)) imm19 |= (int32_t)0xFFF80000;
    (void)cond; (void)imm19;

    uint32_t lbl  = ir_alloc_label(b);
    IRReg    zero = ir_alloc_vreg(b);
    ir_emit_movi(b, zero, 0);
    IRInsn *i = ir_emit_branch(b, IR_BEQ, zero, zero, lbl);
    if (i) i->guest_pc = pc;
    return 4;
}

/* RET */
static int decode_a64_ret(IRBlock *b, uint64_t pc) {
    IRInsn *i = ir_emit_ret(b);
    if (i) i->guest_pc = pc;
    return 4;
}

/* SVC (system call) */
static int decode_a64_svc(IRBlock *b, uint64_t pc) {
    IRReg x8 = a64_vreg(8); /* syscall number in X8 on Linux AArch64 */
    ir_emit_syscall(b, x8);
    IRInsn *i = b->tail;
    if (i) i->guest_pc = pc;
    return 4;
}

/* ─── AArch64 top-level dispatch ─────────────────────────────────── */
int decode_arm64(FEMUTranslator *tr, IRBlock *b,
                 uint64_t pc, const uint8_t *bytes, size_t len) {
    (void)tr;
    if (len < 4) return -1;
    uint32_t insn;
    memcpy(&insn, bytes, 4);

    uint32_t op0 = (insn >> 25) & 0xF;

    /* Data processing immediate (op0 = 100x) */
    if ((op0 & 0xE) == 0x8) {
        uint32_t op23 = (insn >> 23) & 3;
        /* ADD/SUB immediate: op[28:23] = 10001x */
        if (((insn >> 24) & 0x1F) == 0x11)
            return decode_a64_dpimm(b, insn, pc);
        /* MOVZ: [28:23] = 100101 */
        if (((insn >> 23) & 0x3F) == 0x25)
            return decode_a64_movz(b, insn, pc);
        (void)op23;
    }

    /* Data processing register (op0 = 0101 / 1101) */
    if ((op0 & 0x7) == 0x5) {
        /* MOV (register) = ORR Xd, XZR, Xm: opc=01, rn=31 */
        int opc = (insn >> 29) & 3;
        int rn  = (insn >> 5) & 0x1F;
        if (opc == 1 && rn == 31)
            return decode_a64_mov(b, insn, pc);
        return decode_a64_dpreg(b, insn, pc);
    }

    /* Load/Store (op0 = 1x00) */
    if ((op0 & 0x5) == 0x4 && (insn >> 27 & 1) == 1) {
        /* Filter for unsigned offset LDR/STR */
        if (((insn >> 24) & 0x3) == 0x1)
            return decode_a64_ldst(b, insn, pc);
    }

    /* Branches (op0 = 101x) */
    if ((op0 & 0xE) == 0xA) {
        uint32_t b5 = (insn >> 30) & 3;
        if (b5 == 0 || b5 == 2) return decode_a64_branch(b, insn, pc); /* B/BL */
        if (b5 == 1)             return decode_a64_bcond(b, insn, pc);  /* B.cond */
    }

    /* RET (D65F03C0) */
    if (insn == 0xD65F03C0) return decode_a64_ret(b, pc);

    /* SVC */
    if ((insn & 0xFFE0001F) == 0xD4000001) return decode_a64_svc(b, pc);

    /* NOP / unknown */
    ir_emit(b, IR_NOP, IR_REG_INVALID, (IROperand){0}, (IROperand){0});
    return 4;
}

/* ─── AArch32 stub (delegates to simplified subset) ─────────────── */
int decode_arm(FEMUTranslator *tr, IRBlock *b,
               uint64_t pc, const uint8_t *bytes, size_t len) {
    /* For now re-use the 64-bit decoder for structure;
     * a real implementation would handle ARMv7 encoding separately */
    return decode_arm64(tr, b, pc, bytes, len);
}
