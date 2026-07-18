/* Same as t11, but for a function-local `static const T ops[] = {...}`
 * (the exact pattern the kernel's scripting-language parser files use
 * for their operator tables). */
static int count_matches(int x) {
    static const int table[] = { 3, 7, 11, 15 };
    int n = (int)(sizeof(table) / sizeof(table[0]));
    for (int i = 0; i < n; i++) {
        if (table[i] == x) return 1;
    }
    return 0;
}

int main(void) {
    int hits = 0;
    for (int x = 0; x < 20; x++) {
        if (count_matches(x)) hits++;
    }
    return hits; /* 3,7,11,15 -> 4 */
}
