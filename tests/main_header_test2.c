#include "core/compat.h"
#include "core/cc.h"
#include "back/pp.h"
#include "front/lexer.h"
#include "core/types.h"
#include "front/ast.h"
#include "front/parse.h"
#include "back/gen.h"
#include "back/emit.h"

static struct option long_opts[] = {
    {"include",    required_argument, 0, 'I'},
    {"define",     required_argument, 0, 'D'},
    {"output",     required_argument, 0, 'o'},
    {"entry",      required_argument, 0, 'e'},
    {"ring1",      no_argument,       0, 'R'},
    {"verbose",    no_argument,       0, 'v'},
    {"dump-tokens",no_argument,       0, 'T'},
    {"help",       no_argument,       0, 'h'},
    {0, 0, 0, 0},
};

static void usage(void) {
    fprintf(stderr, "hello\n");
}

int _start(void) {
    usage();
    return 0;
}
