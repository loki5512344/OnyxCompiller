/*
 * syscalls.c — raw syscall wrappers.
 */
#include "onyxc.h"
#include "../../include/syscalls.h"

long _onyx_write(int fd, const void *buf, size_t n) {
    return _onyx_syscall3(SYS_write, fd, (long)buf, (long)n);
}

long _onyx_read(int fd, void *buf, size_t n) {
    return _onyx_syscall3(SYS_read, fd, (long)buf, (long)n);
}

void _onyx_exit(int code) {
    _onyx_syscall1(SYS_exit, code);
    /* Should not return, but just in case. */
    for (;;) {
        _onyx_syscall0(SYS_yield);
    }
}

long _onyx_yield(void) { return _onyx_syscall0(SYS_yield); }
long _onyx_getpid(void) { return _onyx_syscall0(SYS_getpid); }

void *_onyx_sbrk(long inc) {
    return (void *)_onyx_syscall1(SYS_sbrk, inc);
}

long _onyx_open(const char *path, int flags, int mode) {
    return _onyx_syscall3(SYS_open, (long)path, flags, mode);
}

long _onyx_close(int fd) {
    return _onyx_syscall1(SYS_close, fd);
}

long _onyx_lseek(int fd, long off, int whence) {
    return _onyx_syscall3(SYS_lseek, fd, off, whence);
}

long _onyx_exec(const char *path) {
    return _onyx_syscall1(SYS_exec, (long)path);
}

long _onyx_spawn(const char *path, int ring_hint) {
    return _onyx_syscall2(SYS_spawn, (long)path, ring_hint);
}

long _onyx_wait(int *status) {
    return _onyx_syscall1(SYS_wait, (long)status);
}

long _onyx_readdir(const char *dir, char *name_out, size_t len) {
    return _onyx_syscall3(SYS_readdir, (long)dir, (long)name_out, (long)len);
}

long _onyx_getring(void) { return _onyx_syscall0(SYS_getring); }
long _onyx_dropring(int target) { return _onyx_syscall1(SYS_dropring, target); }
