typedef struct { const char *name; int kind; } entry_t;
static const entry_t table[] = {
    {"auto", 1}, {"break", 2},
};
int main(void) {
    int x = table[0].kind;
    return x;
}
