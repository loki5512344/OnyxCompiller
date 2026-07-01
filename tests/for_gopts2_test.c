#include "core/compat.h"
#include "core/cc.h"

int _start(void) {
    for (g_opts.n_input_files = 0;
         g_opts.n_input_files < 3;
         g_opts.n_input_files++) {
    }
    return 0;
}
