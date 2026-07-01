#include "core/compat.h"
#include "core/cc.h"

int _start(void) {
    for (int i = 0; i < 3; i++) {
        fprintf(stderr, "%s%s", i > 0 ? "," : "", "x");
    }
    return 0;
}
