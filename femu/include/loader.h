/*
 * loader.h — Binary / ELF loader for femu
 *
 * Loads guest binaries (ELF32 / ELF64) into the virtual memory map
 * and returns the entry point + text segment bounds for the AOT translator.
 */

#ifndef FEMU_LOADER_H
#define FEMU_LOADER_H

#include "femu.h"
#include "memory.h"

/* ─── ELF magic + constants ────────────────────────────────────────── */
#define ELF_MAGIC   "\x7f""ELF"
#define ELF_CLASS32  1
#define ELF_CLASS64  2
#define ELF_DATA_LSB 1    /* little-endian */
#define ELF_DATA_MSB 2    /* big-endian    */
#define ELF_TYPE_EXEC 2
#define ELF_TYPE_DYN  3

/* ELF program-header types */
#define PT_LOAD   1
#define PT_INTERP 3

/* ELF machine codes */
#define EM_386    3
#define EM_ARM    40
#define EM_X86_64 62
#define EM_AARCH64 183
#define EM_RISCV   243

/* ─── Parsed ELF info ─────────────────────────────────────────────── */
typedef struct {
    uint64_t  entry;         /* entry point virtual address           */
    uint64_t  text_start;    /* lowest PT_LOAD (exec) vaddr           */
    uint64_t  text_end;      /* highest PT_LOAD (exec) vaddr + size   */
    uint64_t  load_bias;     /* ASLR / PIE offset applied             */
    FEMUArch  arch;          /* detected guest architecture           */
    bool      is_64bit;
    bool      is_pie;        /* position-independent executable       */
} FEMUELFInfo;

/* ─── Raw binary info (non-ELF flat binaries) ────────────────────── */
typedef struct {
    uint64_t  load_addr;     /* address to load raw blob at           */
    uint64_t  entry;         /* entry point (= load_addr usually)     */
    size_t    size;
} FEMURawInfo;

/* ─── Loader result ───────────────────────────────────────────────── */
typedef struct {
    uint64_t entry_pc;
    uint64_t text_start;
    uint64_t text_end;
    FEMUArch arch;
    bool     is_elf;
} FEMULoadResult;

/* ─── Loader API ──────────────────────────────────────────────────── */

/* Load an ELF file into memory, return info */
int loader_load_elf(FEMUMemory *mem, const char *path,
                    FEMULoadResult *result);

/* Load a raw binary blob (kernel images, bare-metal) */
int loader_load_raw(FEMUMemory *mem, const char *path,
                    uint64_t load_addr, FEMULoadResult *result);

/* Auto-detect format and load */
int loader_load(FEMUMemory *mem, const char *path,
                FEMUArch hint_arch, FEMULoadResult *result);

/* ELF helper: detect arch from ELF header without full load */
FEMUArch loader_detect_arch(const char *path);

/* Dump loaded segments to stdout */
void loader_dump_info(const FEMULoadResult *res, FILE *out);

#endif /* FEMU_LOADER_H */
