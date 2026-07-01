/* Test: struct with brace init */
struct point {
    int x;
    int y;
};

struct point pts[] = {
    {1, 2},
    {3, 4},
};

int _start(void) {
    return pts[0].x + pts[1].y;
}
