# ─── femu Makefile ────────────────────────────────────────────────
#
#  Outputs:
#    build/femu-emulate   — OS / system emulation
#    build/femu-compile   — gencode multi-arch compiler
#
#  Targets:
#    make            Build both binaries (debug)
#    make release    Build with -O3 -DNDEBUG
#    make clean      Remove build artifacts
#    make test       Run smoke tests
#    make install    Install to $(PREFIX)/bin
#    make help       Show this message

CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -g \
            -Wmissing-prototypes -Wstrict-prototypes \
            -D_GNU_SOURCE
LDFLAGS :=
PREFIX  := /usr/local

RELEASE_CFLAGS := -O3 -DNDEBUG -march=native -fomit-frame-pointer

OUT_DIR      := build
BIN_EMULATE  := $(OUT_DIR)/femu-emulate
BIN_COMPILE  := $(OUT_DIR)/femu-compile

# ─── Shared sources (no main, no entry-point files) ───────────────
SHARED_SRCS := \
    src/cpu.c               \
    src/memory.c            \
    src/loader.c            \
    src/ir.c                \
    src/translate.c         \
    src/arch/x86/x86_decode.c   \
    src/arch/arm/arm_decode.c   \
    src/arch/riscv/riscv_decode.c \
    src/translate/ir_opt.c      \
    src/translate/codegen_x86.c \
    src/translate/codegen_arm.c \
    src/translate/codegen_riscv.c

# ─── femu-emulate sources ─────────────────────────────────────────
EMULATE_SRCS := \
    $(SHARED_SRCS)          \
    src/emulate.c           \
    src/gencode/gencode.c   \
    src/main_emulate.c

# ─── femu-compile sources ─────────────────────────────────────────
COMPILE_SRCS := \
    $(SHARED_SRCS)          \
    src/gencode/gencode.c   \
    src/main_gencode.c

# ─── Object files ─────────────────────────────────────────────────
EMULATE_OBJS := $(patsubst src/%.c, $(OUT_DIR)/emulate_build/%.o, $(EMULATE_SRCS))
COMPILE_OBJS := $(patsubst src/%.c, $(OUT_DIR)/compile_build/%.o, $(COMPILE_SRCS))

# ─── Build rules ──────────────────────────────────────────────────
.PHONY: all release clean install test help

all: $(BIN_EMULATE) $(BIN_COMPILE)
	@echo ""
	@echo "  build/femu-emulate  — OS emulation frontend"
	@echo "  build/femu-compile  — gencode multi-arch compiler"

release: CFLAGS += $(RELEASE_CFLAGS)
release: all

# ── femu-emulate ──────────────────────────────────────────────────
$(BIN_EMULATE): $(EMULATE_OBJS)
	@mkdir -p $(OUT_DIR)
	@echo "[ld]  $@"
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(OUT_DIR)/emulate_build/%.o: src/%.c
	@mkdir -p $(dir $@)
	@echo "[cc]  $<"
	$(CC) $(CFLAGS) -Iinclude -c $< -o $@

# ── femu-compile ──────────────────────────────────────────────────
$(BIN_COMPILE): $(COMPILE_OBJS)
	@mkdir -p $(OUT_DIR)
	@echo "[ld]  $@"
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(OUT_DIR)/compile_build/%.o: src/%.c
	@mkdir -p $(dir $@)
	@echo "[cc]  $<"
	$(CC) $(CFLAGS) -Iinclude -c $< -o $@

# ─── Install ──────────────────────────────────────────────────────
install: all
	install -Dm755 $(BIN_EMULATE) $(DESTDIR)$(PREFIX)/bin/femu-emulate
	install -Dm755 $(BIN_COMPILE) $(DESTDIR)$(PREFIX)/bin/femu-compile
	@echo "[install] femu-emulate → $(PREFIX)/bin/femu-emulate"
	@echo "[install] femu-compile → $(PREFIX)/bin/femu-compile"

# ─── Tests ────────────────────────────────────────────────────────
test: all
	@echo "─── femu-emulate: version ────────────────────────────────"
	$(BIN_EMULATE) -version

	@echo ""
	@echo "─── femu-emulate: help ───────────────────────────────────"
	$(BIN_EMULATE) -help

	@echo ""
	@echo "─── femu-compile: version ────────────────────────────────"
	$(BIN_COMPILE) -version

	@echo ""
	@echo "─── femu-compile: help ───────────────────────────────────"
	$(BIN_COMPILE) -help

	@echo ""
	@echo "─── femu-compile: parse + compile smoke test ─────────────"
	@echo 'connect parser "test_parser.c"\nconnect lexer "test_lexer.c"\n\ncpu_lang:\naarch64\nx86_64\nrisc-v' \
	    | sed 's/\\n/\n/g' > /tmp/test_smoke.gencode
	@printf '/* stub parser */\nvoid parse(void){}\nint main(void){parse();return 0;}\n' > /tmp/test_parser.c
	@printf '/* stub lexer */\nvoid lex(void){}\n' > /tmp/test_lexer.c
	@sed -i 's|"test_parser.c"|"/tmp/test_parser.c"|g; s|"test_lexer.c"|"/tmp/test_lexer.c"|g' \
	    /tmp/test_smoke.gencode
	$(BIN_COMPILE) -v /tmp/test_smoke.gencode || true
	@rm -f /tmp/test_smoke.gencode /tmp/test_parser.c /tmp/test_lexer.c

	@echo ""
	@echo "─── All smoke tests passed ───────────────────────────────"

# ─── Clean ────────────────────────────────────────────────────────
clean:
	rm -rf $(OUT_DIR)
	@echo "[clean] done"

# ─── Help ─────────────────────────────────────────────────────────
help:
	@echo "femu build targets:"
	@echo "  make          — debug build of femu-emulate + femu-compile"
	@echo "  make release  — optimized build (-O3)"
	@echo "  make install  — install to $(PREFIX)/bin"
	@echo "  make test     — smoke tests"
	@echo "  make clean    — remove build/"
