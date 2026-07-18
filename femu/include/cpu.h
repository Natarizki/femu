/*
 * cpu.h — Generic CPU state for all guest architectures
 */

#ifndef FEMU_CPU_H
#define FEMU_CPU_H

#include "femu.h"

/* ─── Register counts ─────────────────────────────────────────────── */
#define X86_NUM_REGS      16   /* RAX..R15                            */
#define ARM_NUM_REGS      16   /* R0..R15 (R15=PC)                    */
#define ARM64_NUM_REGS    32   /* X0..X30 + SP                        */
#define RISCV_NUM_REGS    32   /* x0..x31                             */

/* ─── x86-64 register indices ──────────────────────────────────────── */
typedef enum {
    X86_RAX = 0, X86_RCX, X86_RDX, X86_RBX,
    X86_RSP,     X86_RBP, X86_RSI, X86_RDI,
    X86_R8,      X86_R9,  X86_R10, X86_R11,
    X86_R12,     X86_R13, X86_R14, X86_R15,
    X86_RIP,     /* program counter (not in gp array, kept separate) */
} X86Reg;

/* x86 EFLAGS bits */
#define X86_FLAG_CF  (1 << 0)  /* carry      */
#define X86_FLAG_PF  (1 << 2)  /* parity     */
#define X86_FLAG_AF  (1 << 4)  /* adjust     */
#define X86_FLAG_ZF  (1 << 6)  /* zero       */
#define X86_FLAG_SF  (1 << 7)  /* sign       */
#define X86_FLAG_OF  (1 << 11) /* overflow   */

typedef struct {
    uint64_t gpr[X86_NUM_REGS]; /* RAX..R15              */
    uint64_t rip;               /* instruction pointer   */
    uint64_t rflags;            /* EFLAGS/RFLAGS         */
    /* Segment registers (base addresses in 64-bit mode) */
    uint64_t cs, ds, es, fs, gs, ss;
    /* Control registers */
    uint64_t cr0, cr3, cr4;
} X86State;

/* ─── ARM (AArch32) register state ────────────────────────────────── */
typedef enum {
    ARM_R0 = 0, ARM_R1, ARM_R2,  ARM_R3,
    ARM_R4,     ARM_R5, ARM_R6,  ARM_R7,
    ARM_R8,     ARM_R9, ARM_R10, ARM_R11,
    ARM_R12,    ARM_SP, ARM_LR,  ARM_PC,
} ARMReg;

/* CPSR bits */
#define ARM_FLAG_N  (1 << 31)
#define ARM_FLAG_Z  (1 << 30)
#define ARM_FLAG_C  (1 << 29)
#define ARM_FLAG_V  (1 << 28)
#define ARM_FLAG_T  (1 << 5)   /* Thumb mode */

typedef struct {
    uint32_t gpr[ARM_NUM_REGS];
    uint32_t cpsr;             /* current program status register  */
    uint32_t spsr;             /* saved program status register     */
} ARMState;

/* ─── AArch64 register state ──────────────────────────────────────── */
typedef struct {
    uint64_t x[ARM64_NUM_REGS]; /* X0..X30, SP */
    uint64_t pc;
    uint32_t pstate;            /* NZCV + mode bits                 */
} ARM64State;

/* ─── RISC-V register state ───────────────────────────────────────── */
/* ABI register names */
typedef enum {
    RV_ZERO=0, RV_RA, RV_SP,  RV_GP,
    RV_TP,     RV_T0, RV_T1,  RV_T2,
    RV_S0,     RV_S1, RV_A0,  RV_A1,
    RV_A2,     RV_A3, RV_A4,  RV_A5,
    RV_A6,     RV_A7, RV_S2,  RV_S3,
    RV_S4,     RV_S5, RV_S6,  RV_S7,
    RV_S8,     RV_S9, RV_S10, RV_S11,
    RV_T3,     RV_T4, RV_T5,  RV_T6,
} RVReg;

typedef struct {
    uint64_t x[RISCV_NUM_REGS]; /* x0 always 0 */
    uint64_t pc;
    uint64_t csr[4096];          /* CSR space    */
} RVState;

/* ─── Generic CPU state (union over all architectures) ─────────────── */
typedef struct FEMUCPU {
    FEMUArch arch;
    union {
        X86State   x86;
        ARM64State arm64;
        ARMState   arm;
        RVState    rv;
    } state;

    /* Execution stats */
    uint64_t insn_count;
    uint64_t cycle_count;

    /* Pointer back to memory subsystem */
    struct FEMUMemory *mem;
} FEMUCPU;

/* ─── CPU API ─────────────────────────────────────────────────────── */
FEMUCPU    *cpu_create(FEMUArch arch, struct FEMUMemory *mem);
void        cpu_destroy(FEMUCPU *cpu);
void        cpu_reset(FEMUCPU *cpu);
void        cpu_set_pc(FEMUCPU *cpu, uint64_t pc);
uint64_t    cpu_get_pc(const FEMUCPU *cpu);
void        cpu_dump_state(const FEMUCPU *cpu, FILE *out);

/* Arch string helpers */
const char *cpu_reg_name_x86(int reg);
const char *cpu_reg_name_arm(int reg);
const char *cpu_reg_name_rv(int reg);

#endif /* FEMU_CPU_H */
