/*
 * libonyxc — minimal C library for OnyxOS userspace.
 *
 * Design goals:
 *   - Tiny (target: < 30 KiB compiled .onx)
 *   - musl-inspired API surface, but smaller
 *   - Direct syscall wrappers, no fancy buffering
 *   - Suitable for self-hosting onyxcc
 *
 * Headers in include/ mirror standard locations.
 */
#ifndef LIBONYXC_H
#define LIBONYXC_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>

/* ---- syscalls (raw) ---- */
long _onyx_write(int fd, const void *buf, size_t n);
long _onyx_read(int fd, void *buf, size_t n);
void _onyx_exit(int code) __attribute__((noreturn));
long _onyx_yield(void);
long _onyx_getpid(void);
void *_onyx_sbrk(long inc);
long _onyx_open(const char *path, int flags, int mode);
long _onyx_close(int fd);
long _onyx_lseek(int fd, long off, int whence);
long _onyx_exec(const char *path);
long _onyx_spawn(const char *path, int ring_hint);
long _onyx_wait(int *status);
long _onyx_readdir(const char *dir, char *name_out, size_t len);
long _onyx_getring(void);
long _onyx_dropring(int target);

/* ---- stdio ---- */
int printf(const char *fmt, ...);
int fprintf(void *fp, const char *fmt, ...);  /* fp ignored in MVP */
int puts(const char *s);
int putchar(int c);
int fflush(void *fp);
size_t strlen(const char *s);

/* ---- stdlib ---- */
void *malloc(size_t n);
void  free(void *p);
void *calloc(size_t n, size_t sz);
void *realloc(void *p, size_t n);
void  exit(int code) __attribute__((noreturn));
int   atoi(const char *s);
long  strtol(const char *s, char **endp, int base);

/* ---- string ---- */
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, size_t n);
char *strcat(char *d, const char *s);
char *strchr(const char *s, int c);
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);

/* ---- ctype ---- */
int isalpha(int c);
int isdigit(int c);
int isspace(int c);
int isupper(int c);
int islower(int c);
int isxdigit(int c);
int isalnum(int c);
int tolower(int c);
int toupper(int c);

#endif /* LIBONYXC_H */
