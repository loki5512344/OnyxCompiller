#include "core/compat.h"

int _start(void) {
    fprintf(stderr, "%s", 1 > 0 ? "," : "");
    return 0;
}
