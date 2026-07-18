/*
 * translate.c — AOT translation pipeline core
 */

#include <sys/mman.h>
#include <errno.h>
#include "../include/translate.h"

/* ─── Lifecycle ─────────────────────────────────────────────────── */
FEMUTranslator *translator_create(FEMUConfig *cfg, FEMUMemory *mem) {
    FEMUTranslator *tr = calloc(1, sizeof(FEMUTranslator));
    if (!tr) { femu_err("translator_create: OOM"); return NULL; }
    tr->cfg  = cfg;
    tr->mem  = mem;
    tr->unit = ir_unit_create(cfg->guest_arch, cfg->host_arch);
    if (!tr->unit) { free(tr); return NULL; }

    /* Initial code buffer: 4 MB */
    tr->code_cap = 4 * 1024 * 1024;
    tr->code_buf = mmap(NULL, tr->code_cap,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tr->code_buf == MAP_FAILED) {
        femu_err("translator_create: code buf mmap failed");
        ir_unit_destroy(tr->unit); free(tr); return NULL;
    }
    tr->code_size = 0;
    return tr;
}

void translator_destroy(FEMUTranslator *tr) {
    if (!tr) return;
    if (tr->code_buf && tr->code_buf != MAP_FAILED)
        munmap(tr->code_buf, tr->code_cap);
    ir_unit_destroy(tr->unit);
    free(tr);
}

/* ─── Code buffer emission ───────────────────────────────────────── */
static int tr_grow(FEMUTranslator *tr, size_t need) {
    if (tr->code_size + need <= tr->code_cap) return 0;
    size_t new_cap = tr->code_cap * 2;
    while (new_cap < tr->code_size + need) new_cap *= 2;
    uint8_t *nb = mmap(NULL, new_cap,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (nb == MAP_FAILED) { femu_err("tr_grow: OOM"); return -1; }
    memcpy(nb, tr->code_buf, tr->code_size);
    munmap(tr->code_buf, tr->code_cap);
    tr->code_buf = nb;
    tr->code_cap = new_cap;
    return 0;
}

int tr_emit_byte(FEMUTranslator *tr, uint8_t b) {
    if (tr_grow(tr, 1) < 0) return -1;
    tr->code_buf[tr->code_size++] = b;
    return 0;
}
int tr_emit_u16(FEMUTranslator *tr, uint16_t v) {
    if (tr_grow(tr, 2) < 0) return -1;
    memcpy(tr->code_buf + tr->code_size, &v, 2);
    tr->code_size += 2; return 0;
}
int tr_emit_u32(FEMUTranslator *tr, uint32_t v) {
    if (tr_grow(tr, 4) < 0) return -1;
    memcpy(tr->code_buf + tr->code_size, &v, 4);
    tr->code_size += 4; return 0;
}
int tr_emit_u64(FEMUTranslator *tr, uint64_t v) {
    if (tr_grow(tr, 8) < 0) return -1;
    memcpy(tr->code_buf + tr->code_size, &v, 8);
    tr->code_size += 8; return 0;
}
int tr_emit_bytes(FEMUTranslator *tr, const uint8_t *data, size_t len) {
    if (tr_grow(tr, len) < 0) return -1;
    memcpy(tr->code_buf + tr->code_size, data, len);
    tr->code_size += len; return 0;
}

/* ─── Decoder / codegen table ────────────────────────────────────── */
DecodeFn translator_get_decoder(FEMUArch arch) {
    switch (arch) {
        case ARCH_X86:    return decode_x86;
        case ARCH_X86_64: return decode_x86_64;
        case ARCH_ARM:    return decode_arm;
        case ARCH_ARM64:  return decode_arm64;
        case ARCH_RISCV32:return decode_riscv32;
        case ARCH_RISCV64:return decode_riscv64;
        default:          return NULL;
    }
}

CodegenFn translator_get_codegen(FEMUArch arch) {
    switch (arch) {
        case ARCH_X86:
        case ARCH_X86_64: return codegen_x86_64;
        case ARCH_ARM:
        case ARCH_ARM64:  return codegen_arm64;
        case ARCH_RISCV32:
        case ARCH_RISCV64:return codegen_riscv64;
        default:          return NULL;
    }
}

/* ─── Main AOT run ───────────────────────────────────────────────── */
/*
 * AOT pipeline:
 *   1. Scan text section, split into basic blocks at control flow
 *   2. Decode each block: guest bytes → IR
 *   3. Run IR optimizer
 *   4. Codegen: IR → host binary
 */
int translator_run(FEMUTranslator *tr, uint64_t entry_pc, uint64_t text_end) {
    DecodeFn  decode  = translator_get_decoder(tr->cfg->guest_arch);
    CodegenFn codegen = translator_get_codegen(tr->cfg->host_arch);

    if (!decode) {
        femu_err("No decoder for guest arch '%s'",
                 femu_arch_name(tr->cfg->guest_arch));
        return FEMU_ERR_UNSUPPORTED;
    }
    if (!codegen) {
        femu_err("No codegen for host arch '%s'",
                 femu_arch_name(tr->cfg->host_arch));
        return FEMU_ERR_UNSUPPORTED;
    }

    femu_info("AOT decode: entry=0x%llx  end=0x%llx",
              (unsigned long long)entry_pc,
              (unsigned long long)text_end);

    /*
     * Simple linear scan: start at entry_pc, decode instructions
     * until we reach text_end or hit an unconditional branch.
     * A more complete implementation would use a worklist of
     * reachable PCs, but linear scan covers most binaries well.
     */
    uint64_t pc = entry_pc;
    IRBlock *cur_block = NULL;

    while (pc < text_end) {
        /* Begin a new basic block */
        cur_block = ir_block_create(tr->unit, pc);
        if (!cur_block) return FEMU_ERR_MEMORY;

        bool block_done = false;
        while (pc < text_end && !block_done) {
            /* Fetch up to 16 bytes for decoding */
            uint8_t buf[16];
            size_t  avail = (text_end - pc < 16) ? (size_t)(text_end - pc) : 16;
            uint8_t *hp = mem_get_host_ptr(tr->mem, pc, avail, MEM_READ);
            if (!hp) {
                if (mem_read(tr->mem, pc, buf, avail) < 0) break;
                hp = buf;
            }

            int consumed = decode(tr, cur_block, pc, hp, avail);
            if (consumed <= 0) {
                femu_warn("Decode failed at 0x%llx (consumed=%d); skipping",
                          (unsigned long long)pc, consumed);
                pc++;
                break;
            }
            tr->guest_insns++;
            pc += (uint64_t)consumed;

            /* Check if the last emitted insn is a control-flow terminator */
            if (cur_block->tail) {
                IROp last = cur_block->tail->op;
                if (last == IR_RET || last == IR_JMP) block_done = true;
            }
        }
        cur_block->guest_pc_end = pc;
        tr->blocks++;

        femu_dbg("Block 0x%llx..0x%llx  %u insns",
                 (unsigned long long)cur_block->guest_pc_start,
                 (unsigned long long)cur_block->guest_pc_end,
                 cur_block->num_insns);
    }

    femu_info("Decode done: %llu blocks, %llu guest insns",
              (unsigned long long)tr->blocks,
              (unsigned long long)tr->guest_insns);

    /* 3. Optimize IR */
    femu_info("Running IR optimizer...");
    ir_opt_run(tr->unit);

    /* 4. Codegen */
    femu_info("Codegen (%s)...", femu_arch_name(tr->cfg->host_arch));
    int r = codegen(tr, tr->unit);
    if (r != FEMU_OK) { femu_err("Codegen failed"); return r; }

    femu_info("Codegen done: %zu host bytes emitted", tr->code_size);
    return FEMU_OK;
}

/* ─── Write output binary ────────────────────────────────────────── */
int translator_write_output(FEMUTranslator *tr, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        femu_err("Cannot open output '%s': %s", path, strerror(errno));
        return FEMU_ERR_IO;
    }
    /*
     * For now, write a flat raw binary.
     * A production implementation would wrap in an ELF with proper
     * sections, symbol table, and plt/got stubs for syscall shims.
     */
    size_t n = fwrite(tr->code_buf, 1, tr->code_size, f);
    fclose(f);
    if (n != tr->code_size) {
        femu_err("Short write to '%s'", path); return FEMU_ERR_IO;
    }
    return FEMU_OK;
}

/* ─── Stats ──────────────────────────────────────────────────────── */
void translator_print_stats(const FEMUTranslator *tr, FILE *out) {
    fprintf(out, "\n=== femu AOT Stats ===\n");
    fprintf(out, "  Guest arch    : %s\n", femu_arch_name(tr->cfg->guest_arch));
    fprintf(out, "  Host arch     : %s\n", femu_arch_name(tr->cfg->host_arch));
    fprintf(out, "  Basic blocks  : %llu\n",   (unsigned long long)tr->blocks);
    fprintf(out, "  Guest insns   : %llu\n",   (unsigned long long)tr->guest_insns);
    fprintf(out, "  Host bytes    : %zu\n",     tr->code_size);
    if (tr->guest_insns > 0) {
        fprintf(out, "  Bytes/guest insn: %.1f\n",
                (double)tr->code_size / (double)tr->guest_insns);
    }
    fprintf(out, "======================\n\n");
}
