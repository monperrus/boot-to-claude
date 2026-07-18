typedef struct {
    int x;
    int y;
} point_t;

typedef struct {
    char used;
    int size;
} __attribute__((packed)) packed_t;

int dist_manhattan(point_t *a, point_t *b) {
    int dx = a->x - b->x;
    int dy = a->y - b->y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx + dy;
}

static point_t g_origin;

int main(void) {
    point_t p1;
    p1.x = 3;
    p1.y = 4;

    point_t p2;
    p2.x = 0;
    p2.y = 0;

    g_origin.x = 1;
    g_origin.y = 1;

    int d1 = dist_manhattan(&p1, &p2); /* 3+4=7 */
    int d2 = dist_manhattan(&p1, &g_origin); /* 2+3=5 */

    packed_t pk;
    pk.used = 1;
    pk.size = 100;
    int psize = sizeof(packed_t); /* packed: 1 + 4 = 5 bytes, no padding */

    return d1 + d2 + psize - pk.size + 100; /* 7+5+5-100+100 = 17 */
}
