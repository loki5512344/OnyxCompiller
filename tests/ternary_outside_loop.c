#include "core/compat.h"

int _start(void) {
    int i = 1;
    const char *sep = i > 0 ? "," : "";
    fprintf(stderr, "%s%s", sep, "x");
    return 0;
}
