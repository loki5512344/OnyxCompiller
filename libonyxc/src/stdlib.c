/*
 * stdlib.c — malloc/free/exit/atoi/strtol.
 *
 * malloc uses a simple bump allocator on top of sbrk(). free is a no-op
 * (memory is reclaimed only on process exit). Suitable for short-lived
 * CLI tools like onyxcc; would need a real allocator for long-running
 * processes.
 */
#include "onyxc.h"

static void *g_heap_ptr = NULL;
static void *g_heap_end = NULL;

static void heap_init(void) {
    if (!g_heap_ptr) {
        g_heap_ptr = _onyx_sbrk(0);
        g_heap_end = g_heap_ptr;
    }
}

void *malloc(size_t n) {
    heap_init();
    /* Align to 16 bytes. */
    n = (n + 15) & ~15UL;
    /* Grow if needed. */
    if ((char *)g_heap_ptr + n > (char *)g_heap_end) {
        long inc = (long)(n - ((char *)g_heap_end - (char *)g_heap_ptr));
        /* Always grow in 64K chunks. */
        inc = (inc + 0xFFFF) & ~0xFFFFL;
        void *new_end = _onyx_sbrk(inc);
        if (new_end == (void *)-1) return NULL;
        g_heap_end = (char *)new_end + inc;
    }
    void *p = g_heap_ptr;
    g_heap_ptr = (char *)g_heap_ptr + n;
    return p;
}

void free(void *p) { (void)p; }

void *calloc(size_t n, size_t sz) {
    size_t total = n * sz;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *p, size_t n) {
    void *q = malloc(n);
    if (q && p) {
        /* We don't know old size; copy up to n. */
        memcpy(q, p, n);
    }
    return q;
}

void exit(int code) {
    _onyx_exit(code);
}

int atoi(const char *s) {
    int sign = 1;
    int v = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v * sign;
}

long strtol(const char *s, char **endp, int base) {
    long sign = 1;
    long v = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    if (base == 0) {
        if (*s == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (*s == '0') { base = 8; s++; }
        else base = 10;
    }
    while (*s) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        s++;
    }
    if (endp) *endp = (char *)s;
    return v * sign;
}
