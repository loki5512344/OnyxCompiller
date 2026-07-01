#include "core/compat.h"

int _start(void) {
    for (int i = 0; i < 3; i++) {
        fprintf(stderr, "%s%s", i > 0 ? "," : "_", "x");
    }
    return 0;
}
