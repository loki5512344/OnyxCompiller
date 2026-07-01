/* Test using exact compat.h non-linux declarations */
typedef struct { int dummy; } FILE;
extern FILE *stderr;
int fprintf(FILE *f, const char *fmt, ...);

static void usage(void) {
    fprintf(stderr, "hello\n");
}

int _start(void) {
    usage();
    return 0;
}
