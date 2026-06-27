/* variadic_test.c — test variadic function arguments */
#include "onyxc.h"

int sum(int n, ...) {
    va_list args;
    va_start(args, n);
    int total = 0;
    for (int i = 0; i < n; i++) {
        total += va_arg(args, int);
    }
    va_end(args);
    return total;
}

void _start() {
    int r = sum(3, 10, 20, 30);
    /* r should be 60.
     *
     * FIX (v0.4): previously used __ecall1(93, r) — syscall 93 is Linux's
     * RISC-V exit, NOT OnyxOS's SYS_exit (which is 3). OnyxOS returned
     * -ENOSYS and the process hung. We now use _onyx_exit() which correctly
     * issues SYS_exit with the code in a0. */
    _onyx_exit(r);
}
