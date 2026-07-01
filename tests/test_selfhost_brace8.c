typedef struct { const char *name; int kind; } entry_t;
static const entry_t table[] = {
    {"auto", 1}, {"break", 2},
};
int main(void) {
    int count = 0;
    for (int i = 0; i < 2; i++) {
        count += table[i].kind;
    }
    return count != 3;
}
