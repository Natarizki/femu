/*
 * cpu.c — Generic CPU state implementation
 */

#include "../include/cpu.h"
#include "../include/memory.h"

/* ─── Arch helpers ───────────────────────────────────────────────── */
const char *femu_arch_name(FEMUArch arch) {
    switch (arch) {
        case ARCH_X86:     return "x86";
        case ARCH_X86_64:  return "x86_64";
        case ARCH_ARM:     return "arm";
        case ARCH_ARM64:   return "arm64";
        case ARCH_RISCV32: return "riscv32";
        case ARCH_RISCV64: return "riscv64";
        default:           return "unknown";
    }
}

FEMUArch femu_arch_from_string(const char *name) {
    if (!name) return ARCH_NONE;
    if (strcmp(name, "x86")     == 0) return ARCH_X86;
    if (strcmp(name, "x86_64")  == 0 ||
        strcmp(name, "amd64")   == 0 ||
        strcmp(name, "x64")     == 0) return ARCH_X86_64;
    if (strcmp(name, "arm")     == 0 ||
        strcmp(name, "armv7")   == 0) return ARCH_ARM;
    if (strcmp(name, "arm64")   == 0 ||
        strcmp(name, "aarch64") == 0) return ARCH_ARM64;
    if (strcmp(name, "riscv32") == 0 ||
        strcmp(name, "rv32")    == 0) return ARCH_RISCV32;
    if (strcmp(name, "riscv64") == 0 ||
        strcmp(name, "rv64")    == 0) return ARCH_RISCV64;
    /* gencode-style aliases */
    if (strcmp(name, "risc-v")  == 0 ||
        strcmp(name, "riscv")   == 0) return ARCH_RISCV64;
    if (strcmp(name, "x86-64")  == 0) return ARCH_X86_64;
    return ARCH_NONE;
}

FEMUArch femu_host_arch(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return ARCH_X86_64;
#elif defined(__i386__) || defined(_M_IX86)
    return ARCH_X86;
#elif defined(__aarch64__) || defined(_M_ARM64)
    return ARCH_ARM64;
#elif defined(__arm__) || defined(_M_ARM)
    return ARCH_ARM;
#elif defined(__riscv) && __riscv_xlen == 64
    return ARCH_RISCV64;
#elif defined(__riscv) && __riscv_xlen == 32
    return ARCH_RISCV32;
#else
    return ARCH_NONE;
#endif
}

/* ─── CPU lifecycle ──────────────────────────────────────────────── */
FEMUCPU *cpu_create(FEMUArch arch, FEMUMemory *mem) {
    FEMUCPU *cpu = calloc(1, sizeof(FEMUCPU));
    if (!cpu) { femu_err("cpu_create: OOM"); return NULL; }
    cpu->arch = arch;
    cpu->mem  = mem;
    cpu_reset(cpu);
    return cpu;
}

void cpu_destroy(FEMUCPU *cpu) { free(cpu); }

void cpu_reset(FEMUCPU *cpu) {
    if (!cpu) return;
    switch (cpu->arch) {
        case ARCH_X86:
        case ARCH_X86_64:
            memset(&cpu->state.x86, 0, sizeof(cpu->state.x86));
            /* EFLAGS: bit 1 is always 1 */
            cpu->state.x86.rflags = 0x2;
            break;
        case ARCH_ARM:
            memset(&cpu->state.arm, 0, sizeof(cpu->state.arm));
            /* Supervisor mode */
            cpu->state.arm.cpsr = 0x13;
            break;
        case ARCH_ARM64:
            memset(&cpu->state.arm64, 0, sizeof(cpu->state.arm64));
            break;
        case ARCH_RISCV32:
        case ARCH_RISCV64:
            memset(&cpu->state.rv, 0, sizeof(cpu->state.rv));
            /* x0 is always 0 — enforced by ISA */
            break;
        default:
            break;
    }
    cpu->insn_count  = 0;
    cpu->cycle_count = 0;
}

void cpu_set_pc(FEMUCPU *cpu, uint64_t pc) {
    switch (cpu->arch) {
        case ARCH_X86:
        case ARCH_X86_64:  cpu->state.x86.rip  = pc; break;
        case ARCH_ARM:     cpu->state.arm.gpr[ARM_PC] = (uint32_t)pc; break;
        case ARCH_ARM64:   cpu->state.arm64.pc  = pc; break;
        case ARCH_RISCV32:
        case ARCH_RISCV64: cpu->state.rv.pc     = pc; break;
        default: break;
    }
}

uint64_t cpu_get_pc(const FEMUCPU *cpu) {
    switch (cpu->arch) {
        case ARCH_X86:
        case ARCH_X86_64:  return cpu->state.x86.rip;
        case ARCH_ARM:     return cpu->state.arm.gpr[ARM_PC];
        case ARCH_ARM64:   return cpu->state.arm64.pc;
        case ARCH_RISCV32:
        case ARCH_RISCV64: return cpu->state.rv.pc;
        default:           return 0;
    }
}

/* ─── Register name tables ───────────────────────────────────────── */
static const char *x86_reg_names[] = {
    "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
    "r8", "r9", "r10","r11","r12","r13","r14","r15","rip"
};
static const char *arm_reg_names[] = {
    "r0","r1","r2","r3","r4","r5","r6","r7",
    "r8","r9","r10","r11","r12","sp","lr","pc"
};
static const char *rv_reg_names[] = {
    "zero","ra","sp","gp","tp","t0","t1","t2",
    "s0",  "s1","a0","a1","a2","a3","a4","a5",
    "a6",  "a7","s2","s3","s4","s5","s6","s7",
    "s8",  "s9","s10","s11","t3","t4","t5","t6"
};

const char *cpu_reg_name_x86(int r) {
    return (r >= 0 && r < 17) ? x86_reg_names[r] : "??";
}
const char *cpu_reg_name_arm(int r) {
    return (r >= 0 && r < 16) ? arm_reg_names[r] : "??";
}
const char *cpu_reg_name_rv(int r) {
    return (r >= 0 && r < 32) ? rv_reg_names[r] : "??";
}

/* ─── State dump ─────────────────────────────────────────────────── */
void cpu_dump_state(const FEMUCPU *cpu, FILE *out) {
    fprintf(out, "=== CPU State [%s] ===\n", femu_arch_name(cpu->arch));
    switch (cpu->arch) {
        case ARCH_X86:
        case ARCH_X86_64: {
            const X86State *s = &cpu->state.x86;
            for (int i = 0; i < 16; i++) {
                fprintf(out, "  %-4s = 0x%016llx", x86_reg_names[i],
                        (unsigned long long)s->gpr[i]);
                if (i % 2 == 1) fprintf(out, "\n");
            }
            fprintf(out, "  rip  = 0x%016llx\n", (unsigned long long)s->rip);
            fprintf(out, "  rflags = 0x%08llx  (CF=%d ZF=%d SF=%d OF=%d)\n",
                    (unsigned long long)s->rflags,
                    !!(s->rflags & X86_FLAG_CF),
                    !!(s->rflags & X86_FLAG_ZF),
                    !!(s->rflags & X86_FLAG_SF),
                    !!(s->rflags & X86_FLAG_OF));
            break;
        }
        case ARCH_ARM: {
            const ARMState *s = &cpu->state.arm;
            for (int i = 0; i < 16; i++) {
                fprintf(out, "  %-4s = 0x%08x", arm_reg_names[i], s->gpr[i]);
                if (i % 4 == 3) fprintf(out, "\n");
            }
            fprintf(out, "  cpsr = 0x%08x\n", s->cpsr);
            break;
        }
        case ARCH_ARM64: {
            const ARM64State *s = &cpu->state.arm64;
            for (int i = 0; i < 31; i++) {
                fprintf(out, "  x%-2d  = 0x%016llx", i,
                        (unsigned long long)s->x[i]);
                if (i % 2 == 1) fprintf(out, "\n");
            }
            fprintf(out, "  sp   = 0x%016llx\n",
                    (unsigned long long)s->x[31]);
            fprintf(out, "  pc   = 0x%016llx\n",
                    (unsigned long long)s->pc);
            break;
        }
        case ARCH_RISCV32:
        case ARCH_RISCV64: {
            const RVState *s = &cpu->state.rv;
            for (int i = 0; i < 32; i++) {
                fprintf(out, "  %-5s= 0x%016llx", rv_reg_names[i],
                        (unsigned long long)s->x[i]);
                if (i % 2 == 1) fprintf(out, "\n");
            }
            fprintf(out, "  pc   = 0x%016llx\n",
                    (unsigned long long)s->pc);
            break;
        }
        default:
            fprintf(out, "  (no state for arch %d)\n", cpu->arch);
    }
    fprintf(out, "  insns: %llu  cycles: %llu\n",
            (unsigned long long)cpu->insn_count,
            (unsigned long long)cpu->cycle_count);
}
