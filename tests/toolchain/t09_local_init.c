typedef struct { int a; int b; } pair_t;
typedef struct { int tag; pair_t p; } tagged_t;

int main(void) {
    int arr[3] = { 10, 20, 30 };
    pair_t pr = { 1, 2 };
    tagged_t t = { 5, { 6, 7 } };
    return arr[0] + arr[1] + arr[2] + pr.a + pr.b + t.tag + t.p.a + t.p.b;
    /* 10+20+30 + 1+2 + 5+6+7 = 81 */
}
