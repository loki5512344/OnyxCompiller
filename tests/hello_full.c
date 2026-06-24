/*
 * hello_full.c — runnable hello world with _start + syscall stubs.
 *
 * Uses onyxcc built-in __ecallN() functions to issue syscalls without
 * inline assembly. Output is a complete .onx that runs in Ring 2.
 */

typedef unsigned long size_t;
typedef long ssize_t;

#define SYS_write 1
#define SYS_exit  3

/* onyxcc built-ins: ecall with N args, a7=first arg, returns a0. */
long __ecall1(long n);
long __ecall3(long n, long a, long b, long c);

static long write_fd(int fd, const char *s, long n) {
    return __ecall3(SYS_write, fd, (long)s, n);
}

static long write_str(int fd, const char *s) {
    long n = 0;
    while (s[n]) n = n + 1;
    return write_fd(fd, s, n);
}

static void put_udec(int fd, unsigned long v) {
    char buf[24];
    int p = 23;
    buf[p] = 0;
    p = p - 1;
    if (v == 0) {
        buf[p] = '0';
        p = p - 1;
    }
    while (v > 0) {
        buf[p] = '0' + (int)(v % 10);
        p = p - 1;
        v = v / 10;
    }
    write_fd(fd, &buf[p + 1], 23 - p);
}

static void exit_proc(int code) {
    __ecall1(SYS_exit);
    for (;;) { }
}

int main(void) {
    write_str(1, "Hello, OnyxOS!\n");
    write_str(1, "From a C program compiled by onyxcc.\n");
    write_str(1, "Number: ");
    put_udec(1, 42);
    write_str(1, "\n");
    return 0;
}

void _start(void) {
    int ret = main();
    exit_proc(ret);
}
