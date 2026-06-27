/*
 * string.c — minimal string functions.
 */
#include "onyxc.h"

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

char *strcpy(char *d, const char *s) {
    char *r = d;
    while ((*d++ = *s++)) ;
    return r;
}

char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

char *strcat(char *d, const char *s) {
    char *r = d;
    while (*d) d++;
    while ((*d++ = *s++)) ;
    return r;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == 0) ? (char *)s : NULL;
}

void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    for (size_t i = 0; i < n; i++) dp[i] = sp[i];
    return d;
}

void *memset(void *d, int c, size_t n) {
    unsigned char *dp = (unsigned char *)d;
    for (size_t i = 0; i < n; i++) dp[i] = (unsigned char)c;
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *ap = (const unsigned char *)a;
    const unsigned char *bp = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        if (ap[i] != bp[i]) return (int)ap[i] - (int)bp[i];
    }
    return 0;
}

void *memmove(void *d, const void *s, size_t n) {
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    if (dp == sp || n == 0) return d;
    /* Copy forward if dst < src; backward if dst > src (overlap). */
    if (dp < sp) {
        for (size_t i = 0; i < n; i++) dp[i] = sp[i];
    } else {
        for (size_t i = n; i > 0; i--) dp[i - 1] = sp[i - 1];
    }
    return d;
}

char *strdup(const char *s) {
    size_t len = strlen(s);
    char *p = malloc(len + 1);
    if (!p) return NULL;
    memcpy(p, s, len + 1);
    return p;
}
