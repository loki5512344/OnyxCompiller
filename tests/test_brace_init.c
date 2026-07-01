/* Full brace init test + do/while */
struct kw_entry { char *name; int kind; };
struct kw_entry kws[] = {{"auto", 1}, {"break", 2}, {0, 0}};
char *predefines[] = {"hello=1", "world=2", 0};
struct option { char *name; int has_arg; int flag; int val; };
struct option long_opts[] = {{"include", 1, 0, 'I'}, {0, 0, 0, 0}};
int nums[] = {10, 20, 30, 40, 50};
int fixed[10] = {1, 2, 3};
int scalar = {42};
struct point { int x; int y; };
struct labeled { char *label; struct point pt; int id; };
struct labeled pts[] = {{"first", {1, 2}, 10}};

int local_test(void) {
    int arr[] = {1, 2, 3, 4, 5};
    int sum = 0;
    int i = 0;
    while (i < 5) {
        sum = sum + arr[i];
        i = i + 1;
    }
    return sum;
}

int local_struct_test(void) {
    struct point p = {100, 200};
    return p.x + p.y;
}

int do_while_test(void) {
    int i = 0;
    int sum = 0;
    do {
        sum = sum + i;
        i = i + 1;
    } while (i < 5);
    return sum;
}

int _start(void) {
    if (kws[0].kind != 1) return 1;
    if (predefines[0][0] != 'h') return 2;
    if (long_opts[0].val != 'I') return 3;
    if (nums[0] != 10) return 4;
    if (nums[4] != 50) return 5;
    if (fixed[0] != 1) return 6;
    if (fixed[3] != 0) return 7;
    if (scalar != 42) return 8;
    if (pts[0].pt.x != 1) return 9;
    if (pts[0].pt.y != 2) return 10;
    if (local_test() != 15) return 11;
    if (local_struct_test() != 300) return 12;
    if (do_while_test() != 10) return 13;
    return 0;
}
