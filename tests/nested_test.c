/* Nested array brace initializer test */
static int matrix[2][3] = {
    {1, 2, 3},
    {4, 5, 6}
};

int _start(void) {
    return matrix[0][0] + matrix[1][2];
}
