/* Minimal brace initializer test */

struct option {
    const char *name;
    int has_arg;
    int *flag;
    int val;
};

static struct option long_opts[] = {
    {"include", 1, 0, 'I'},
    {"define",  1, 0, 'D'},
    {"output",  1, 0, 'o'},
    {0, 0, 0, 0},
};

static const char *predefines[] = {
    "__onyx__=1",
    "__onyxos__=1",
    0,
};

static int nums[] = {10, 20, 30, 40};

int _start(void) {
    int local_arr[] = {100, 200, 300};
    struct option local_opt = {"help", 0, 0, 'h'};

    int sum = 0;
    sum += long_opts[0].val;
    sum += long_opts[1].val;
    sum += nums[2];
    sum += local_arr[1];
    sum += local_opt.val;
    sum += (predefines[0][0]);
    return sum;
}
