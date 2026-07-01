/* Exact compat.h content */
typedef struct { int dummy; } FILE;
extern FILE *stderr;
extern FILE *stdout;

int fprintf(FILE *f, const char *fmt, ...);

static void usage(void) {
    fprintf(stderr, "hello\n");
}

int _start(void) {
    usage();
    return 0;
}
