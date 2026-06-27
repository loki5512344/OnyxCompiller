/*
 * libonyxc — minimal C library for OnyxOS userspace (v0.4).
 *
 * Design goals:
 *   - Tiny (target: < 30 KiB compiled .onx)
 *   - musl-inspired API surface, but smaller
 *   - Direct syscall wrappers, no fancy buffering
 *   - Suitable for self-hosting onyxcc
 *
 * Headers in include/ mirror standard locations.
 *
 * v0.4 changes:
 *   - Added wrappers for all v0.4 kernel syscalls (50–77).
 *   - start.c now reads argc/argv/envp from the kernel-supplied stack frame.
 *   - getenv/setenv/unsetenv/abort/abs added to stdlib.
 *   - Fixed sbrk error check in stdlib.c (was comparing against -1 as a
 *     pointer, which is now correct thanks to _onyx_sbrk's normalization).
 *   - Added stat/fstat struct definitions and prototypes.
 */
#ifndef LIBONYXC_H
#define LIBONYXC_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>

/* ── Standard file descriptors ────────────────────────────────────────── */
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* ── Linux-compatible struct stat (matches kernel UserStat) ───────────── */
struct stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    int64_t  st_atime;
    int64_t  st_atime_nsec;
    int64_t  st_mtime;
    int64_t  st_mtime_nsec;
    int64_t  st_ctime;
    int64_t  st_ctime_nsec;
    int64_t  __unused[3];
};

/* ── Standard struct timeval / timespec / timespec ────────────────────── */
struct timeval {
    long tv_sec;
    long tv_usec;
};
struct timespec {
    long tv_sec;
    long tv_nsec;
};

/* ── sigaction (minimal — handler pointer + mask + flags) ─────────────── */
typedef void (*sighandler_t)(int);
struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, void *, void *);
    };
    unsigned long sa_mask;
    unsigned long sa_flags;
    void (*sa_restorer)(void);
};

/* ── Raw syscall wrappers (libonyxc/src/syscalls.c) ───────────────────── */
long _onyx_write(int fd, const void *buf, size_t n);
long _onyx_read(int fd, void *buf, size_t n);
void _onyx_exit(int code) __attribute__((noreturn));
long _onyx_yield(void);
long _onyx_getpid(void);
long _onyx_getppid(void);
void *_onyx_sbrk(long inc);
long _onyx_brk(long addr);
long _onyx_open(const char *path, int flags, int mode);
long _onyx_close(int fd);
long _onyx_lseek(int fd, long off, int whence);
long _onyx_stat(const char *path, void *st);
long _onyx_fstat(int fd, void *st);
long _onyx_exec(const char *path, char *const *argv);
long _onyx_execve(const char *path, char *const *argv, char *const *envp);
long _onyx_spawn(const char *path, char *const *argv, int ring_hint);
long _onyx_wait(int *status);
long _onyx_waitpid(int pid, int *status, int options);
long _onyx_fork(void);
long _onyx_readdir(const char *dir, char *name_out, size_t len);
long _onyx_getdents64(int fd, void *buf, size_t len);
long _onyx_getring(void);
long _onyx_dropring(int target);
long _onyx_create(const char *path, int mode, long reserved);
long _onyx_mkdir(const char *path);
long _onyx_unlink(const char *path);
long _onyx_rename(const char *oldp, const char *newp);
long _onyx_truncate(const char *path);
long _onyx_truncate2(const char *path, long length);
long _onyx_ftruncate(int fd, long length);
long _onyx_chmod(const char *path, int mode);
long _onyx_fchmod(int fd, int mode);
long _onyx_access(const char *path, int mode);
long _onyx_chdir(const char *path);
long _onyx_getcwd(char *buf, size_t len);
long _onyx_dup(int oldfd);
long _onyx_pipe(int *pipefd);
long _onyx_fcntl(int fd, int cmd, long arg);
long _onyx_ioctl(int fd, long request, long arg);
long _onyx_isatty(int fd);
long _onyx_fsync(int fd);
long _onyx_mmap(long addr, long len, long prot, long flags, long fd, long off);
long _onyx_munmap(long addr, long len);
long _onyx_mprotect(long addr, long len, long prot);
long _onyx_getuid(void);
long _onyx_getgid(void);
long _onyx_setuid(int uid);
long _onyx_setgid(int gid);
long _onyx_setpgid(int pid, int pgid);
long _onyx_getpgid(int pid);
long _onyx_setsid(void);
long _onyx_gettimeofday(void *tv);
long _onyx_clock_gettime(long clk_id, void *ts);
long _onyx_clock_getres(long clk_id, void *res);
long _onyx_nanosleep(const void *req, void *rem);
long _onyx_utimens(const char *path, const void *times);
long _onyx_kill(int pid, int sig);
long _onyx_sigmask(int how, int sig);
long _onyx_sigaction(int sig, const void *act, void *oldact);
long _onyx_sigprocmask(int how, const void *set, void *oldset);
long _onyx_sigreturn(void);
long _onyx_uname(void *buf);
long _onyx_getentropy(void *buf, size_t len);
long _onyx_readlink(const char *path, char *buf, size_t bufsiz);
long _onyx_symlink(const char *target, const char *linkpath);
long _onyx_chan_create(void);
long _onyx_chan_create_named(const char *n);
long _onyx_chan_open(const char *name);
long _onyx_chan_connect(int chan_id);
long _onyx_chan_send(int chan_id, const void *buf, size_t len);
long _onyx_chan_recv(int chan_id, void *buf, size_t len);
long _onyx_chan_close(int chan_id);
long _onyx_snapshot_create(const char *name);
long _onyx_snapshot_rollback(int id);
long _onyx_snapshot_list(void *buf, size_t len);
long _onyx_write_fd(int fd, const void *buf, size_t n);

/* ── stdio ────────────────────────────────────────────────────────────── */
int printf(const char *fmt, ...);
int fprintf(void *fp, const char *fmt, ...);  /* fp ignored in MVP */
int puts(const char *s);
int putchar(int c);
int fflush(void *fp);
size_t strlen(const char *s);

/* ── stdlib ───────────────────────────────────────────────────────────── */
void *malloc(size_t n);
void  free(void *p);
void *calloc(size_t n, size_t sz);
void *realloc(void *p, size_t n);
void  exit(int code) __attribute__((noreturn));
void  abort(void) __attribute__((noreturn));
int   abs(int n);
int   atoi(const char *s);
long  strtol(const char *s, char **endp, int base);
char *getenv(const char *name);
int   setenv(const char *name, const char *value, int overwrite);
int   unsetenv(const char *name);

/* ── string ───────────────────────────────────────────────────────────── */
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, size_t n);
char *strcat(char *d, const char *s);
char *strchr(const char *s, int c);
char *strdup(const char *s);
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
void *memmove(void *d, const void *s, size_t n);
int   memcmp(const void *a, const void *b, size_t n);

/* ── ctype ────────────────────────────────────────────────────────────── */
int isalpha(int c);
int isdigit(int c);
int isspace(int c);
int isupper(int c);
int islower(int c);
int isxdigit(int c);
int isalnum(int c);
int tolower(int c);
int toupper(int c);

/* ── unistd-flavoured helpers (thin wrappers over syscalls) ───────────── */
static inline int open(const char *path, int flags, int mode) {
    return (int)_onyx_open(path, flags, mode);
}
static inline int close(int fd) {
    return (int)_onyx_close(fd);
}
static inline long read(int fd, void *buf, size_t n) {
    return _onyx_read(fd, buf, n);
}
static inline long write(int fd, const void *buf, size_t n) {
    return _onyx_write(fd, buf, n);
}
static inline long lseek(int fd, long off, int whence) {
    return _onyx_lseek(fd, off, whence);
}
static inline int stat(const char *path, struct stat *st) {
    return (int)_onyx_stat(path, st);
}
static inline int fstat(int fd, struct stat *st) {
    return (int)_onyx_fstat(fd, st);
}
static inline int unlink(const char *path) {
    return (int)_onyx_unlink(path);
}
static inline int rmdir(const char *path) {
    /* OnyxFS doesn't distinguish; unlink works for directories too. */
    return (int)_onyx_unlink(path);
}
static inline int chdir(const char *path) {
    return (int)_onyx_chdir(path);
}
static inline char *getcwd(char *buf, size_t len) {
    long r = _onyx_getcwd(buf, len);
    return r < 0 ? NULL : buf;
}
static inline int isatty(int fd) {
    return (int)_onyx_isatty(fd);
}
static inline int fork(void) {
    return (int)_onyx_fork();
}
static inline int execv(const char *path, char *const *argv) {
    return (int)_onyx_exec(path, argv);
}
static inline int execve(const char *path, char *const *argv, char *const *envp) {
    return (int)_onyx_execve(path, argv, envp);
}
static inline int getpid(void) {
    return (int)_onyx_getpid();
}
static inline int getppid(void) {
    return (int)_onyx_getppid();
}
static inline int getuid(void) {
    return (int)_onyx_getuid();
}
static inline int getgid(void) {
    return (int)_onyx_getgid();
}
static inline int dup(int oldfd) {
    return (int)_onyx_dup(oldfd);
}
static inline int pipe(int *pipefd) {
    return (int)_onyx_pipe(pipefd);
}
static inline int fcntl(int fd, int cmd, long arg) {
    return (int)_onyx_fcntl(fd, cmd, arg);
}
static inline int ioctl(int fd, long req, long arg) {
    return (int)_onyx_ioctl(fd, req, arg);
}
static inline void *sbrk(long inc) {
    return _onyx_sbrk(inc);
}

/* ── Signals ──────────────────────────────────────────────────────────── */
static inline int kill(int pid, int sig) {
    return (int)_onyx_kill(pid, sig);
}
static inline int raise(int sig) {
    return (int)_onyx_kill((int)_onyx_getpid(), sig);
}
static inline int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact) {
    return (int)_onyx_sigaction(sig, act, oldact);
}
static inline int sigprocmask(int how, const void *set, void *oldset) {
    return (int)_onyx_sigprocmask(how, set, oldset);
}

#endif /* LIBONYXC_H */
