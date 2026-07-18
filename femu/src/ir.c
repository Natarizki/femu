/*
 * ir.c — femu Intermediate Representation implementation
 */

#include "../include/ir.h"

/* ─── Op name table ──────────────────────────────────────────────── */
static const char *op_names[] = {
    [IR_MOV]    = "mov",
    [IR_MOVI]   = "movi",
    [IR_ADD]    = "add",    [IR_ADDI]   = "addi",
    [IR_SUB]    = "sub",    [IR_SUBI]   = "subi",
    [IR_MUL]    = "mul",    [IR_DIV]    = "div",
    [IR_DIVU]   = "divu",   [IR_MOD]    = "mod",
    [IR_NEG]    = "neg",
    [IR_AND]    = "and",    [IR_OR]     = "or",
    [IR_XOR]    = "xor",    [IR_NOT]    = "not",
    [IR_SHL]    = "shl",    [IR_SHR]    = "shr",
    [IR_SAR]    = "sar",
    [IR_LOAD8]  = "ld8",    [IR_LOAD16] = "ld16",
    [IR_LOAD32] = "ld32",   [IR_LOAD64] = "ld64",
    [IR_STORE8] = "st8",    [IR_STORE16]= "st16",
    [IR_STORE32]= "st32",   [IR_STORE64]= "st64",
    [IR_JMP]    = "jmp",
    [IR_BEQ]    = "beq",    [IR_BNE]    = "bne",
    [IR_BLT]    = "blt",    [IR_BGE]    = "bge",
    [IR_BLTU]   = "bltu",   [IR_BGEU]   = "bgeu",
    [IR_CALL]   = "call",   [IR_RET]    = "ret",
    [IR_CMP_EQ] = "cmp.eq", [IR_CMP_NE] = "cmp.ne",
    [IR_CMP_LT] = "cmp.lt", [IR_CMP_GE] = "cmp.ge",
    [IR_CMP_LTU]= "cmp.ltu",[IR_CMP_GEU]= "cmp.geu",
    [IR_SEXT8]  = "sext8",  [IR_SEXT16] = "sext16",
    [IR_SEXT32] = "sext32",
    [IR_ZEXT8]  = "zext8",  [IR_ZEXT16] = "zext16",
    [IR_ZEXT32] = "zext32",
    [IR_SYSCALL]= "syscall",
    [IR_LABEL]  = "label",  [IR_NOP]    = "nop",
};

const char *ir_op_name(IROp op) {
    if (op < IR_COUNT && op_names[op]) return op_names[op];
    return "???";
}

/* ─── Unit / block lifecycle ─────────────────────────────────────── */
FEMUIRUnit *ir_unit_create(FEMUArch guest, FEMUArch host) {
    FEMUIRUnit *u = calloc(1, sizeof(FEMUIRUnit));
    if (!u) { femu_err("ir_unit_create: OOM"); return NULL; }
    u->guest_arch = guest;
    u->host_arch  = host;
    return u;
}

void ir_unit_destroy(FEMUIRUnit *unit) {
    if (!unit) return;
    IRBlock *b = unit->blocks;
    while (b) {
        IRBlock *nb = b->next;
        IRInsn  *i  = b->head;
        while (i) { IRInsn *ni = i->next; free(i); i = ni; }
        free(b);
        b = nb;
    }
    free(unit);
}

IRBlock *ir_block_create(FEMUIRUnit *unit, uint64_t guest_pc) {
    IRBlock *b = calloc(1, sizeof(IRBlock));
    if (!b) { femu_err("ir_block_create: OOM"); return NULL; }
    b->guest_pc_start = guest_pc;
    b->next_label     = 1;    /* 0 = no label */
    b->next_vreg      = 0;

    /* Append to unit's block list */
    if (!unit->blocks) {
        unit->blocks = b;
    } else {
        IRBlock *tail = unit->blocks;
        while (tail->next) tail = tail->next;
        tail->next = b;
    }
    unit->num_blocks++;
    return b;
}

/* ─── Allocators ─────────────────────────────────────────────────── */
IRReg ir_alloc_vreg(IRBlock *b) { return b->next_vreg++; }
uint32_t ir_alloc_label(IRBlock *b) { return b->next_label++; }

/* ─── Instruction emission ───────────────────────────────────────── */
static IRInsn *insn_alloc(IRBlock *b, IROp op) {
    IRInsn *i = calloc(1, sizeof(IRInsn));
    if (!i) { femu_err("insn_alloc: OOM"); return NULL; }
    i->op  = op;
    i->dst = IR_REG_INVALID;
    if (!b->head) { b->head = b->tail = i; }
    else          { b->tail->next = i; b->tail = i; }
    b->num_insns++;
    return i;
}

IRInsn *ir_emit(IRBlock *b, IROp op, IRReg dst, IROperand s0, IROperand s1) {
    IRInsn *i = insn_alloc(b, op);
    if (!i) return NULL;
    i->dst    = dst;
    i->src[0] = s0;
    i->src[1] = s1;
    return i;
}

IRInsn *ir_emit_movi(IRBlock *b, IRReg dst, int64_t imm) {
    IRInsn *i = insn_alloc(b, IR_MOVI);
    if (!i) return NULL;
    i->dst        = dst;
    i->src[0]     = IR_OP_IMM(imm);
    return i;
}

IRInsn *ir_emit_mov(IRBlock *b, IRReg dst, IRReg src) {
    IRInsn *i = insn_alloc(b, IR_MOV);
    if (!i) return NULL;
    i->dst    = dst;
    i->src[0] = IR_OP_REG(src);
    return i;
}

IRInsn *ir_emit_load(IRBlock *b, IROp op, IRReg dst, IRReg addr, int64_t off) {
    IRInsn *i = insn_alloc(b, op);
    if (!i) return NULL;
    i->dst    = dst;
    i->src[0] = IR_OP_REG(addr);
    i->src[1] = IR_OP_IMM(off);
    return i;
}

IRInsn *ir_emit_store(IRBlock *b, IROp op, IRReg src, IRReg addr, int64_t off) {
    IRInsn *i = insn_alloc(b, op);
    if (!i) return NULL;
    i->src[0] = IR_OP_REG(src);
    i->src[1] = IR_OP_REG(addr);
    /* offset stored in label field to avoid adding a 3rd operand */
    i->label  = (uint32_t)(int32_t)off;
    return i;
}

IRInsn *ir_emit_branch(IRBlock *b, IROp op, IRReg a, IRReg b_reg, uint32_t lbl) {
    IRInsn *i = insn_alloc(b, op);
    if (!i) return NULL;
    i->src[0] = IR_OP_REG(a);
    i->src[1] = IR_OP_REG(b_reg);
    i->label  = lbl;
    return i;
}

IRInsn *ir_emit_jmp(IRBlock *b, uint32_t label) {
    IRInsn *i = insn_alloc(b, IR_JMP);
    if (!i) return NULL;
    i->label = label;
    return i;
}

IRInsn *ir_emit_label(IRBlock *b, uint32_t label) {
    IRInsn *i = insn_alloc(b, IR_LABEL);
    if (!i) return NULL;
    i->label = label;
    return i;
}

IRInsn *ir_emit_ret(IRBlock *b) { return insn_alloc(b, IR_RET); }

IRInsn *ir_emit_syscall(IRBlock *b, IRReg num) {
    IRInsn *i = insn_alloc(b, IR_SYSCALL);
    if (!i) return NULL;
    i->src[0] = IR_OP_REG(num);
    return i;
}

/* ─── Dump ───────────────────────────────────────────────────────── */
static void print_operand(const IROperand *op, FILE *out) {
    if (op->is_imm) fprintf(out, "#%lld", (long long)op->imm);
    else            fprintf(out, "v%u", op->reg);
}

void ir_dump_block(const IRBlock *b, FILE *out) {
    fprintf(out, "  Block [0x%016llx .. 0x%016llx]  %u insns\n",
            (unsigned long long)b->guest_pc_start,
            (unsigned long long)b->guest_pc_end,
            b->num_insns);
    for (const IRInsn *i = b->head; i; i = i->next) {
        if (i->op == IR_LABEL) {
            fprintf(out, "  .L%u:\n", i->label); continue;
        }
        fprintf(out, "    ");
        if (i->dst != IR_REG_INVALID)
            fprintf(out, "v%-3u = ", i->dst);
        else
            fprintf(out, "         ");
        fprintf(out, "%-10s", ir_op_name(i->op));
        if (i->op == IR_JMP || i->op == IR_CALL) {
            fprintf(out, " .L%u", i->label);
        } else if (i->op == IR_BEQ || i->op == IR_BNE ||
                   i->op == IR_BLT || i->op == IR_BGE ||
                   i->op == IR_BLTU|| i->op == IR_BGEU) {
            print_operand(&i->src[0], out);
            fprintf(out, ", ");
            print_operand(&i->src[1], out);
            fprintf(out, ", .L%u", i->label);
        } else {
            if (i->src[0].reg != IR_REG_INVALID || i->src[0].is_imm)
                print_operand(&i->src[0], out);
            if (i->src[1].reg != IR_REG_INVALID || i->src[1].is_imm) {
                fprintf(out, ", ");
                print_operand(&i->src[1], out);
            }
        }
        if (i->guest_pc) fprintf(out, "  ; pc=0x%llx", (unsigned long long)i->guest_pc);
        fprintf(out, "\n");
    }
}

void ir_dump_unit(const FEMUIRUnit *unit, FILE *out) {
    fprintf(out, "=== IR Unit [%s → %s]  %u blocks ===\n",
            femu_arch_name(unit->guest_arch),
            femu_arch_name(unit->host_arch),
            unit->num_blocks);
    for (const IRBlock *b = unit->blocks; b; b = b->next)
        ir_dump_block(b, out);
    fprintf(out, "=== end IR ===\n");
}
