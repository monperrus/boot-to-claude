int main(void) {
    int sum = 0;
    int i = 0;
    while (i < 5) {
        sum = sum + i;
        i = i + 1;
    }
    /* sum = 0+1+2+3+4 = 10 */

    for (int j = 0; j < 3; j++) {
        sum = sum + 1;
    }
    /* sum = 13 */

    int k = 0;
    do {
        k++;
    } while (k < 4);
    sum = sum + k; /* sum = 17 */

    if (sum > 15) {
        sum = sum + 100;
    } else {
        sum = 0;
    }
    /* sum = 117 */

    int total = 0;
    for (int n = 0; n < 10; n++) {
        if (n == 5) continue;
        if (n == 8) break;
        total = total + 1;
    }
    /* n=0..4 (5), skip 5, n=6,7 (2), break at 8: total = 7 */

    return sum - 117 + total; /* expect 7 */
}
