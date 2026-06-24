/*
 * start.c — program entry point for OnyxOS userspace.
 *
 * _start is the actual entry symbol expected by the OnyxKernel loader.
 * It sets up just enough state to call main(), then exits via SYS_exit.
 *
 * The kernel's onx.rs::load() sets:
 *   - sp = USER_TOP - 16
 *   - a0 = 0 (no argc in MVP — kernel does not pass args)
 *   - pc = entry
 *
 * We don't have argc/argv in MVP; main() takes (void).
 */
#include "onyxc.h"

extern int main(void);

void _start(void) {
    int ret = main();
    exit(ret);
}
