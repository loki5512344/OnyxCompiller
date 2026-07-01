#include "core/compat.h"

int _start(void) {
    for (int i = 0; i < 3; i++) {
        const char *sep = i > 0 ? "," : "";
        fprintf(stderr, "%s%s", sep, "x");
    }
    return 0;
}
