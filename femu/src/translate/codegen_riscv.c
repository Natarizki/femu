/*
 * codegen_riscv.c — IR → RISC-V (RV64I) host binary code generator
 *
 * All RISC-V instructions are 32 bits wide (little-endian).
 * Register allocation:
 *   a0 (x10) = accumulator / result
 *   a1 (x11) = operand B
 *   a2 (x12) = scratch
 *   t0-t6    = temporaries
 *
 * Vregs spilled to stack: [sp + vreg * 8]
 */

#include "translate.h"

/* ─── RISC-V register ABI numbers ───────────────────────────────── */
#define RV_ZERO  0
#define RV_RA    1
#define RV_SP    2
#define RV_GP    3
#define RV_TP    4
#define RV_T0    5
#define RV_T1    6
#define RV_T2    7
#define RV_S0    8   /* FP */
#define RV_S1    9
#define RV_A0    10
#define RV_A1    11
#define RV_A2    12

/* ─── Instruction encoding helpers ──────────────────────────────── */

static int emit_rv(FEMUTranslator *tr, uint32_t insn) {
    return tr_emit_u32(tr, insn);
}

/* R-type: ADD, SUB, AND, OR, XOR, SLL, SRL, SRA, MUL */
static uint32_t rv_r(int funct7, int rs2, int rs1, int funct3, int rd, int opcode) {
    return (uint32_t)(funct7 << 25) | (uint32_t)(rs2 << 20)
         | (uint32_t)(rs1 << 15)   | (uint32_t)(funct3 << 12)
         | (uint32_t)(rd  << 7)    | (uint32_t)(opcode);
}

/* I-type: ADDI, LW, LD, JALR, etc. */
static uint32_t rv_i(int32_t imm12, int rs1, int funct3, int rd, int opcode) {
    return (uint32_t)((imm12 & 0xFFF) << 20)
         | (uint32_t)(rs1    << 15)
         | (uint32_t)(funct3 << 12)
         | (uint32_t)(rd     << 7)
         | (uint32_t)(opcode);
}

/* S-type: SW, SD, SB, SH */
static uint32_t rv_s(int32_t imm12, int rs2, int rs1, int funct3, int opcode) {
    uint32_t hi = (uint32_t)((imm12 >> 5) & 0x7F) << 25;
    uint32_t lo = (uint32_t)((imm12 & 0x1F)) << 7;
    return hi | (uint32_t)(rs2 << 20) | (uint32_t)(rs1 << 15)
             | (uint32_t)(funct3 << 12) | lo | (uint32_t)(opcode);
}

/* B-type: BEQ, BNE, BLT, BGE, BLTU, BGEU */
static uint32_t rv_b(int32_t offset, int rs2, int rs1, int funct3, int opcode) {
    /* offset is in multiples of 2 */
    uint32_t b12  = (uint32_t)((offset >> 12) & 1) << 31;
    uint32_t b11  = (uint32_t)((offset >> 11) & 1) << 7;
    uint32_t b10_5= (uint32_t)((offset >>  5) & 0x3F) << 25;
    uint32_t b4_1 = (uint32_t)((offset >>  1) & 0xF)  << 8;
    return b12 | b10_5 | (uint32_t)(rs2 << 20) | (uint32_t)(rs1 << 15)
              | (uint32_t)(funct3 << 12) | b4_1 | b11 | (uint32_t)(opcode);
}

/* U-type: LUI, AUIPC */
static uint32_t rv_u(int32_t imm20, int rd, int opcode) {
    return (uint32_t)(imm20 & (int32_t)0xFFFFF000) | (uint32_t)(rd << 7) | (uint32_t)(opcode);
}

/* J-type: JAL */
static uint32_t rv_j(int32_t offset, int rd, int opcode) {
    uint32_t b20   = (uint32_t)((offset >> 20) & 1) << 31;
    uint32_t b19_12= (uint32_t)((offset >> 12) & 0xFF) << 12;
    uint32_t b11   = (uint32_t)((offset >> 11) & 1) << 20;
    uint32_t b10_1 = (uint32_t)((offset >>  1) & 0x3FF) << 21;
    return b20 | b10_1 | b11 | b19_12 | (uint32_t)(rd << 7) | (uint32_t)(opcode);
}

/* ─── Specific instruction emitters ─────────────────────────────── */

/* ADDI rd, rs1, imm */
static int rv_addi(FEMUTranslator *tr, int rd, int rs1, int32_t imm) {
    return emit_rv(tr, rv_i(imm, rs1, 0, rd, 0x13));
}

/* LD rd, imm(rs1) — 64-bit load */
static int rv_ld(FEMUTranslator *tr, int rd, int rs1, int32_t off) {
    return emit_rv(tr, rv_i(off, rs1, 3, rd, 0x03));
}

/* SD rs2, imm(rs1) — 64-bit store */
static int rv_sd(FEMUTranslator *tr, int rs2, int rs1, int32_t off) {
    return emit_rv(tr, rv_s(off, rs2, rs1, 3, 0x23));
}

/* ADD rd, rs1, rs2 */
static int rv_add(FEMUTranslator *tr, int rd, int rs1, int rs2) {
    return emit_rv(tr, rv_r(0, rs2, rs1, 0, rd, 0x33));
}

/* SUB rd, rs1, rs2 */
static int rv_sub(FEMUTranslator *tr, int rd, int rs1, int rs2) {
    return emit_rv(tr, rv_r(0x20, rs2, rs1, 0, rd, 0x33));
}

/* AND rd, rs1, rs2 */
static int rv_and(FEMUTranslator *tr, int rd, int rs1, int rs2) {
    return emit_rv(tr, rv_r(0, rs2, rs1, 7, rd, 0x33));
}

/* OR rd, rs1, rs2 */
static int rv_or(FEMUTranslator *tr, int rd, int rs1, int rs2) {
    return emit_rv(tr, rv_r(0, rs2, rs1, 6, rd, 0x33));
}

/* XOR rd, rs1, rs2 */
static int rv_xor(FEMUTranslator *tr, int rd, int rs1, int rs2) {
    return emit_rv(tr, rv_r(0, rs2, rs1, 4, rd, 0x33));
}

/* MUL rd, rs1, rs2 (M extension) */
static int rv_mul(FEMUTranslator *tr, int rd, int rs1, int rs2) {
    return emit_rv(tr, rv_r(1, rs2, rs1, 0, rd, 0x33));
}

/* SLL rd, rs1, rs2 */
static int rv_sll(FEMUTranslator *tr, int rd, int rs1, int rs2) {
    return emit_rv(tr, rv_r(0, rs2, rs1, 1, rd, 0x33));
}

/* SRL rd, rs1, rs2 */
static int rv_srl(FEMUTranslator *tr, int rd, int rs1, int rs2) {
    return emit_rv(tr, rv_r(0, rs2, rs1, 5, rd, 0x33));
}

/* LUI rd, imm20 */
static int rv_lui(FEMUTranslator *tr, int rd, int32_t imm20) {
    return emit_rv(tr, rv_u(imm20, rd, 0x37));
}

/* ECALL */
static int rv_ecall(FEMUTranslator *tr) {
    return emit_rv(tr, 0x00000073);
}

/* JAL zero, 0 (infinite loop placeholder — like NOP for branches) */
static int rv_jal_nop(FEMUTranslator *tr) {
    return emit_rv(tr, rv_j(0, RV_ZERO, 0x6F));
}

/* NOP = ADDI zero, zero, 0 */
static int rv_nop(FEMUTranslator *tr) {
    return rv_addi(tr, RV_ZERO, RV_ZERO, 0);
}

/* RET = JALR zero, ra, 0 */
static int rv_ret(FEMUTranslator *tr) {
    return emit_rv(tr, rv_i(0, RV_RA, 0, RV_ZERO, 0x67));
}

/* ─── Load 64-bit immediate into a register ──────────────────────── */
/* LUI + ADDI — limited to 32-bit sign-extended values for now */
static void rv_movi(FEMUTranslator *tr, int rd, int64_t val) {
    if (val >= -2048 && val < 2048) {
        rv_addi(tr, rd, RV_ZERO, (int32_t)val);
        return;
    }
    /* For larger values: LUI rd, upper; ADDI rd, rd, lower */
    int32_t lower = (int32_t)(val & 0xFFF);
    if (lower >= 2048) lower -= 4096;
    int32_t upper = (int32_t)((val - lower) >> 12);
    rv_lui(tr, rd, (int32_t)(upper << 12));
    if (lower) rv_addi(tr, rd, rd, lower);
    /* For true 64-bit: emit AUIPC + shifts — omitted for brevity */
}

/* ─── Vreg stack slot helpers ────────────────────────────────────── */
#define VREG_OFFSET(v)  ((int32_t)((v) * 8))

static void rv_load_vreg(FEMUTranslator *tr, int rd, IRReg v) {
    rv_ld(tr, rd, RV_SP, VREG_OFFSET(v));
}

static void rv_store_vreg(FEMUTranslator *tr, IRReg v, int rs) {
    rv_sd(tr, rs, RV_SP, VREG_OFFSET(v));
}

/* ─── Prologue / epilogue ────────────────────────────────────────── */
static void rv_emit_prologue(FEMUTranslator *tr, uint32_t nregs) {
    int32_t frame = (int32_t)FEMU_ALIGN(nregs * 8 + 16, 16);
    /* ADDI SP, SP, -frame */
    rv_addi(tr, RV_SP, RV_SP, -frame);
    /* SD RA, frame-8(SP) */
    rv_sd(tr, RV_RA, RV_SP, frame - 8);
    /* SD S0, frame-16(SP) */
    rv_sd(tr, RV_S0, RV_SP, frame - 16);
    /* ADDI S0, SP, frame */
    rv_addi(tr, RV_S0, RV_SP, frame);
}

static void rv_emit_epilogue(FEMUTranslator *tr, uint32_t nregs) {
    int32_t frame = (int32_t)FEMU_ALIGN(nregs * 8 + 16, 16);
    /* LD RA, frame-8(SP) */
    rv_ld(tr, RV_RA, RV_SP, frame - 8);
    /* LD S0, frame-16(SP) */
    rv_ld(tr, RV_S0, RV_SP, frame - 16);
    /* ADDI SP, SP, +frame */
    rv_addi(tr, RV_SP, RV_SP, frame);
    /* RET */
    rv_ret(tr);
}

/* ─── Per-instruction codegen ────────────────────────────────────── */
static int codegen_riscv_insn(FEMUTranslator *tr, const IRInsn *i) {
    switch (i->op) {
        case IR_NOP:
        case IR_LABEL:
            rv_nop(tr);
            break;

        case IR_MOVI:
            rv_movi(tr, RV_A0, i->src[0].imm);
            if (i->dst != IR_REG_INVALID) rv_store_vreg(tr, i->dst, RV_A0);
            break;

        case IR_MOV:
            if (i->src[0].is_imm) rv_movi(tr, RV_A0, i->src[0].imm);
            else                  rv_load_vreg(tr, RV_A0, i->src[0].reg);
            if (i->dst != IR_REG_INVALID) rv_store_vreg(tr, i->dst, RV_A0);
            break;

        case IR_ADD:
        case IR_ADDI: {
            if (i->src[0].is_imm) rv_movi(tr, RV_A0, i->src[0].imm);
            else                  rv_load_vreg(tr, RV_A0, i->src[0].reg);
            if (i->src[1].is_imm) {
                rv_addi(tr, RV_A0, RV_A0, (int32_t)i->src[1].imm);
            } else {
                rv_load_vreg(tr, RV_A1, i->src[1].reg);
                rv_add(tr, RV_A0, RV_A0, RV_A1);
            }
            if (i->dst != IR_REG_INVALID) rv_store_vreg(tr, i->dst, RV_A0);
            break;
        }

        case IR_SUB:
        case IR_SUBI: {
            if (i->src[0].is_imm) rv_movi(tr, RV_A0, i->src[0].imm);
            else                  rv_load_vreg(tr, RV_A0, i->src[0].reg);
            if (i->src[1].is_imm) {
                rv_addi(tr, RV_A0, RV_A0, -(int32_t)i->src[1].imm);
            } else {
                rv_load_vreg(tr, RV_A1, i->src[1].reg);
                rv_sub(tr, RV_A0, RV_A0, RV_A1);
            }
            if (i->dst != IR_REG_INVALID) rv_store_vreg(tr, i->dst, RV_A0);
            break;
        }

        case IR_AND:
            rv_load_vreg(tr, RV_A0, i->src[0].reg);
            rv_load_vreg(tr, RV_A1, i->src[1].reg);
            rv_and(tr, RV_A0, RV_A0, RV_A1);
            if (i->dst != IR_REG_INVALID) rv_store_vreg(tr, i->dst, RV_A0);
            break;

        case IR_OR:
            rv_load_vreg(tr, RV_A0, i->src[0].reg);
            rv_load_vreg(tr, RV_A1, i->src[1].reg);
            rv_or(tr, RV_A0, RV_A0, RV_A1);
            if (i->dst != IR_REG_INVALID) rv_store_vreg(tr, i->dst, RV_A0);
            break;

        case IR_XOR:
            rv_load_vreg(tr, RV_A0, i->src[0].reg);
            rv_load_vreg(tr, RV_A1, i->src[1].reg);
            rv_xor(tr, RV_A0, RV_A0, RV_A1);
            if (i->dst != IR_REG_INVALID) rv_store_vreg(tr, i->dst, RV_A0);
            break;

        case IR_SHL:
            rv_load_vreg(tr, RV_A0, i->src[0].reg);
            rv_load_vreg(tr, RV_A1, i->src[1].reg);
            rv_sll(tr, RV_A0, RV_A0, RV_A1);
            if (i->dst != IR_REG_INVALID) rv_store_vreg(tr, i->dst, RV_A0);
            break;

        case IR_SHR:
            rv_load_vreg(tr, RV_A0, i->src[0].reg);
            rv_load_vreg(tr, RV_A1, i->src[1].reg);
            rv_srl(tr, RV_A0, RV_A0, RV_A1);
            if (i->dst != IR_REG_INVALID) rv_store_vreg(tr, i->dst, RV_A0);
            break;

        case IR_MUL:
            rv_load_vreg(tr, RV_A0, i->src[0].reg);
            rv_load_vreg(tr, RV_A1, i->src[1].reg);
            rv_mul(tr, RV_A0, RV_A0, RV_A1);
            if (i->dst != IR_REG_INVALID) rv_store_vreg(tr, i->dst, RV_A0);
            break;

        case IR_SYSCALL:
            rv_ecall(tr);
            break;

        case IR_JMP:
            rv_jal_nop(tr);
            break;

        case IR_BEQ: {
            rv_load_vreg(tr, RV_A0, i->src[0].reg);
            rv_load_vreg(tr, RV_A1, i->src[1].reg);
            emit_rv(tr, rv_b(8, RV_A1, RV_A0, 0, 0x63)); /* BEQ placeholder */
            break;
        }

        case IR_BNE: {
            rv_load_vreg(tr, RV_A0, i->src[0].reg);
            rv_load_vreg(tr, RV_A1, i->src[1].reg);
            emit_rv(tr, rv_b(8, RV_A1, RV_A0, 1, 0x63)); /* BNE placeholder */
            break;
        }

        case IR_BLT: {
            rv_load_vreg(tr, RV_A0, i->src[0].reg);
            rv_load_vreg(tr, RV_A1, i->src[1].reg);
            emit_rv(tr, rv_b(8, RV_A1, RV_A0, 4, 0x63)); /* BLT */
            break;
        }

        case IR_BGE: {
            rv_load_vreg(tr, RV_A0, i->src[0].reg);
            rv_load_vreg(tr, RV_A1, i->src[1].reg);
            emit_rv(tr, rv_b(8, RV_A1, RV_A0, 5, 0x63)); /* BGE */
            break;
        }

        case IR_RET:
            rv_emit_epilogue(tr, 64); /* placeholder nregs */
            break;

        default:
            rv_nop(tr);
            break;
    }
    return FEMU_OK;
}

/* ─── Main RISC-V codegen entry ──────────────────────────────────── */
int codegen_riscv64(FEMUTranslator *tr, const FEMUIRUnit *unit) {
    for (const IRBlock *b = unit->blocks; b; b = b->next) {
        uint32_t nregs = b->next_vreg + 4;
        rv_emit_prologue(tr, nregs);

        for (const IRInsn *i = b->head; i; i = i->next) {
            int r = codegen_riscv_insn(tr, i);
            if (r != FEMU_OK) return r;
            tr->host_insns++;
        }

        if (!b->tail || b->tail->op != IR_RET)
            rv_emit_epilogue(tr, nregs);
    }
    return FEMU_OK;
}
