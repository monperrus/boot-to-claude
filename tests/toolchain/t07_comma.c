int main(void) {
    int sum = 0;
    int i, j;
    for (i = 0, j = 10; i < 5; i++, j--) {
        sum += i + j;
    }
    /* pairs (0,10)(1,9)(2,8)(3,7)(4,6), each sums to 10 -> 50 */

    int x = (1, 2, 3); /* comma operator: discards 1 and 2, x = 3 */

    return sum + x; /* 50 + 3 = 53 */
}
