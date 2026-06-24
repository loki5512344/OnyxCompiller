# libonyxc — minimal C library for OnyxOS userspace.
#
# Build (host verification, only C-files without inline asm):
#   make                 # compile only the portable files (string, ctype, stdlib)
#
# Use:
#   libonyxc sources are linked into user programs by onyxcc at compile time.
#   Place the libonyxc/include/ headers on the include path (-I libonyxc/include)
#   and compile the libonyxc/src/*.c together with your program's main.c.
#
#   Example:
#     onyxcc -I libonyxc/include -o prog.onx \
#         libonyxc/src/start.c libonyxc/src/syscalls.c \
#         libonyxc/src/stdio.c libonyxc/src/stdlib.c \
#         libonyxc/src/string.c libonyxc/src/ctype.c \
#         prog.c

CC      ?= gcc
CFLAGS  ?= -std=c99 -Wall -Wno-unused-function -Wno-unused-variable -O2 -g

# Files that use inline asm (RISC-V only) — excluded from host-check.
PORTABLE_SRCS = src/stdio.c src/stdlib.c src/string.c src/ctype.c

.PHONY: all clean

all: $(PORTABLE_SRCS:.c=.o)
	@echo "host-check OK: portable libonyxc files compile on $(CC)."

%.o: %.c
	$(CC) $(CFLAGS) -Iinclude -c -o $@ $<

clean:
	rm -f *.o src/*.o
