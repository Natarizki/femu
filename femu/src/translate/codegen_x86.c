/*
 * codegen_x86.c — IR → x86-64 host binary code generator
 *
 * Translates femu IR instructions into raw x86-64 machine code bytes.
 * Uses a simple register allocation: IR vregs are spilled to a stack
 * frame and loaded/stored as needed.
 *
 * Register mapping:
 *   RAX = accumulator / result
 *   RCX = operand B
 *   RDX = scratch
 *
 * All guest GPRs live in a contiguous array on the host stack:
 *   [RBP - (vreg+1)*8]
 */

#include "translate.h"

/* ─── Emit helpers ───────────────────────────────────────────────── */
#define EMIT1(b)          tr_emit_byte(tr, (b))
#define EMIT2(a,b)        do { EMIT1(a); EMIT1(b); } while(0)
#define EMIT3(a,b,c)      do { EMIT1(a); EMIT1(b); EMIT1(c); } while(0)
#define EMIT4(a,b,c,d)    do { EMIT3(a,b,c); EMIT1(d); } while(0)
#define EMIT_U32(v)       tr_emit_u32(tr, (v))
#define EMIT_U64(v)       tr_emit_u64(tr, (v))
#define EMIT_I32(v)       tr_emit_u32(tr, (uint32_t)(int32_t)(v))

#define REX_W64           0x48
#define MODRM_RR(reg,rm)  (uint8_t)(0xC0 | ((reg)<<3) | (rm))

#define ENC_RAX  0
#define ENC_RCX  1
#define ENC_RDX  2

/* ─── Stack-slot layout ──────────────────────────────────────────── */
#define MAX_VREG_SLOTS 4096
#define VREG_SLOT(v)   (-(int32_t)((v) + 1) * 8)

static void emit_load_vreg(FEMUTranslator *tr, IRReg v)
{
    if (v == IR_REG_INVALID) return;
    int32_t off = VREG_SLOT(v);
    EMIT3(REX_W64, 0x8B, 0x85);
    EMIT_I32(off);
}

static void emit_store_vreg(FEMUTranslator *tr, IRReg v)
{
    if (v == IR_REG_INVALID) return;
    int32_t off = VREG_SLOT(v);
    EMIT3(REX_W64, 0x89, 0x85);
    EMIT_I32(off);
}

static void emit_load_vreg_rcx(FEMUTranslator *tr, IRReg v)
{
    if (v == IR_REG_INVALID) return;
    int32_t off = VREG_SLOT(v);
    EMIT3(REX_W64, 0x8B, 0x8D);
    EMIT_I32(off);
}

static void emit_movi_rax(FEMUTranslator *tr, int64_t imm)
{
    EMIT2(REX_W64, 0xB8);
    EMIT_U64((uint64_t)imm);
}

static void emit_movi_rcx(FEMUTranslator *tr, int64_t imm)
{
    EMIT2(REX_W64, 0xB9);
    EMIT_U64((uint64_t)imm);
    (void)emit_movi_rcx; /* suppress unused warning if not called */
}

/* ─── Prologue / epilogue ────────────────────────────────────────── */
static void emit_prologue(FEMUTranslator *tr, uint32_t num_vregs)
{
    EMIT1(0x55);                         /* PUSH RBP */
    EMIT3(REX_W64, 0x89, 0xE5);          /* MOV RBP, RSP */
    uint32_t frame = FEMU_ALIGN((num_vregs + 1) * 8, 16);
    if (frame <= 127) {
        EMIT3(REX_W64, 0x83, 0xEC); EMIT1((uint8_t)frame);
    } else {
        EMIT3(REX_W64, 0x81, 0xEC); EMIT_U32(frame);
    }
}

static void emit_epilogue(FEMUTranslator *tr)
{
    EMIT3(REX_W64, 0x89, 0xEC);  /* MOV RSP, RBP */
    EMIT1(0x5D);                  /* POP RBP */
    EMIT1(0xC3);                  /* RET */
}

/* ─── Per-instruction codegen ────────────────────────────────────── */
static int codegen_x86_insn(FEMUTranslator *tr, const IRInsn *insn)
{
    switch (insn->op) {
        case IR_NOP:
        case IR_LABEL:
            EMIT1(0x90);
            break;

        case IR_MOVI:
            emit_movi_rax(tr, insn->src[0].imm);
            emit_store_vreg(tr, insn->dst);
            break;

        case IR_MOV:
            if (insn->src[0].is_imm) emit_movi_rax(tr, insn->src[0].imm);
            else                     emit_load_vreg(tr, insn->src[0].reg);
            emit_store_vreg(tr, insn->dst);
            break;

        case IR_ADD:
        case IR_ADDI: {
            if (insn->src[0].is_imm) emit_movi_rax(tr, insn->src[0].imm);
            else                     emit_load_vreg(tr, insn->src[0].reg);
            if (insn->src[1].is_imm) {
                EMIT3(REX_W64, 0x05, 0x00);
                EMIT_I32((int32_t)insn->src[1].imm);
            } else {
                emit_load_vreg_rcx(tr, insn->src[1].reg);
                EMIT3(REX_W64, 0x03, MODRM_RR(ENC_RAX, ENC_RCX));
            }
            emit_store_vreg(tr, insn->dst);
            break;
        }

        case IR_SUB:
        case IR_SUBI: {
            if (insn->src[0].is_imm) emit_movi_rax(tr, insn->src[0].imm);
            else                     emit_load_vreg(tr, insn->src[0].reg);
            if (insn->src[1].is_imm) {
                EMIT3(REX_W64, 0x2D, 0x00);
                EMIT_I32((int32_t)insn->src[1].imm);
            } else {
                emit_load_vreg_rcx(tr, insn->src[1].reg);
                EMIT3(REX_W64, 0x2B, MODRM_RR(ENC_RAX, ENC_RCX));
            }
            emit_store_vreg(tr, insn->dst);
            break;
        }

        case IR_AND:
            emit_load_vreg(tr, insn->src[0].reg);
            emit_load_vreg_rcx(tr, insn->src[1].reg);
            EMIT3(REX_W64, 0x23, MODRM_RR(ENC_RAX, ENC_RCX));
            emit_store_vreg(tr, insn->dst);
            break;

        case IR_OR:
            emit_load_vreg(tr, insn->src[0].reg);
            emit_load_vreg_rcx(tr, insn->src[1].reg);
            EMIT3(REX_W64, 0x0B, MODRM_RR(ENC_RAX, ENC_RCX));
            emit_store_vreg(tr, insn->dst);
            break;

        case IR_XOR:
            emit_load_vreg(tr, insn->src[0].reg);
            emit_load_vreg_rcx(tr, insn->src[1].reg);
            EMIT3(REX_W64, 0x33, MODRM_RR(ENC_RAX, ENC_RCX));
            emit_store_vreg(tr, insn->dst);
            break;

        case IR_SHL:
            emit_load_vreg(tr, insn->src[0].reg);
            if (insn->src[1].is_imm) {
                EMIT3(REX_W64, 0xC1, 0xE0);
                EMIT1((uint8_t)(insn->src[1].imm & 63));
            } else {
                emit_load_vreg_rcx(tr, insn->src[1].reg);
                EMIT3(REX_W64, 0xD3, 0xE0);
            }
            emit_store_vreg(tr, insn->dst);
            break;

        case IR_SHR:
            emit_load_vreg(tr, insn->src[0].reg);
            if (insn->src[1].is_imm) {
                EMIT3(REX_W64, 0xC1, 0xE8);
                EMIT1((uint8_t)(insn->src[1].imm & 63));
            } else {
                emit_load_vreg_rcx(tr, insn->src[1].reg);
                EMIT3(REX_W64, 0xD3, 0xE8);
            }
            emit_store_vreg(tr, insn->dst);
            break;

        case IR_MUL:
            emit_load_vreg(tr, insn->src[0].reg);
            emit_load_vreg_rcx(tr, insn->src[1].reg);
            EMIT4(REX_W64, 0x0F, 0xAF, MODRM_RR(ENC_RAX, ENC_RCX));
            emit_store_vreg(tr, insn->dst);
            break;

        case IR_RET:
            emit_epilogue(tr);
            break;

        case IR_SYSCALL:
            EMIT2(0x0F, 0x05);
            break;

        case IR_LOAD64:
            emit_load_vreg(tr, insn->src[0].reg);
            EMIT3(REX_W64, 0x8B, 0x00);  /* MOV RAX, [RAX] */
            emit_store_vreg(tr, insn->dst);
            break;

        case IR_STORE64: {
            emit_load_vreg(tr, insn->src[0].reg);
            EMIT3(REX_W64, 0x89, MODRM_RR(ENC_RAX, ENC_RDX)); /* MOV RDX, RAX */
            emit_load_vreg(tr, insn->src[1].reg);
            EMIT3(REX_W64, 0x89, 0x10);  /* MOV [RAX], RDX */
            break;
        }

        case IR_JMP:
            EMIT1(0xE9); EMIT_U32(0x00000000);
            break;

        case IR_BEQ:
        case IR_BNE:
            emit_load_vreg(tr, insn->src[0].reg);
            emit_load_vreg_rcx(tr, insn->src[1].reg);
            EMIT3(REX_W64, 0x3B, MODRM_RR(ENC_RAX, ENC_RCX));
            EMIT2(0x0F, (insn->op == IR_BEQ) ? 0x84 : 0x85);
            EMIT_U32(0x00000000);
            break;

        default:
            EMIT1(0x90);
            break;
    }
    return FEMU_OK;
}

/* ─── Main x86-64 codegen entry ──────────────────────────────────── */
int codegen_x86_64(FEMUTranslator *tr, const FEMUIRUnit *unit)
{
    for (const IRBlock *b = unit->blocks; b; b = b->next) {
        emit_prologue(tr, b->next_vreg < MAX_VREG_SLOTS
                          ? b->next_vreg : MAX_VREG_SLOTS);
        tr->host_insns++;

        for (const IRInsn *i = b->head; i; i = i->next) {
            int r = codegen_x86_insn(tr, i);
            if (r != FEMU_OK) return r;
            tr->host_insns++;
        }

        if (!b->tail || b->tail->op != IR_RET)
            emit_epilogue(tr);
    }
    return FEMU_OK;
}
