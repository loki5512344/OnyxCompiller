#include "core/compat.h"

int _start(void) {
    int i = 1;
    fprintf(stderr, "%s%s", i > 0 ? "," : "", "x");
    return 0;
}
