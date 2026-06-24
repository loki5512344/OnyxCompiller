/*
 * hello.c — OnyxCC MVP smoke test.
 *
 * Compiles to a .onx that runs in Ring 2 (user space). Writes
 * "Hello, OnyxOS!\n" to stdout (fd=1, which the kernel routes to UART),
 * then exits with code 0.
 *
 * Built with libonyxc, which provides _start → main() → exit().
 */
#include <stdio.h>

int main(void) {
    printf("Hello, OnyxOS!\n");
    printf("From a C program compiled by onyxcc.\n");
    printf("Numbers: %d, %u, %x, %s, %c\n", -42, 42u, 0xDEAD, "ok", 'X');
    return 0;
}
