# OnyxCC — C/C++ → RISC-V64 → .onx compiler for OnyxOS
#
# Build targets:
#   make                 # build host onyxcc (Linux/x86_64)
#   make hello           # compile tests/hello_full.c → tests/hello_full.onx
#   make onyxcc-riscv    # cross-compile onyxcc → onyxcc.riscv.elf (for OnyxOS)
#   make onyxcc-onx      # convert onyxcc.riscv.elf → onyxcc.onx
#   make all-targets     # all of the above
#   make libonyxc        # build libonyxc (host verification)
#   make test            # compile and dump hello.onx header
#   make clean
#
# Host toolchain: any C compiler (gcc/clang) on Linux.
# Cross toolchain: clang-19 + lld (auto-detected).

CC      ?= gcc
CFLAGS  ?= -std=c99 -Wall -Wno-unused-function -Wno-unused-variable -Wno-stringop-truncation -O2 -g
LDFLAGS ?=

ONYXCC_SRCS = \
    src/main.c \
    src/util.c \
    src/lexer.c \
    src/pp.c \
    src/types.c \
    src/ast.c \
    src/parse.c \
    src/gen.c \
    src/riscv64.c \
    src/emit.c

ONYXCC_OBJS = $(ONYXCC_SRCS:.c=.o)

ONYXCC = onyxcc
LIBONYXC = libonyxc/libonyxc.a

# Cross-compilation toolchain for OnyxOS build.
CLANG        ?= clang-19
LLD          ?= ld.lld-19
ELF2ONX      ?= /home/z/my-project/onyx/OnyxKernel/target/release/elf2onx
RISCV_FLAGS  = --target=riscv64-unknown-elf -march=rv64gc -mabi=lp64d -mcmodel=medany \
	-Os -ffunction-sections -fdata-sections \
	-ffreestanding -nostdlib -fno-builtin -Iinclude -Wall -Wno-unused-function \
	-Wno-unused-variable -Wno-unused-but-set-variable -Wno-incompatible-pointer-types

.PHONY: all clean test libonyxc hello onyxcc-riscv onyxcc-onx all-targets

all: $(ONYXCC)

$(ONYXCC): $(ONYXCC_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -Iinclude -c -o $@ $<

# Cross-compile onyxcc itself to RISC-V64 ELF (for OnyxOS).
# Uses clang-19 + lld with a freestanding libc shim (src/shim.c).
onyxcc-riscv: onyxcc.riscv.elf

onyxcc.riscv.elf: $(ONYXCC_SRCS) src/shim.c linker_onyx.ld
	$(CLANG) $(RISCV_FLAGS) \
	-Wl,-T,linker_onyx.ld -Wl,--gc-sections -Wl,--strip-all -Wl,-n \
	$(ONYXCC_SRCS) src/shim.c -o $@
	@ls -la $@
	@echo "--- onyxcc.riscv.elf ready. Convert to .onx with: make onyxcc-onx"

# Convert onyxcc.riscv.elf → onyxcc.onx using OnyxKernel's elf2onx tool.
onyxcc-onx: onyxcc.riscv.elf
	@if [ ! -x "$(ELF2ONX)" ]; then \
	echo "Error: elf2onx not found at $(ELF2ONX)"; \
	echo "Build it: cd ../OnyxKernel && cargo build --release -p onyx_tools"; \
	exit 1; \
	fi
	$(ELF2ONX) --ring=1 onyxcc.riscv.elf onyxcc.onx
	@ls -la onyxcc.onx
	@echo "--- onyxcc.onx ready — drop into OnyxFS image to run on OnyxOS"

# Compile libonyxc as a host-side static archive (verification only).
libonyxc:
	$(MAKE) -C libonyxc CC=$(CC) CFLAGS="$(CFLAGS)"

# Build hello.onx using our onyxcc.
hello: $(ONYXCC)
	./$(ONYXCC) -v -I libonyxc/include -o tests/hello_full.onx tests/hello_full.c

# Dump .onx header for inspection.
test: hello
	@echo "--- hello_full.onx header ---"
	@od -A x -t x1z -v -N 64 tests/hello_full.onx
	@echo "--- file size ---"
	@wc -c tests/hello_full.onx

all-targets: $(ONYXCC) hello onyxcc-onx
	@echo "All targets built."
	@echo "  Linux binary:   $(ONYXCC)"
	@echo "  RISC-V ELF:     onyxcc.riscv.elf"
	@echo "  OnyxOS binary:  onyxcc.onx"
	@echo "  Test program:   tests/hello_full.onx"

clean:
	rm -f $(ONYXCC) $(ONYXCC_OBJS) tests/*.onx onyxcc.riscv.elf onyxcc.onx
