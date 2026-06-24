/* OnyxCC feature test: switch/case, goto, variadic, BSS globals */
#include <stdint.h>
#include <stdarg.h>

static int g_counter = 0;       /* goes to .bss */
static char g_buf[4096];        /* 4 KB in .bss */
static int g_table[256];        /* 1 KB in .bss */
static long g_accum = 0;        /* .bss */

static int classify(int x) {
    switch (x) {
        case 0:
        case 1:
            return 100;
        case 2:
            return 200;
        case 3:
            goto special;
        default:
            return -1;
    }
special:
    g_counter++;
    return 999;
}

static int sum_variadic(int n, ...) {
    va_list ap;
    va_start(ap, n);
    int total = 0;
    for (int i = 0; i < n; i++) {
        total += va_arg(ap, int);
    }
    va_end(ap);
    return total;
}

static int compute_lookup(int idx) {
    if (idx < 0 || idx >= 256) return -1;
    return g_table[idx];
}

int main(void) {
    /* Fill BSS array */
    for (int i = 0; i < 256; i++) {
        g_table[i] = i * 2;
    }

    /* Switch/case/goto */
    int a = classify(0);   /* 100 */
    int b = classify(2);   /* 200 */
    int c = classify(3);   /* 999, g_counter becomes 1 */
    int d = classify(99);  /* -1 */

    /* Variadic */
    int s = sum_variadic(4, 10, 20, 30, 40);  /* 100 */

    /* BSS access */
    g_buf[0] = 'O';
    g_buf[1] = 'K';
    g_buf[2] = 0;

    /* Accumulate */
    g_accum = (long)a + b + c + d + s + compute_lookup(50);

    return (int)g_accum;
}
