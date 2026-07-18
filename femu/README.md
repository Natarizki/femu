# femu — Fast Emulation

**femu** is an AOT (Ahead-Of-Time) multi-architecture emulator and binary translator written in pure C.

- Runs OS images and user-space programs on **foreign architectures** (x86, ARM, RISC-V)
- Translates the entire guest binary to host native code **before execution** — zero per-instruction translation overhead at runtime
- Two focused CLI tools: `femu-emulate` (OS/system emulation) and `femu-compile` (language frontend → multi-target AOT)

> **Why AOT instead of JIT?**  
> JIT translates blocks on demand at runtime. AOT pays the translation cost once, upfront, and saves a native binary you can re-run without ever touching the translator again — ideal for workloads that run repeatedly (servers, embedded systems, cross-compilation pipelines).

---

## AOT Pipeline

```
Guest Binary (ELF)
       │
       ▼  [ Loader ]
   Guest bytes in virtual memory
       │
       ▼  [ Decoder — arch-specific ]
   IR  (flat 3-address code, ~50 opcodes)
       │
       ▼  [ IR Optimizer ]
   Optimized IR  (constant folding · copy propagation · DCE)
       │
       ▼  [ Codegen — arch-specific ]
   Host Binary (raw machine code / ELF)
```

---

## Supported Architectures

| Guest (input)  | Host (output) |
|----------------|---------------|
| x86 / x86-64   | x86-64        |
| ARM / AArch64  | AArch64       |
| RISC-V 32/64   | RISC-V 64     |

---

## Build

```bash
cd femu

# Debug build (default)
make

# Release build (optimized)
make release

# Install to /usr/local/bin
sudo make install

# Run smoke tests
make test

# Clean build artifacts
make clean
```

Binaries are placed in `build/`:

| Binary          | Purpose                                      |
|-----------------|----------------------------------------------|
| `femu-emulate`  | OS / system / user-space emulation           |
| `femu-compile`  | Compile a language frontend to multiple CPUs |

---

## femu-emulate

QEMU-style flags for running OS images and user-space programs on a different architecture.

```
femu-emulate [options] <binary>

Options:
  -os <name>       Guest OS / mode: linux | bare
  -hda <file>      Primary disk image
  -hdb <file>      Secondary disk image
  -vga <type>      VGA type: std | virtio | none
  -cpu <arch>      Guest CPU: x86 | x86_64 | arm | arm64 | riscv32 | riscv64
  -m <size>        RAM size (e.g. 128M, 1G)  [default: 128M]
  -smp <n>         Number of vCPUs           [default: 1]
  -v               Verbose logging
  -d               Debug output
  -version         Print version and exit
  -help            Show this help
```

### Examples

```bash
# Run an ARM64 ELF on an x86-64 host
femu-emulate -cpu arm64 -m 128M ./hello_arm64

# Boot a RISC-V kernel image with a disk
femu-emulate -cpu riscv64 -os linux -m 256M -hda disk.img vmlinuz-riscv

# Full VirtIO display
femu-emulate -cpu x86_64 -vga virtio -m 512M system.img
```

---

## femu-compile

Reads a `.gencode` manifest file that describes a language frontend (parser, lexer, runtime sources) and compiles it to one or more target CPU architectures via AOT translation.

### `.gencode` manifest format

```
connect parser   "src/parser.c"
connect lexer    "src/lexer.c"
connect runtime  "src/runtime.c"

cpu_lang:
  x86_64
  arm64
  riscv64
```

`connect` lines wire source files into the compiler pipeline.  
`cpu_lang:` lists the target architectures to emit.

```
femu-compile [options] <manifest.gencode>

Options:
  -o <dir>         Output directory for compiled binaries [default: out/]
  -v               Verbose
  -d               Debug
  -version         Print version and exit
  -help            Show this help
```

### Example

```bash
femu-compile -o dist/ mylang.gencode
# Produces: dist/mylang-x86_64, dist/mylang-arm64, dist/mylang-riscv64
```

---

## Code Structure

```
femu/
├── include/
│   ├── femu.h            — Core types, FEMUArch, FEMUConfig, error codes, logging
│   ├── cpu.h             — Per-arch CPU state structs (X86State, ARMState, RVState)
│   ├── memory.h          — Virtual memory map API (RAM, ROM, MMIO regions)
│   ├── ir.h              — IR opcode enum, IRInsn, IRBlock, emit helpers
│   ├── translate.h       — AOT pipeline API, decoder/codegen function typedefs
│   ├── loader.h          — ELF32/ELF64 + raw binary loader
│   ├── emulate.h         — femu-emulate config and run API
│   └── gencode.h         — .gencode manifest parser and compile API
├── src/
│   ├── main_emulate.c    — femu-emulate entry point
│   ├── main_gencode.c    — femu-compile entry point
│   ├── cpu.c             — Arch name helpers, host arch detection
│   ├── memory.c          — mmap-backed memory regions, read/write/MMIO
│   ├── loader.c          — ELF PT_LOAD segment loader
│   ├── ir.c              — IR lifecycle, emit helpers, pretty-printer
│   ├── translate.c       — AOT pipeline core (decode → optimize → codegen)
│   ├── emulate.c         — femu-emulate runtime (memory setup, loader, AOT)
│   ├── arch/
│   │   ├── x86/x86_decode.c       — x86-64 → IR  (REX, ModRM, ALU, JCC, CALL, SYSCALL)
│   │   ├── arm/arm_decode.c       — AArch64 → IR (fixed-width, DP-imm/reg, B/BL, SVC)
│   │   └── riscv/riscv_decode.c   — RV64I → IR   (R/I/S/B/U/J types, ECALL)
│   ├── translate/
│   │   ├── ir_opt.c        — Constant folding, copy propagation, dead code elimination
│   │   ├── codegen_x86.c   — IR → x86-64 machine bytes (RBP stack-spill model)
│   │   ├── codegen_arm.c   — IR → AArch64 32-bit words (MOVZ/MOVK for 64-bit imm)
│   │   └── codegen_riscv.c — IR → RV64I 32-bit words  (LUI+ADDI, SP-relative LD/SD)
│   └── gencode/
│       └── gencode.c       — .gencode parser + multi-target AOT compile pipeline
└── Makefile
```

---

## IR — Intermediate Representation

femu IR is flat 3-address code (similar in spirit to LLVM IR, but minimal). Every instruction has the form `vN = op src0, src1`. Virtual registers spill to the stack — no register allocator required for v0.1.

```
; Example: decode RISC-V  ADD a0, a1, a2
  v0 = ld64   v410, #0      ; load x10 (a0)
  v1 = ld64   v411, #0      ; load x11 (a1)
  v2 = add    v0, v1
       mov    v410, v2      ; write back x10
```

**Opcodes:** `MOV MOVI ADD SUB MUL DIV AND OR XOR SHL SHR SAR`  
`LOAD8/16/32/64  STORE8/16/32/64`  
`JMP BEQ BNE BLT BGE CALL RET`  
`SEXT8/16/32  ZEXT8/16/32`  
`SYSCALL LABEL NOP`

---

## Optimizer Passes

| Pass              | What it does                                         |
|-------------------|------------------------------------------------------|
| Constant folding  | `ADD #3, #5` → `MOVI #8`                            |
| Copy propagation  | `v1 = MOV v0; ADD v1, x` → `ADD v0, x`              |
| Dead code elim    | Remove instructions whose result is never used       |

Passes run repeatedly until a fixed point is reached (max 8 iterations per block).

---

## AOT vs JIT

|                    | JIT (e.g. QEMU TCG)               | AOT (femu)                         |
|--------------------|-----------------------------------|------------------------------------|
| Translation timing | Runtime, per basic block          | Upfront, before first execution    |
| Startup latency    | Fast                              | Slower (full translate first)      |
| Runtime overhead   | Per-block compile cost            | **Zero** — code is already native  |
| Output             | In-memory only                    | Persistable native binary          |
| Optimization depth | Limited by time budget            | Unlimited (offline)                |

---

## License

Apache License 2.0 — see [LICENSE](LICENSE).
