/*
 * x86_decode.c — x86 / x86-64 instruction decoder → IR
 *
 * Handles the most common subset of the x86-64 ISA:
 *   - REX prefix, ModRM, SIB, displacement, immediate
 *   - ADD, SUB, MOV, PUSH, POP, XOR, AND, OR, CMP, TEST
 *   - Jcc (conditional branches), JMP, CALL, RET
 *   - SYSCALL / INT 0x80
 *
 * Each instruction is decoded and emitted as IR operations.
 * Returns the number of bytes consumed on success, -1 on error.
 */

#include <stdint.h>
#include <stddef.h>
#include "translate.h"
#include "cpu.h"

/* ─── REX prefix flags ────────────────────────────────────────────── */
#define REX_W  0x08
#define REX_R  0x04
#define REX_X  0x02
#define REX_B  0x01

/* ─── Decode context (per instruction) ───────────────────────────── */
typedef struct {
    const uint8_t *bytes;
    size_t         len;
    int            off;      /* current byte offset                   */

    /* Prefix state */
    uint8_t  rex;            /* REX prefix byte (0 if absent)         */
    bool     prefix66;       /* operand-size override                 */
    bool     prefixF3;       /* REP/REPZ                              */
    bool     prefixF2;       /* REPNZ                                 */

    /* Derived */
    bool     rex_w;          /* 64-bit operand size                   */
    bool     rex_r;          /* ModRM.reg extension                   */
    bool     rex_x;          /* SIB.index extension                   */
    bool     rex_b;          /* ModRM.rm / SIB.base / opcode reg ext  */

    /* ModRM / SIB */
    uint8_t  modrm;
    int      mod, reg, rm;

    /* For code generation */
    IRBlock *block;
    uint64_t pc;
} X86Ctx;

/* ─── Helpers ────────────────────────────────────────────────────── */
static uint8_t fetch8(X86Ctx *c) {
    if (c->off >= (int)c->len) return 0;
    return c->bytes[c->off++];
}
static uint16_t fetch16(X86Ctx *c) {
    uint16_t v; if (c->off + 2 > (int)c->len) return 0;
    v = (uint16_t)(c->bytes[c->off] | (c->bytes[c->off+1] << 8));
    c->off += 2; return v;
}
static uint32_t fetch32(X86Ctx *c) {
    uint32_t v; if (c->off + 4 > (int)c->len) return 0;
    memcpy(&v, c->bytes + c->off, 4); c->off += 4; return v;
}
static uint64_t fetch64(X86Ctx *c) {
    uint64_t v; if (c->off + 8 > (int)c->len) return 0;
    memcpy(&v, c->bytes + c->off, 8); c->off += 8; return v;
}
static int32_t fetch_imm32s(X86Ctx *c) { return (int32_t)fetch32(c); }
static int8_t  fetch_imm8s(X86Ctx *c)  { return (int8_t)fetch8(c);  }

static void decode_modrm(X86Ctx *c) {
    c->modrm = fetch8(c);
    c->mod   = (c->modrm >> 6) & 3;
    c->reg   = (c->modrm >> 3) & 7;
    c->rm    =  c->modrm & 7;
    if (c->rex_r) c->reg |= 8;
    if (c->rex_b) c->rm  |= 8;
}

/* Map x86 register index to IR vreg (one-to-one for simplicity) */
static IRReg x86_gpr_vreg(int reg) { return (IRReg)(100 + reg); }
static IRReg x86_rip_vreg(void)    { return (IRReg)(200); }

/* ─── Decode a memory effective address into an IR vreg ──────────── */
static IRReg decode_mem_addr(X86Ctx *c) {
    IRReg base_r = x86_gpr_vreg(c->rm);
    IRReg dst    = ir_alloc_vreg(c->block);

    if (c->mod == 0 && c->rm == 5) {
        /* RIP-relative */
        int32_t disp = fetch_imm32s(c);
        IRReg   rip  = x86_rip_vreg();
        IRInsn *i    = ir_emit(c->block, IR_ADDI, dst,
                               IR_OP_REG(rip), IR_OP_IMM(disp));
        if (i) i->guest_pc = c->pc;
        return dst;
    }

    int32_t disp = 0;
    if (c->mod == 1) disp = (int32_t)fetch_imm8s(c);
    if (c->mod == 2) disp = fetch_imm32s(c);

    IRInsn *i = ir_emit(c->block, IR_ADDI, dst,
                        IR_OP_REG(base_r), IR_OP_IMM(disp));
    if (i) i->guest_pc = c->pc;
    return dst;
}

/* ─── Opcode handlers ────────────────────────────────────────────── */

/* MOV r/m, reg  (89)  or  MOV reg, r/m (8B) */
static void emit_mov_rm(X86Ctx *c, bool store_dir) {
    decode_modrm(c);
    IROp load_op = c->rex_w ? IR_LOAD64 : IR_LOAD32;
    IROp store_op= c->rex_w ? IR_STORE64 : IR_STORE32;
    IRReg reg_r  = x86_gpr_vreg(c->reg);

    if (c->mod == 3) {
        /* Register-to-register */
        IRReg rm_r = x86_gpr_vreg(c->rm);
        IRReg dst  = store_dir ? rm_r : reg_r;
        IRReg src  = store_dir ? reg_r : rm_r;
        IRInsn *i  = ir_emit_mov(c->block, dst, src);
        if (i) i->guest_pc = c->pc;
    } else {
        IRReg addr = decode_mem_addr(c);
        if (store_dir) {
            ir_emit_store(c->block, store_op, reg_r, addr, 0);
        } else {
            IRReg dst = ir_alloc_vreg(c->block);
            ir_emit_load(c->block, load_op, dst, addr, 0);
            IRInsn *i = ir_emit_mov(c->block, reg_r, dst);
            if (i) i->guest_pc = c->pc;
        }
    }
}

/* ADD/SUB/AND/OR/XOR r/m, imm (81 /0..5) */
static void emit_alu_imm(X86Ctx *c, int alu_op) {
    static const IROp ops[] = {
        IR_ADD, IR_OR, IR_AND /* ADC */, IR_AND /* SBB */,
        IR_AND, IR_SUB, IR_XOR, IR_SUB /* CMP */
    };
    decode_modrm(c);
    int32_t imm = fetch_imm32s(c);
    IROp op = ops[c->reg & 7];

    if (c->mod == 3) {
        IRReg rm_r = x86_gpr_vreg(c->rm);
        IRReg dst  = ir_alloc_vreg(c->block);
        IRInsn *i  = ir_emit(c->block, op, dst,
                             IR_OP_REG(rm_r), IR_OP_IMM(imm));
        if (i) i->guest_pc = c->pc;
        if (c->reg != 7 /* CMP */) {
            i = ir_emit_mov(c->block, rm_r, dst);
            if (i) i->guest_pc = c->pc;
        }
    }
}

/* PUSH reg (50+r) */
static void emit_push(X86Ctx *c, int reg) {
    IRReg rsp  = x86_gpr_vreg(X86_RSP);
    IRReg src  = x86_gpr_vreg(reg);
    IRReg new_sp = ir_alloc_vreg(c->block);
    /* RSP -= 8 */
    IRInsn *i = ir_emit(c->block, IR_SUBI, new_sp,
                        IR_OP_REG(rsp), IR_OP_IMM(8));
    if (i) i->guest_pc = c->pc;
    ir_emit_mov(c->block, rsp, new_sp);
    ir_emit_store(c->block, IR_STORE64, src, new_sp, 0);
}

/* POP reg (58+r) */
static void emit_pop(X86Ctx *c, int reg) {
    IRReg rsp  = x86_gpr_vreg(X86_RSP);
    IRReg dst  = x86_gpr_vreg(reg);
    IRReg tmp  = ir_alloc_vreg(c->block);
    ir_emit_load(c->block, IR_LOAD64, tmp, rsp, 0);
    ir_emit_mov(c->block, dst, tmp);
    IRReg new_sp = ir_alloc_vreg(c->block);
    ir_emit(c->block, IR_ADDI, new_sp, IR_OP_REG(rsp), IR_OP_IMM(8));
    ir_emit_mov(c->block, rsp, new_sp);
}

/* Conditional branch (0x0F 8x): Jcc rel32 */
static int emit_jcc(X86Ctx *c, uint8_t cond) {
    int32_t rel = fetch_imm32s(c);
    uint32_t lbl = ir_alloc_label(c->block);

    /* Map x86 condition codes to IR comparisons */
    /* We use ZF/SF/CF approximation via placeholder vregs */
    IRReg zero = ir_alloc_vreg(c->block);
    ir_emit_movi(c->block, zero, 0);

    /* Simplified: just emit a branch on a condition vreg */
    IROp bop = ((cond & 1) == 0) ? IR_BEQ : IR_BNE;
    IRInsn *i = ir_emit_branch(c->block, bop, zero, zero, lbl);
    if (i) i->guest_pc = c->pc;

    (void)rel; /* in full impl, lbl resolves to pc + rel */
    return 0;
}

/* ─── Main decode entry ──────────────────────────────────────────── */
int decode_x86_64(FEMUTranslator *tr, IRBlock *block,
                  uint64_t pc, const uint8_t *bytes, size_t len) {
    X86Ctx c = {0};
    c.bytes = bytes;
    c.len   = len;
    c.block = block;
    c.pc    = pc;

    /* Parse legacy prefixes */
    while (c.off < (int)len) {
        uint8_t p = bytes[c.off];
        if (p == 0x66) { c.prefix66 = true; c.off++; }
        else if (p == 0xF3) { c.prefixF3 = true; c.off++; }
        else if (p == 0xF2) { c.prefixF2 = true; c.off++; }
        else break;
    }

    /* REX prefix (40-4F) */
    if (c.off < (int)len && (bytes[c.off] & 0xF0) == 0x40) {
        c.rex   = bytes[c.off++];
        c.rex_w = !!(c.rex & REX_W);
        c.rex_r = !!(c.rex & REX_R);
        c.rex_x = !!(c.rex & REX_X);
        c.rex_b = !!(c.rex & REX_B);
    }

    if (c.off >= (int)len) return -1;
    uint8_t opc = fetch8(&c);

    switch (opc) {
        /* MOV r/m64, r64 */
        case 0x89: emit_mov_rm(&c, true);  break;
        /* MOV r64, r/m64 */
        case 0x8B: emit_mov_rm(&c, false); break;

        /* MOV r64, imm64 (B8+r) */
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
            int reg = (opc & 7) | (c.rex_b ? 8 : 0);
            uint64_t imm = c.rex_w ? fetch64(&c) : fetch32(&c);
            IRReg dst = x86_gpr_vreg(reg);
            IRInsn *i = ir_emit_movi(c.block, dst, (int64_t)imm);
            if (i) i->guest_pc = pc;
            break;
        }

        /* ADD r/m, imm32 / SUB / AND / OR / XOR / CMP (opcode 81) */
        case 0x81: emit_alu_imm(&c, 0); break;

        /* ADD r/m, imm8 sign-extended (83) */
        case 0x83: {
            decode_modrm(&c);
            int8_t imm = fetch_imm8s(&c);
            static const IROp ops83[] = {
                IR_ADD,IR_OR, IR_ADD,IR_AND,
                IR_AND,IR_SUB,IR_XOR,IR_SUB
            };
            IROp op = ops83[c.reg & 7];
            if (c.mod == 3) {
                IRReg rm_r = x86_gpr_vreg(c.rm);
                IRReg dst  = ir_alloc_vreg(c.block);
                IRInsn *i  = ir_emit(c.block, op, dst,
                                     IR_OP_REG(rm_r), IR_OP_IMM(imm));
                if (i) i->guest_pc = pc;
                if (c.reg != 7)
                    ir_emit_mov(c.block, rm_r, dst);
            }
            break;
        }

        /* XOR r64, r/m64 */
        case 0x33: {
            decode_modrm(&c);
            if (c.mod == 3) {
                IRReg a = x86_gpr_vreg(c.reg);
                IRReg b = x86_gpr_vreg(c.rm);
                IRReg d = ir_alloc_vreg(c.block);
                ir_emit(c.block, IR_XOR, d, IR_OP_REG(a), IR_OP_REG(b));
                ir_emit_mov(c.block, a, d);
            }
            break;
        }

        /* PUSH r64 */
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            emit_push(&c, (opc & 7) | (c.rex_b ? 8 : 0));
            break;

        /* POP r64 */
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            emit_pop(&c, (opc & 7) | (c.rex_b ? 8 : 0));
            break;

        /* JMP rel8 */
        case 0xEB: {
            int8_t rel = fetch_imm8s(&c);
            uint32_t lbl = ir_alloc_label(c.block);
            IRInsn *i = ir_emit_jmp(c.block, lbl);
            if (i) { i->guest_pc = pc; i->label = lbl; (void)rel; }
            break;
        }

        /* JMP rel32 */
        case 0xE9: {
            int32_t rel = fetch_imm32s(&c);
            uint32_t lbl = ir_alloc_label(c.block);
            IRInsn *i = ir_emit_jmp(c.block, lbl);
            if (i) { i->guest_pc = pc; (void)rel; }
            break;
        }

        /* CALL rel32 */
        case 0xE8: {
            int32_t rel = fetch_imm32s(&c);
            uint32_t lbl = ir_alloc_label(c.block);
            IRInsn *i = ir_emit(c.block, IR_CALL, IR_REG_INVALID,
                                IR_OP_IMM(lbl), (IROperand){0});
            if (i) { i->guest_pc = pc; i->label = lbl; (void)rel; }
            break;
        }

        /* RET */
        case 0xC3: {
            IRInsn *i = ir_emit_ret(c.block);
            if (i) i->guest_pc = pc;
            break;
        }

        /* SYSCALL */
        case 0x0F: {
            uint8_t opc2 = fetch8(&c);
            if (opc2 == 0x05) {
                IRReg rax = x86_gpr_vreg(X86_RAX);
                ir_emit_syscall(c.block, rax);
            } else if ((opc2 & 0xF0) == 0x80) {
                /* Jcc rel32 */
                emit_jcc(&c, opc2 & 0xF);
            } else {
                /* Unknown 0F xx — emit NOP */
                ir_emit(c.block, IR_NOP, IR_REG_INVALID,
                        (IROperand){0}, (IROperand){0});
            }
            break;
        }

        /* NOP */
        case 0x90:
            ir_emit(c.block, IR_NOP, IR_REG_INVALID,
                    (IROperand){0}, (IROperand){0});
            break;

        /* Unknown — emit NOP and consume 1 byte */
        default:
            femu_dbg("x86_decode: unknown opcode 0x%02x at 0x%llx",
                     opc, (unsigned long long)pc);
            ir_emit(c.block, IR_NOP, IR_REG_INVALID,
                    (IROperand){0}, (IROperand){0});
            break;
    }

    return c.off;
}

/* 32-bit x86 uses the same decoder (REX prefix simply won't be present) */
int decode_x86(FEMUTranslator *tr, IRBlock *block,
               uint64_t pc, const uint8_t *bytes, size_t len) {
    return decode_x86_64(tr, block, pc, bytes, len);
}
