/*
 * start.c — program entry point for OnyxOS userspace (v0.4).
 *
 * _start is the actual entry symbol expected by the OnyxKernel loader.
 * The kernel's onx/argv.rs now passes a full ELF-style stack frame:
 *
 *   sp ──►  argc            (8 bytes)
 *           argv[0]         (8 bytes)
 *           ...
 *           argv[argc-1]    (8 bytes)
 *           NULL            (8 bytes)
 *           envp[0..envc]   (8 bytes each)
 *           NULL            (8 bytes)
 *           auxv[]          (16 bytes each, AT_NULL-terminated)
 *           <argv strings>
 *           <envp strings>
 *           <16 bytes of AT_RANDOM entropy>
 *
 * On entry:
 *   a0 = argc
 *   a1 = &argv[0]   (i.e. sp + 8)
 *
 * We support two main() signatures so that both classic Unix programs
 * (int main(int argc, char **argv)) and the simpler OnyxOS programs
 * (int main(void)) work. If the program's main() takes no args we just
 * ignore argc/argv.
 */
#include "onyxc.h"

extern int main(int argc, char **argv);

/* _argv and _environ are exported so user code can access them via the
 * standard `extern char **environ;` declaration if it wants to. */
char **__onyx_argv = 0;
char **environ     = 0;

void _start(void) {
    long argc;
    char **argv;
    __asm__ volatile (
        "mv %0, a0\n"
        "mv %1, a1\n"
        : "=r"(argc), "=r"(argv)
        :
        : "memory"
    );

    /* If argc is 0 (kernel didn't pass argv — old v0.3 contract), synthesize
     * a minimal argv[0] = "onyx-program" so libc consumers don't crash. */
    static char progname[] = "onyx-program";
    static char *default_argv[2] = { 0, 0 };
    if (argc == 0 || argv == 0) {
        default_argv[0] = progname;
        argc = 1;
        argv = default_argv;
    }

    __onyx_argv = argv;
    /* environ lives just past argv's NULL terminator. argv layout is
     *   [argv[0] .. argv[argc-1] NULL envp[0] .. envp[envc-1] NULL auxv...]
     * so environ = &argv[argc + 1]. */
    environ = &argv[argc + 1];

    int ret = main((int)argc, argv);
    exit(ret);
}
