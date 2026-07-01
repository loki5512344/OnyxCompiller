typedef struct { int dummy; } FILE;
extern FILE *stderr;
int fprintf(FILE *f, const char *fmt, ...);

int _start(void) {
    fprintf(stderr, "hello\n");
    return 0;
}
