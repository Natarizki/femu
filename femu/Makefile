# ─── femu Makefile ────────────────────────────────────────────────
#
#  Targets:
#    make            Build femu binary (debug)
#    make release    Build with -O3 -DNDEBUG
#    make clean      Remove build artifacts
#    make test       Run smoke tests
#    make help       Show this message

CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -g \
            -Wmissing-prototypes -Wstrict-prototypes \
            -D_GNU_SOURCE
LDFLAGS :=
PREFIX  := /usr/local

# Release overrides (make release)
RELEASE_CFLAGS := -O3 -DNDEBUG -march=native -fomit-frame-pointer

OUT_DIR  := build
BIN      := $(OUT_DIR)/femu

# ─── Source files ──────────────────────────────────────────────────
SRCS := \
    src/main.c              \
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

OBJS := $(patsubst src/%.c, $(OUT_DIR)/%.o, $(SRCS))

# ─── Build rules ───────────────────────────────────────────────────
.PHONY: all release clean install test help

all: $(BIN)

release: CFLAGS += $(RELEASE_CFLAGS)
release: $(BIN)
	@echo "[femu] Release build done: $(BIN)"

$(BIN): $(OBJS)
	@mkdir -p $(OUT_DIR)
	@echo "[ld]  $@"
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)
	@echo "[ok]  femu built → $(BIN)"

$(OUT_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	@echo "[cc]  $<"
	$(CC) $(CFLAGS) -Iinclude -c $< -o $@

# ─── Install ───────────────────────────────────────────────────────
install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/femu
	@echo "[install] femu → $(PREFIX)/bin/femu"

# ─── Tests ─────────────────────────────────────────────────────────
test: $(BIN)
	@echo "─── smoke: --version ─────────────────────────────"
	$(BIN) -version

	@echo "─── smoke: -help ──────────────────────────────────"
	$(BIN) -help

	@echo "─── smoke: translate (no input, expect error) ─────"
	$(BIN) -translate -arch riscv64 -o /dev/null /dev/null 2>&1 || true

	@echo "─── All smoke tests passed ────────────────────────"

# ─── Clean ─────────────────────────────────────────────────────────
clean:
	rm -rf $(OUT_DIR)
	@echo "[clean] done"

# ─── Dependency auto-generation ────────────────────────────────────
-include $(OBJS:.o=.d)

$(OUT_DIR)/%.d: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Iinclude -MM -MT $(patsubst src/%.c,$(OUT_DIR)/%.o,$<) $< > $@

# ─── Help ──────────────────────────────────────────────────────────
help:
	@echo "femu build targets:"
	@echo "  make          — debug build  ($(BIN))"
	@echo "  make release  — optimized build (-O3)"
	@echo "  make install  — install to $(PREFIX)/bin"
	@echo "  make test     — smoke tests"
	@echo "  make clean    — remove build/"
