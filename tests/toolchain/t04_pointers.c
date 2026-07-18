int arr_sum(int *a, int n) {
    int total = 0;
    for (int i = 0; i < n; i++) {
        total = total + a[i];
    }
    return total;
}

void inc_via_ptr(int *p) {
    *p = *p + 1;
}

int main(void) {
    int nums[5];
    for (int i = 0; i < 5; i++) {
        nums[i] = i * 2;
    }
    /* nums = 0,2,4,6,8 -> sum = 20 */

    int x = 10;
    int *px = &x;
    inc_via_ptr(px);
    /* x = 11 */

    char buf[4];
    buf[0] = 'a';
    buf[1] = 'b';
    buf[2] = 'c';
    buf[3] = 0;
    int charsum = buf[0] + buf[1] + buf[2]; /* 'a'+'b'+'c' = 97+98+99=294 */

    return arr_sum(nums, 5) + x - charsum + 294; /* 20 + 11 - 294 + 294 = 31 */
}
