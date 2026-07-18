/*
 * riscv_decode.c — RISC-V (RV32I / RV64I) instruction decoder → IR
 *
 * RISC-V is the cleanest ISA to decode: fixed-width 32-bit instructions,
 * 3 source fields (opcode, funct3, funct7), 5-bit register specifiers.
 *
 * Supported instruction groups:
 *   R-type : ADD, SUB, AND, OR, XOR, SLL, SRL, SRA, MUL, DIV, REM
 *   I-type : ADDI, ANDI, ORI, XORI, SLLI, SRLI, SRAI, LW, LD, JALR
 *   S-type : SW, SD, SB, SH
 *   B-type : BEQ, BNE, BLT, BGE, BLTU, BGEU
 *   U-type : LUI, AUIPC
 *   J-type : JAL
 *   System : ECALL, EBREAK
 */

#include <stdint.h>
#include <stddef.h>
#include "translate.h"
#include "cpu.h"

/* Map RISC-V register x0..x31 to IR vregs (base 400) */
static IRReg rv_vreg(int xn)
{
    return (IRReg)(400 + xn);
}

/* ─── Instruction field extraction ───────────────────────────────── */
static inline int rv_rd   (uint32_t i) { return (i >> 7)  & 0x1F; }
static inline int rv_rs1  (uint32_t i) { return (i >> 15) & 0x1F; }
static inline int rv_rs2  (uint32_t i) { return (i >> 20) & 0x1F; }
static inline int rv_funct3(uint32_t i){ return (i >> 12) & 0x7;  }
static inline int rv_funct7(uint32_t i){ return (i >> 25) & 0x7F; }

/* Sign-extended immediates */
static inline int32_t rv_imm_i(uint32_t i) { return (int32_t)i >> 20; }
static inline int32_t rv_imm_s(uint32_t i) {
    return ((int32_t)(i & 0xFE000000) >> 20) | ((i >> 7) & 0x1F);
}
static inline int32_t rv_imm_b(uint32_t i) {
    return ((int32_t)(i & 0x80000000) >> 19)
         | ((i & 0x80) << 4)
         | ((i >> 20) & 0x7E)
         | ((i >> 7)  & 0x1E);
}
static inline int32_t rv_imm_u(uint32_t i) { return (int32_t)(i & 0xFFFFF000); }
static inline int32_t rv_imm_j(uint32_t i) {
    return ((int32_t)(i & 0x80000000) >> 11)
         | ((i & 0xFF000))
         | ((i >> 9)  & 0x800)
         | ((i >> 20) & 0x7FE);
}

/* ─── Emit helpers ───────────────────────────────────────────────── */

static IRReg rv_get_reg(IRBlock *b, int xn)
{
    if (xn == 0) {
        IRReg z = ir_alloc_vreg(b);
        ir_emit_movi(b, z, 0);
        return z;
    }
    return rv_vreg(xn);
}

static void rv_set_reg(IRBlock *b, int rd, IRReg src, uint64_t pc)
{
    if (rd == 0) return;
    IRInsn *i = ir_emit_mov(b, rv_vreg(rd), src);
    if (i) i->guest_pc = pc;
}

/* ─── R-type ─────────────────────────────────────────────────────── */
static int decode_rv_r(IRBlock *b, uint32_t insn, uint64_t pc)
{
    int f3 = rv_funct3(insn), f7 = rv_funct7(insn);
    int rd = rv_rd(insn), rs1 = rv_rs1(insn), rs2 = rv_rs2(insn);
    IRReg va = rv_get_reg(b, rs1);
    IRReg vb = rv_get_reg(b, rs2);
    IRReg vd = ir_alloc_vreg(b);
    IROp op;
    bool is_m = (f7 == 0x01);
    if (is_m) {
        switch (f3) {
            case 0: op = IR_MUL;  break;
            case 4: op = IR_DIV;  break;
            case 5: op = IR_DIVU; break;
            case 6: op = IR_MOD;  break;
            default: op = IR_NOP; break;
        }
    } else {
        switch (f3) {
            case 0: op = (f7 == 0x20) ? IR_SUB : IR_ADD; break;
            case 1: op = IR_SHL;     break;
            case 2: op = IR_CMP_LT;  break;
            case 3: op = IR_CMP_LTU; break;
            case 4: op = IR_XOR;     break;
            case 5: op = (f7 == 0x20) ? IR_SAR : IR_SHR; break;
            case 6: op = IR_OR;      break;
            case 7: op = IR_AND;     break;
            default: op = IR_NOP;    break;
        }
    }
    IRInsn *i = ir_emit(b, op, vd, IR_OP_REG(va), IR_OP_REG(vb));
    if (i) i->guest_pc = pc;
    rv_set_reg(b, rd, vd, pc);
    return 4;
}

/* ─── I-type: arithmetic ──────────────────────────────────────────── */
static int decode_rv_i_arith(IRBlock *b, uint32_t insn, uint64_t pc)
{
    int f3  = rv_funct3(insn), f7 = rv_funct7(insn);
    int rd  = rv_rd(insn), rs1 = rv_rs1(insn);
    int32_t imm = rv_imm_i(insn);
    IRReg va = rv_get_reg(b, rs1);
    IRReg vd = ir_alloc_vreg(b);
    IROp op;
    switch (f3) {
        case 0: op = IR_ADDI; break;
        case 1: op = IR_SHL;  imm = imm & 63; break;
        case 2: op = IR_CMP_LT;  break;
        case 3: op = IR_CMP_LTU; break;
        case 4: op = IR_XOR;  break;
        case 5: op = (f7 == 0x20) ? IR_SAR : IR_SHR; imm = imm & 63; break;
        case 6: op = IR_OR;   break;
        case 7: op = IR_AND;  break;
        default: op = IR_NOP; break;
    }
    IRInsn *i = ir_emit(b, op, vd, IR_OP_REG(va), IR_OP_IMM(imm));
    if (i) i->guest_pc = pc;
    rv_set_reg(b, rd, vd, pc);
    return 4;
}

/* ─── I-type: load ───────────────────────────────────────────────── */
static int decode_rv_load(IRBlock *b, uint32_t insn, uint64_t pc)
{
    int f3  = rv_funct3(insn);
    int rd  = rv_rd(insn), rs1 = rv_rs1(insn);
    int32_t imm = rv_imm_i(insn);
    static const IROp load_ops[] = {
        IR_LOAD8, IR_LOAD16, IR_LOAD32, IR_LOAD64,
        IR_LOAD8, IR_LOAD16, IR_LOAD32, IR_LOAD64
    };
    IROp lop  = load_ops[f3 & 7];
    IRReg base = rv_get_reg(b, rs1);
    IRReg addr = ir_alloc_vreg(b);
    ir_emit(b, IR_ADDI, addr, IR_OP_REG(base), IR_OP_IMM(imm));
    IRReg vd   = ir_alloc_vreg(b);
    IRInsn *i  = ir_emit_load(b, lop, vd, addr, 0);
    if (i) i->guest_pc = pc;
    if (f3 < 3) {
        static const IROp sext_ops[] = { IR_SEXT8, IR_SEXT16, IR_SEXT32 };
        IRReg ext = ir_alloc_vreg(b);
        ir_emit(b, sext_ops[f3], ext, IR_OP_REG(vd), (IROperand){0});
        vd = ext;
    }
    rv_set_reg(b, rd, vd, pc);
    return 4;
}

/* ─── S-type: store ──────────────────────────────────────────────── */
static int decode_rv_store(IRBlock *b, uint32_t insn, uint64_t pc)
{
    int f3  = rv_funct3(insn);
    int rs1 = rv_rs1(insn), rs2 = rv_rs2(insn);
    int32_t imm = rv_imm_s(insn);
    static const IROp store_ops[] = {
        IR_STORE8, IR_STORE16, IR_STORE32, IR_STORE64
    };
    IROp sop  = (f3 < 4) ? store_ops[f3] : IR_STORE64;
    IRReg base = rv_get_reg(b, rs1);
    IRReg src  = rv_get_reg(b, rs2);
    IRReg addr = ir_alloc_vreg(b);
    ir_emit(b, IR_ADDI, addr, IR_OP_REG(base), IR_OP_IMM(imm));
    IRInsn *i  = ir_emit_store(b, sop, src, addr, 0);
    if (i) i->guest_pc = pc;
    return 4;
}

/* ─── B-type: branch ─────────────────────────────────────────────── */
static int decode_rv_branch(IRBlock *b, uint32_t insn, uint64_t pc)
{
    int f3  = rv_funct3(insn);
    int rs1 = rv_rs1(insn), rs2 = rv_rs2(insn);
    int32_t imm = rv_imm_b(insn);
    (void)imm;
    static const IROp bops[] = {
        IR_BEQ, IR_BNE, IR_NOP, IR_NOP,
        IR_BLT, IR_BGE, IR_BLTU, IR_BGEU
    };
    IROp bop = bops[f3 & 7];
    if (bop == IR_NOP) {
        ir_emit(b, IR_NOP, IR_REG_INVALID, (IROperand){0}, (IROperand){0});
        return 4;
    }
    IRReg va  = rv_get_reg(b, rs1);
    IRReg vb  = rv_get_reg(b, rs2);
    uint32_t lbl = ir_alloc_label(b);
    IRInsn *i    = ir_emit_branch(b, bop, va, vb, lbl);
    if (i) i->guest_pc = pc;
    return 4;
}

/* ─── U-type ─────────────────────────────────────────────────────── */
static int decode_rv_lui(IRBlock *b, uint32_t insn, uint64_t pc)
{
    int rd = rv_rd(insn);
    int32_t imm = rv_imm_u(insn);
    IRReg vd = ir_alloc_vreg(b);
    IRInsn *i = ir_emit_movi(b, vd, imm);
    if (i) i->guest_pc = pc;
    rv_set_reg(b, rd, vd, pc);
    return 4;
}

static int decode_rv_auipc(IRBlock *b, uint32_t insn, uint64_t pc)
{
    int rd = rv_rd(insn);
    int32_t imm = rv_imm_u(insn);
    IRReg vd = ir_alloc_vreg(b);
    IRInsn *i = ir_emit_movi(b, vd, (int64_t)pc + imm);
    if (i) i->guest_pc = pc;
    rv_set_reg(b, rd, vd, pc);
    return 4;
}

/* ─── J-type: JAL ────────────────────────────────────────────────── */
static int decode_rv_jal(IRBlock *b, uint32_t insn, uint64_t pc)
{
    int rd = rv_rd(insn);
    int32_t imm = rv_imm_j(insn);
    (void)imm;
    if (rd != 0) {
        IRReg ret_addr = ir_alloc_vreg(b);
        ir_emit_movi(b, ret_addr, (int64_t)(pc + 4));
        rv_set_reg(b, rd, ret_addr, pc);
    }
    uint32_t lbl = ir_alloc_label(b);
    IRInsn *i = (rd == 0)
        ? ir_emit_jmp(b, lbl)
        : ir_emit(b, IR_CALL, IR_REG_INVALID, IR_OP_IMM(lbl), (IROperand){0});
    if (i) { i->guest_pc = pc; i->label = lbl; }
    return 4;
}

/* ─── JALR (I-type) ──────────────────────────────────────────────── */
static int decode_rv_jalr(IRBlock *b, uint32_t insn, uint64_t pc)
{
    int rd  = rv_rd(insn), rs1 = rv_rs1(insn);
    int32_t imm = rv_imm_i(insn);
    (void)imm;
    if (rd != 0) {
        IRReg ra = ir_alloc_vreg(b);
        ir_emit_movi(b, ra, (int64_t)(pc + 4));
        rv_set_reg(b, rd, ra, pc);
    }
    IRReg base = rv_get_reg(b, rs1);
    uint32_t lbl = ir_alloc_label(b);
    IRInsn *i = ir_emit_jmp(b, lbl);
    if (i) { i->guest_pc = pc; i->src[0] = IR_OP_REG(base); }
    return 4;
}

/* ─── System: ECALL / EBREAK ─────────────────────────────────────── */
static int decode_rv_system(IRBlock *b, uint32_t insn, uint64_t pc)
{
    uint32_t imm12 = insn >> 20;
    if (imm12 == 0) {
        IRReg a7 = rv_get_reg(b, 17);
        ir_emit_syscall(b, a7);
        IRInsn *i = b->tail;
        if (i) i->guest_pc = pc;
    } else {
        ir_emit(b, IR_NOP, IR_REG_INVALID, (IROperand){0}, (IROperand){0});
    }
    return 4;
}

/* ─── Top-level RISC-V decoder ───────────────────────────────────── */
static int decode_rv(FEMUTranslator *tr, IRBlock *b,
                     uint64_t pc, const uint8_t *bytes, size_t len,
                     bool is64)
{
    (void)tr; (void)is64;
    if (len < 4) return -1;
    uint32_t insn;
    memcpy(&insn, bytes, 4);
    uint32_t opcode = insn & 0x7F;
    switch (opcode) {
        case 0x33: return decode_rv_r(b, insn, pc);
        case 0x3B: return decode_rv_r(b, insn, pc);
        case 0x13: return decode_rv_i_arith(b, insn, pc);
        case 0x1B: return decode_rv_i_arith(b, insn, pc);
        case 0x03: return decode_rv_load(b, insn, pc);
        case 0x23: return decode_rv_store(b, insn, pc);
        case 0x63: return decode_rv_branch(b, insn, pc);
        case 0x37: return decode_rv_lui(b, insn, pc);
        case 0x17: return decode_rv_auipc(b, insn, pc);
        case 0x6F: return decode_rv_jal(b, insn, pc);
        case 0x67: return decode_rv_jalr(b, insn, pc);
        case 0x73: return decode_rv_system(b, insn, pc);
        default:
            femu_dbg("riscv_decode: unknown opcode 0x%02x at 0x%llx",
                     opcode, (unsigned long long)pc);
            ir_emit(b, IR_NOP, IR_REG_INVALID, (IROperand){0}, (IROperand){0});
            return 4;
    }
}

int decode_riscv64(FEMUTranslator *tr, IRBlock *b,
                   uint64_t pc, const uint8_t *bytes, size_t len)
{
    return decode_rv(tr, b, pc, bytes, len, true);
}

int decode_riscv32(FEMUTranslator *tr, IRBlock *b,
                   uint64_t pc, const uint8_t *bytes, size_t len)
{
    return decode_rv(tr, b, pc, bytes, len, false);
}
