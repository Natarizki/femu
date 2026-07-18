# femu — Fast Emulation

**femu** adalah virtual machine dan code translator AOT (Ahead-Of-Time) yang ditulis dalam C.

- Menjalankan OS dan program dengan **arsitektur berbeda** (seperti QEMU)
- Menerjemahkan kode guest ke native binary **sebelum eksekusi** (AOT, bukan JIT)
- Antarmuka **CLI seperti QEMU**

> **"fast emulation"** — AOT berarti seluruh kode diterjemahkan terlebih dahulu,
> sehingga runtime tidak ada overhead translasi per-instruksi seperti interpreter.

---

## Pipeline AOT

```
Guest Binary (ELF)
       │
       ▼  [ Loader ]
   Guest bytes di memori virtual
       │
       ▼  [ Decoder (arch-specific) ]
   IR — Intermediate Representation
       │
       ▼  [ IR Optimizer ]
   IR yang sudah dioptimasi (const folding, copy prop, DCE)
       │
       ▼  [ Codegen (arch-specific) ]
   Host Binary (raw / ELF)
```

---

## Arsitektur yang didukung

| Guest (input) | Host (output)  |
|---------------|----------------|
| x86 / x86-64  | x86-64         |
| ARM / AArch64  | AArch64        |
| RISC-V 32/64  | RISC-V 64      |

---

## Build

```bash
# Debug build
cd femu
make

# Release build (optimized)
make release

# Install ke /usr/local/bin
sudo make install

# Smoke tests
make test

# Clean
make clean
```

Binary dihasilkan di `build/femu`.

---

## Penggunaan (CLI)

```
femu [options] <binary>

Modes:
  -system              Full system emulation
  -user                User-space process emulation  (default)
  -translate           AOT translate only; write native binary

Options:
  -arch <name>         Guest architecture
                       x86 | x86_64 | arm | arm64 | riscv32 | riscv64
  -m <size>            RAM size  (e.g. 128M, 1G)    [default: 128M]
  -smp <n>             Number of vCPUs              [default: 1]
  -o <file>            Output binary (-translate mode)
  -dump-ir             Print IR sebelum codegen
  -v                   Verbose
  -d                   Debug output
  -version             Print version
  -h, -help            Help
```

---

## Contoh penggunaan

### 1. User-space emulation (jalankan program ARM di host x86)
```bash
femu -arch arm64 -user ./hello_arm64
```

### 2. System emulation (jalankan kernel)
```bash
femu -arch riscv64 -system -m 256M vmlinuz-riscv
```

### 3. AOT Translate only (translate RISC-V → native x86-64)
```bash
femu -arch riscv64 -translate -o hello_native hello_riscv.elf
./hello_native   # jalankan langsung tanpa emulator
```

### 4. Debug dengan IR dump
```bash
femu -arch arm64 -translate -dump-ir -o out.bin input.elf
```

---

## Struktur kode

```
femu/
├── include/
│   ├── femu.h          — types utama, config, error codes, logging
│   ├── cpu.h           — CPU state (X86State, ARMState, RVState, ...)
│   ├── memory.h        — virtual memory map (RAM, ROM, MMIO)
│   ├── ir.h            — Intermediate Representation (IR opcodes, insns, blocks)
│   ├── translate.h     — AOT pipeline, decoder/codegen function types
│   └── loader.h        — ELF32/ELF64 + raw binary loader
├── src/
│   ├── main.c          — CLI entry point (QEMU-style argument parsing)
│   ├── cpu.c           — CPU state helpers, arch detection
│   ├── memory.c        — mmap-backed memory regions, read/write
│   ├── loader.c        — ELF loader (PT_LOAD segments)
│   ├── ir.c            — IR instruction emission + dump
│   ├── translate.c     — AOT pipeline core (decode → opt → codegen)
│   ├── arch/
│   │   ├── x86/x86_decode.c     — x86-64 → IR decoder
│   │   ├── arm/arm_decode.c     — AArch64 → IR decoder
│   │   └── riscv/riscv_decode.c — RISC-V (RV64I) → IR decoder
│   └── translate/
│       ├── ir_opt.c        — Constant folding, copy propagation, DCE
│       ├── codegen_x86.c   — IR → x86-64 machine code
│       ├── codegen_arm.c   — IR → AArch64 machine code
│       └── codegen_riscv.c — IR → RISC-V machine code
├── Makefile
└── README.md
```

---

## IR — Intermediate Representation

IR femu adalah 3-address code flat (mirip LLVM IR tapi jauh lebih sederhana).
Setiap instruksi: `vN = op src0, src1`

```
; contoh: decode RISC-V ADD a0, a1, a2
  v0 = ld64    v410, #0     ; load x10 (a0)
  v1 = ld64    v411, #0     ; load x11 (a1)
  v2 = add     v0, v1
       mov     v410, v2     ; write back x10
```

IR Opcodes: `MOV, MOVI, ADD, SUB, MUL, DIV, AND, OR, XOR, SHL, SHR, SAR,
LOAD8/16/32/64, STORE8/16/32/64, JMP, BEQ, BNE, BLT, BGE, CALL, RET,
SEXT8/16/32, ZEXT8/16/32, SYSCALL, LABEL, NOP`

---

## Optimizer passes

| Pass           | Keterangan                                          |
|----------------|-----------------------------------------------------|
| Constant fold  | `ADD #3, #5` → `MOVI #8`                           |
| Copy prop      | `v1 = MOV v0; ADD v1, x` → `ADD v0, x`             |
| Dead code elim | Hapus instruksi yang hasilnya tidak pernah dipakai  |

Passes dijalankan berulang hingga fixed point (max 8 iterasi per block).

---

## Kenapa AOT dan bukan JIT?

| | JIT (seperti QEMU TCG) | AOT (femu) |
|-|------------------------|------------|
| Translasi | Saat runtime, per-block | Sebelum eksekusi, sekali |
| Startup | Lebih cepat | Lebih lambat (translate dulu) |
| Runtime | Ada overhead compile | **Zero overhead compile** |
| Output | Di memori saja | Bisa disimpan ke binary |
| Optimasi | Terbatas (time budget) | Bisa lebih dalam |

femu cocok untuk workload yang dijalankan berulang-ulang (server, embedded)
di mana biaya AOT dibayar sekali dan dihemat berkali-kali.

---

## Lisensi

MIT
