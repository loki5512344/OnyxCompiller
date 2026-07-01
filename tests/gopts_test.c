#include "core/compat.h"
#include "core/cc.h"

int _start(void) {
    g_opts.current_file_idx = 0;
    g_opts.n_input_files++;
    return 0;
}
