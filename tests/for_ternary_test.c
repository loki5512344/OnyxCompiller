#include "core/compat.h"

int _start(void) {
    for (int i = 0; i < 3; i++) {
        int x = 1 ? 2 : 3;
    }
    return 0;
}
