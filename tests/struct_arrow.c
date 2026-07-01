typedef struct { int fd; } FILE;

int _start(void) {
    FILE f;
    FILE *p = &f;
    p->fd = 1;
    return p->fd;
}
