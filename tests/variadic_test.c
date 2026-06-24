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
    /* r should be 60 */
    __ecall1(93, r);
}
