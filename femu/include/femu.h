/*
 * femu.h — Fast Emulation (femu) main header
 *
 * femu: AOT-based virtual machine and code translator
 * Like QEMU (multi-arch emulation) + LLVM (code translation to binary)
 * but using Ahead-Of-Time compilation for maximum runtime speed.
 */

#ifndef FEMU_H
#define FEMU_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─── Version ──────────────────────────────────────────────────── */
#define FEMU_VERSION_MAJOR 0
#define FEMU_VERSION_MINOR 1
#define FEMU_VERSION_PATCH 0
#define FEMU_VERSION_STR   "0.1.0"

/* ─── Architecture IDs ─────────────────────────────────────────── */
typedef enum {
    ARCH_NONE   = 0,
    ARCH_X86    = 1,   /* 32-bit x86              */
    ARCH_X86_64 = 2,   /* 64-bit x86 (amd64)      */
    ARCH_ARM    = 3,   /* 32-bit ARM (v7)          */
    ARCH_ARM64  = 4,   /* 64-bit ARM (AArch64)     */
    ARCH_RISCV32 = 5,  /* 32-bit RISC-V            */
    ARCH_RISCV64 = 6,  /* 64-bit RISC-V            */
    ARCH_COUNT
} FEMUArch;

/* ─── Operating mode ────────────────────────────────────────────── */
typedef enum {
    MODE_SYSTEM = 0,   /* full system emulation (like qemu-system-*)  */
    MODE_USER   = 1,   /* user-space emulation  (like qemu-user-*)    */
    MODE_TRANSLATE = 2,/* AOT translate only, output binary           */
} FEMUMode;

/* ─── Global run configuration ──────────────────────────────────── */
typedef struct {
    FEMUArch   guest_arch;   /* architecture of guest binary          */
    FEMUArch   host_arch;    /* architecture of host (auto-detected)  */
    FEMUMode   mode;
    const char *input_file;  /* guest binary / kernel image           */
    const char *output_file; /* AOT output binary (translate mode)    */
    uint64_t   ram_size;     /* RAM in bytes (system mode)            */
    int        smp_cores;    /* vCPU count                            */
    bool       verbose;
    bool       debug;
    bool       dump_ir;      /* dump IR before codegen                */
} FEMUConfig;

/* ─── Error codes ────────────────────────────────────────────────── */
typedef enum {
    FEMU_OK           =  0,
    FEMU_ERR_ARGS     = -1,
    FEMU_ERR_ARCH     = -2,
    FEMU_ERR_MEMORY   = -3,
    FEMU_ERR_LOADER   = -4,
    FEMU_ERR_DECODE   = -5,
    FEMU_ERR_CODEGEN  = -6,
    FEMU_ERR_IO       = -7,
    FEMU_ERR_UNSUPPORTED = -8,
} FEMUError;

/* ─── Logging ─────────────────────────────────────────────────────── */
extern bool femu_verbose;
extern bool femu_debug;

#define femu_log(fmt, ...) \
    fprintf(stdout, "[femu] " fmt "\n", ##__VA_ARGS__)

#define femu_info(fmt, ...) \
    do { if (femu_verbose) fprintf(stdout, "[info]  " fmt "\n", ##__VA_ARGS__); } while(0)

#define femu_dbg(fmt, ...) \
    do { if (femu_debug) fprintf(stderr, "[debug] %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); } while(0)

#define femu_err(fmt, ...) \
    fprintf(stderr, "[error] " fmt "\n", ##__VA_ARGS__)

#define femu_warn(fmt, ...) \
    fprintf(stderr, "[warn]  " fmt "\n", ##__VA_ARGS__)

/* ─── Utility macros ─────────────────────────────────────────────── */
#define FEMU_ALIGN(x, a)    (((x) + (a) - 1) & ~((a) - 1))
#define FEMU_MB(x)          ((uint64_t)(x) * 1024 * 1024)
#define FEMU_GB(x)          ((uint64_t)(x) * 1024 * 1024 * 1024)
#define FEMU_ARRAY_SIZE(a)  (sizeof(a) / sizeof((a)[0]))

/* ─── Forward declarations ────────────────────────────────────────── */
struct FEMUMemory;
struct FEMUCPU;
struct FEMUTranslator;
struct FEMUIRUnit;

/* ─── Arch name lookup ────────────────────────────────────────────── */
const char *femu_arch_name(FEMUArch arch);
FEMUArch    femu_arch_from_string(const char *name);
FEMUArch    femu_host_arch(void);

#endif /* FEMU_H */
