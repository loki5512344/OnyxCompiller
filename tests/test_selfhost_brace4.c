#include "core/cc.h"
typedef struct { const char *name; int kind; } entry_t;
static const entry_t table[] = {
    {"auto",   1},
    {"break",  2},
    {"case",   3},
    {"char",   4},
    {"const",  5},
    {"do",     6},
    {"double", 7},
    {"else",   8},
    {"int",    9},
    {"long",   10},
    {"return", 11},
    {"short",  12},
    {"static", 13},
    {"struct", 14},
    {"void",   15},
    {"while",  16},
};
int main(void) { return 0; }
