/*
 * ir_opt.c — IR Optimization passes
 *
 * Passes (in order):
 *   1. Constant folding   — evaluate operations on immediates at compile time
 *   2. Copy propagation   — replace copies of a value with the original
 *   3. Dead code elim     — remove instructions whose result is never used
 */

#include "translate.h"

/* ─── Pass 1: Constant folding ───────────────────────────────────── */
int ir_opt_const_fold(IRBlock *b) {
    int changed = 0;
    for (IRInsn *i = b->head; i; i = i->next) {
        /* Skip non-arithmetic or non-immediate ops */
        if (i->src[0].is_imm && i->src[1].is_imm) {
            int64_t a = i->src[0].imm;
            int64_t c = i->src[1].imm;
            int64_t result = 0;
            bool    fold   = true;

            switch (i->op) {
                case IR_ADD:  result = a + c;  break;
                case IR_SUB:  result = a - c;  break;
                case IR_MUL:  result = a * c;  break;
                case IR_AND:  result = a & c;  break;
                case IR_OR:   result = a | c;  break;
                case IR_XOR:  result = a ^ c;  break;
                case IR_SHL:  result = a << (c & 63); break;
                case IR_SHR:  result = (int64_t)((uint64_t)a >> (c & 63)); break;
                case IR_SAR:  result = a >> (c & 63); break;
                case IR_DIV:  if (c) { result = a / c; } else fold = false; break;
                case IR_MOD:  if (c) { result = a % c; } else fold = false; break;
                default:      fold = false; break;
            }
            if (fold) {
                i->op        = IR_MOVI;
                i->src[0]    = IR_OP_IMM(result);
                i->src[1]    = (IROperand){0};
                changed++;
                femu_dbg("const_fold: folded to #%lld", (long long)result);
            }
        }

        /* Fold unary with immediate */
        if (i->op == IR_NEG && i->src[0].is_imm) {
            i->op     = IR_MOVI;
            i->src[0] = IR_OP_IMM(-i->src[0].imm);
            changed++;
        }
        if (i->op == IR_NOT && i->src[0].is_imm) {
            i->op     = IR_MOVI;
            i->src[0] = IR_OP_IMM(~i->src[0].imm);
            changed++;
        }

        /* Algebraic identities */
        if (!i->src[0].is_imm && i->src[1].is_imm) {
            int64_t k = i->src[1].imm;
            if (i->op == IR_ADD && k == 0) { i->op = IR_MOV; i->src[1] = (IROperand){0}; changed++; }
            if (i->op == IR_MUL && k == 1) { i->op = IR_MOV; i->src[1] = (IROperand){0}; changed++; }
            if (i->op == IR_MUL && k == 0) { i->op = IR_MOVI; i->src[0] = IR_OP_IMM(0); i->src[1] = (IROperand){0}; changed++; }
            if (i->op == IR_AND && k == 0) { i->op = IR_MOVI; i->src[0] = IR_OP_IMM(0); i->src[1] = (IROperand){0}; changed++; }
        }
    }
    return changed;
}

/* ─── Pass 2: Copy propagation ───────────────────────────────────── */
#define MAX_VREGS 4096

int ir_opt_copy_prop(IRBlock *b) {
    if (b->next_vreg == 0) return 0;

    /* copy_of[v] = w means v is a direct copy of w, or IR_REG_INVALID */
    uint32_t *copy_of = calloc(b->next_vreg, sizeof(uint32_t));
    if (!copy_of) return 0;
    for (uint32_t i = 0; i < b->next_vreg; i++) copy_of[i] = IR_REG_INVALID;

    int changed = 0;

    /* Build copy map */
    for (IRInsn *i = b->head; i; i = i->next) {
        if (i->op == IR_MOV && !i->src[0].is_imm && i->dst != IR_REG_INVALID) {
            uint32_t src = i->src[0].reg;
            /* Chase chain */
            while (src < b->next_vreg && copy_of[src] != IR_REG_INVALID)
                src = copy_of[src];
            copy_of[i->dst] = src;
        }
    }

    /* Apply substitutions */
    for (IRInsn *i = b->head; i; i = i->next) {
        for (int k = 0; k < 2; k++) {
            if (!i->src[k].is_imm && i->src[k].reg != IR_REG_INVALID) {
                uint32_t r = i->src[k].reg;
                if (r < b->next_vreg && copy_of[r] != IR_REG_INVALID) {
                    i->src[k].reg = copy_of[r];
                    changed++;
                }
            }
        }
    }

    free(copy_of);
    return changed;
}

/* ─── Pass 3: Dead code elimination ──────────────────────────────── */
int ir_opt_dead_code(IRBlock *b) {
    if (b->next_vreg == 0) return 0;

    /* use_count[v] = number of uses of vreg v */
    uint32_t *use_count = calloc(b->next_vreg, sizeof(uint32_t));
    if (!use_count) return 0;

    /* Count uses */
    for (IRInsn *i = b->head; i; i = i->next) {
        for (int k = 0; k < 2; k++) {
            if (!i->src[k].is_imm && i->src[k].reg != IR_REG_INVALID)
                if (i->src[k].reg < b->next_vreg)
                    use_count[i->src[k].reg]++;
        }
    }

    /* Mark dead instructions (result unused, no side effects) */
    int changed = 0;
    for (IRInsn *i = b->head; i; i = i->next) {
        if (i->dst == IR_REG_INVALID) continue;
        /* Has side effects — never elim */
        if (i->op == IR_STORE8  || i->op == IR_STORE16 ||
            i->op == IR_STORE32 || i->op == IR_STORE64 ||
            i->op == IR_SYSCALL || i->op == IR_CALL    ||
            i->op == IR_RET     || i->op == IR_JMP     ||
            i->op == IR_BEQ     || i->op == IR_BNE     ||
            i->op == IR_BLT     || i->op == IR_BGE     ||
            i->op == IR_BLTU    || i->op == IR_BGEU)
            continue;
        if (i->dst < b->next_vreg && use_count[i->dst] == 0) {
            i->op  = IR_NOP;
            i->dst = IR_REG_INVALID;
            changed++;
        }
    }

    free(use_count);
    return changed;
}

/* ─── Run all passes ─────────────────────────────────────────────── */
int ir_opt_run(FEMUIRUnit *unit) {
    int total = 0;
    for (IRBlock *b = unit->blocks; b; b = b->next) {
        /* Iterate until fixed point (max 8 rounds) */
        for (int round = 0; round < 8; round++) {
            int ch = 0;
            ch += ir_opt_const_fold(b);
            ch += ir_opt_copy_prop(b);
            ch += ir_opt_dead_code(b);
            total += ch;
            if (ch == 0) break;
        }
    }
    femu_info("IR optimizer: %d transformations applied", total);
    return total;
}
