/* Whole-struct assignment (`a = b;`, or a non-brace initializer like
 * `T save = *p;`) needs a real byte-for-byte copy, not a single 4-byte
 * mov - the pattern the kernel's script-language parser relies on for
 * backtracking (`parser_t save = *p; ... *p = save;`). */
typedef struct {
    int a;
    int b;
    char tag[8];
} rec_t;

int main(void) {
    rec_t x;
    x.a = 1;
    x.b = 2;
    x.tag[0] = 'h';
    x.tag[1] = 'i';
    x.tag[2] = 0;

    rec_t save = x; /* non-brace initializer, struct copy */

    x.a = 100;
    x.b = 200;
    x.tag[0] = 'z';

    x = save; /* plain struct assignment, restore */

    return x.a + x.b + x.tag[0] + x.tag[1];
    /* 1 + 2 + 'h'(104) + 'i'(105) = 212 */
}
