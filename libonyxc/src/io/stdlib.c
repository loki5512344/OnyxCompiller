/*
 * stdlib.c — malloc/free/exit/atoi/strtol/getenv (v0.4).
 *
 * malloc uses a simple bump allocator on top of sbrk(). free is a no-op
 * (memory is reclaimed only on process exit). Suitable for short-lived
 * CLI tools like onyxcc; would need a real allocator for long-running
 * processes.
 *
 * getenv / setenv operate on the `environ` array exported by start.c.
 * We provide a tiny linear scan — adequate for typical programs that
 * query only a handful of variables.
 */
#include "onyxc.h"

extern char **environ;

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
        /* FIX (v0.4): kernel returns (void *)-1 on failure (negative errno
         * was already converted by _onyx_sbrk). Old code compared against
         * -1 as a pointer, which is correct now — but we also need to handle
         * the case where sbrk returns NULL (shouldn't happen, but be safe). */
        if (new_end == (void *)-1 || new_end == NULL) return NULL;
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

/* ── Environment ──────────────────────────────────────────────────────── */

char *getenv(const char *name) {
    if (!environ || !name) return NULL;
    size_t name_len = strlen(name);
    for (char **e = environ; *e; e++) {
        char *entry = *e;
        /* Match "NAME=..." — name followed by '='. */
        if (strlen(entry) >= name_len &&
            memcmp(entry, name, name_len) == 0 &&
            entry[name_len] == '=') {
            return entry + name_len + 1;
        }
    }
    return NULL;
}

/* setenv is intentionally simple: we don't shrink environ, only append.
 * Existing variables with the same name are NOT replaced — callers that
 * need replacement should unsetenv first. */
int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !value) return -1;
    /* If already present and !overwrite, succeed without doing anything. */
    if (!overwrite && getenv(name)) return 0;
    /* Compose "NAME=VALUE" into a malloc'd buffer. */
    size_t name_len = strlen(name);
    size_t value_len = strlen(value);
    char *buf = malloc(name_len + value_len + 2);
    if (!buf) return -1;
    memcpy(buf, name, name_len);
    buf[name_len] = '=';
    memcpy(buf + name_len + 1, value, value_len);
    buf[name_len + 1 + value_len] = 0;
    /* Find the trailing NULL of environ and append. */
    int count = 0;
    if (environ) {
        while (environ[count]) count++;
    }
    /* Re-allocate environ to hold count + 1 (new entry) + 1 (NULL). */
    char **new_env = malloc((size_t)(count + 2) * sizeof(char *));
    if (!new_env) return -1;
    for (int i = 0; i < count; i++) new_env[i] = environ[i];
    new_env[count] = buf;
    new_env[count + 1] = NULL;
    environ = new_env;
    return 0;
}

int unsetenv(const char *name) {
    if (!environ || !name) return -1;
    size_t name_len = strlen(name);
    char **dst = environ;
    for (char **src = environ; *src; src++) {
        char *entry = *src;
        if (strlen(entry) >= name_len &&
            memcmp(entry, name, name_len) == 0 &&
            entry[name_len] == '=') {
            /* Skip — leave the slot unreplaced. */
            continue;
        }
        *dst++ = *src;
    }
    *dst = NULL;
    return 0;
}

/* abort — raise SIGABRT (signal 6), which by default terminates with
 * exit code 128 + 6 = 134. */
void abort(void) {
    _onyx_kill(_onyx_getpid(), 6 /* SIGABRT */);
    /* If the handler returns / doesn't kill us, exit anyway. */
    _onyx_exit(134);
}

/* abs — simple integer absolute value. */
int abs(int n) {
    return n < 0 ? -n : n;
}
