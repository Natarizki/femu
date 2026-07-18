/*
 * memory.h — Memory subsystem for femu
 *
 * Provides a flat address-space abstraction backed by mmap.
 * In system mode: full RAM + MMIO regions.
 * In user mode: mirrors host process pages.
 */

#ifndef FEMU_MEMORY_H
#define FEMU_MEMORY_H

#include "femu.h"
#include <sys/mman.h>

/* ─── Memory region flags ─────────────────────────────────────────── */
#define MEM_READ    (1 << 0)
#define MEM_WRITE   (1 << 1)
#define MEM_EXEC    (1 << 2)
#define MEM_MMIO    (1 << 3)   /* MMIO: read/write call callbacks    */
#define MEM_ROM     (MEM_READ | MEM_EXEC)
#define MEM_RAM     (MEM_READ | MEM_WRITE | MEM_EXEC)

#define FEMU_PAGE_SIZE  4096
#define FEMU_PAGE_MASK  (~(uint64_t)(FEMU_PAGE_SIZE - 1))
#define FEMU_PAGE_BITS  12

/* Maximum number of memory regions */
#define MEM_MAX_REGIONS 64

/* ─── MMIO callbacks ──────────────────────────────────────────────── */
typedef uint64_t (*mmio_read_fn)(void *opaque, uint64_t addr, unsigned size);
typedef void     (*mmio_write_fn)(void *opaque, uint64_t addr, uint64_t val, unsigned size);

/* ─── Memory region descriptor ────────────────────────────────────── */
typedef struct FEMUMemRegion {
    uint64_t     base;      /* guest physical base address            */
    uint64_t     size;      /* region size in bytes                   */
    uint8_t     *host_ptr;  /* host pointer (NULL for MMIO)           */
    uint32_t     flags;     /* MEM_READ | MEM_WRITE | MEM_EXEC | ...  */
    const char  *name;      /* human-readable label (e.g. "RAM")      */

    /* MMIO handlers (only used when MEM_MMIO set) */
    mmio_read_fn  read_cb;
    mmio_write_fn write_cb;
    void         *opaque;
} FEMUMemRegion;

/* ─── Memory subsystem ────────────────────────────────────────────── */
typedef struct FEMUMemory {
    FEMUMemRegion regions[MEM_MAX_REGIONS];
    int           num_regions;
    uint64_t      total_ram;   /* total mapped RAM bytes              */
} FEMUMemory;

/* ─── Memory API ──────────────────────────────────────────────────── */

/* Lifecycle */
FEMUMemory *mem_create(void);
void        mem_destroy(FEMUMemory *mem);

/* Region management */
int  mem_add_ram(FEMUMemory *mem, uint64_t base, uint64_t size, const char *name);
int  mem_add_rom(FEMUMemory *mem, uint64_t base, const void *data,
                 uint64_t size, const char *name);
int  mem_add_mmio(FEMUMemory *mem, uint64_t base, uint64_t size,
                  mmio_read_fn rfn, mmio_write_fn wfn,
                  void *opaque, const char *name);

/* Region lookup */
FEMUMemRegion *mem_find_region(FEMUMemory *mem, uint64_t addr);

/* Raw read/write (guest physical address) */
int      mem_write(FEMUMemory *mem, uint64_t gpa, const void *data, size_t len);
int      mem_read(FEMUMemory *mem, uint64_t gpa, void *out, size_t len);

/* Typed helpers */
uint8_t  mem_read8(FEMUMemory *mem,  uint64_t gpa);
uint16_t mem_read16(FEMUMemory *mem, uint64_t gpa);
uint32_t mem_read32(FEMUMemory *mem, uint64_t gpa);
uint64_t mem_read64(FEMUMemory *mem, uint64_t gpa);

void     mem_write8(FEMUMemory *mem,  uint64_t gpa, uint8_t  val);
void     mem_write16(FEMUMemory *mem, uint64_t gpa, uint16_t val);
void     mem_write32(FEMUMemory *mem, uint64_t gpa, uint32_t val);
void     mem_write64(FEMUMemory *mem, uint64_t gpa, uint64_t val);

/* Host pointer for a guest address (NULL if MMIO or unmapped) */
uint8_t *mem_get_host_ptr(FEMUMemory *mem, uint64_t gpa, size_t len, uint32_t flags);

/* Dump memory map to stdout */
void     mem_dump_map(const FEMUMemory *mem, FILE *out);

#endif /* FEMU_MEMORY_H */
