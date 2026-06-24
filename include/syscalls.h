/*
 * syscalls.h — OnyxOS syscall ABI.
 * Synced with OnyxKernel/kernel/src/syscall/abi.rs.
 *
 * Calling convention (RISC-V LP64):
 *   a7 = syscall number
 *   a0..a5 = arguments
 *   a0 = return value (negative = -errno)
 *
 * Trap: ecall instruction (CAUSE_U_ECALL handled by kernel/src/srv/trap.rs).
 *
 * Ring ACL (kernel/src/syscall/handler.rs):
 *   Ring 2 (user) may call: write, read, exit, yield, getpid, sbrk,
 *     open, close, lseek, stat, exec, readdir, getring, dropring,
 *     sigmask, write_fd, chan_connect, chan_send, chan_recv, chan_close.
 *   Ring 1 (root) additionally: spawn, wait, kill, create, mkdir,
 *     chan_create, snapshot_*.
 */
#ifndef ONYX_SYSCALLS_H
#define ONYX_SYSCALLS_H

#include <stdint.h>

#define SYS_write             1
#define SYS_read              2
#define SYS_exit              3
#define SYS_yield             4
#define SYS_getpid            5
#define SYS_brk               6
#define SYS_mmap              7
#define SYS_open              8
#define SYS_close             9
#define SYS_lseek            10
#define SYS_stat             11
#define SYS_exec             12
#define SYS_sbrk             13
#define SYS_spawn            14
#define SYS_wait             15
#define SYS_readdir          16
#define SYS_getring          17
#define SYS_dropring         18
#define SYS_snapshot_create  19
#define SYS_snapshot_rollback 20
#define SYS_snapshot_list    21
#define SYS_kill             22
#define SYS_sigmask          23
#define SYS_write_fd         24
#define SYS_create           25
#define SYS_mkdir            26
#define SYS_chan_create      27
#define SYS_chan_connect     28
#define SYS_chan_send        29
#define SYS_chan_recv        30
#define SYS_chan_close       31

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define RING_KERNEL 0
#define RING_ROOT   1
#define RING_USER   2

/* O_* flags are interpreted by kernel/src/syscall/fs_sys.rs.
 * Only a small subset is honored; values match kernel constants. */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x80
#define O_APPEND 0x100

/* errnos — match onyx-core/errno.rs. */
#define EPERM    1
#define ENOENT   2
#define EIO      5
#define EBADF    9
#define ENOMEM  12
#define EACCES  13
#define EBUSY   16
#define EEXIST  17
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define ENOSPC  28
#define EPIPE   32
#define ERANGE  34

#ifndef __ASSEMBLER__

static inline long _onyx_syscall0(long n) {
    register long a0 __asm__("a0");
    register long a7 __asm__("a7") = n;
    __asm__ volatile ("ecall" : "=r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline long _onyx_syscall1(long n, long a) {
    register long a0 __asm__("a0") = a;
    register long a7 __asm__("a7") = n;
    __asm__ volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline long _onyx_syscall2(long n, long a, long b) {
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a7 __asm__("a7") = n;
    __asm__ volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return a0;
}

static inline long _onyx_syscall3(long n, long a, long b, long c) {
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a7 __asm__("a7") = n;
    __asm__ volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

#endif /* __ASSEMBLER__ */
#endif /* ONYX_SYSCALLS_H */
