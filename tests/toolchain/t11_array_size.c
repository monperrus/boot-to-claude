/* `T arr[] = { ... }` must infer its length from the initializer -
 * sizeof/array indexing bounds depend on it (kernel/keyboard.c's
 * scancode table used exactly this pattern; a wrong inferred length of
 * 0 made every bounds check `code >= TABLE_LEN` vacuously true). */
static const char table[] = { 10, 20, 30, 40, 50 };
#define TABLE_LEN (sizeof(table) / sizeof(table[0]))

int main(void) {
    int sum = 0;
    for (unsigned int i = 0; i < TABLE_LEN; i++) {
        sum += table[i];
    }
    return sum + (int)TABLE_LEN; /* (10+20+30+40+50) + 5 = 155 */
}
