int classify(int x) {
    int r = 0;
    switch (x) {
        case 1:
        case 2:
            r = 10;
            break;
        case 3:
            r = 20;
            /* fallthrough */
        case 4:
            r = r + 1;
            break;
        default:
            r = 99;
            break;
    }
    return r;
}

int loop_with_switch(void) {
    int total = 0;
    for (int i = 0; i < 5; i++) {
        switch (i) {
            case 2:
                continue; /* continues the enclosing for loop, not the switch */
            case 4:
                break; /* breaks only the switch - falls through to total += i below */
        }
        total += i;
    }
    return total;
}

int main(void) {
    int a = classify(1); /* case 1 falls into case 2's body -> 10 */
    int b = classify(3); /* r=20, falls through into case 4 -> 21 */
    int c = classify(4); /* r=0+1 -> 1 */
    int d = classify(100); /* no match -> default -> 99 */
    int e = loop_with_switch(); /* 0+1+3+4 (i=2 skipped via continue) -> 8 */
    return a + b + c + d + e; /* 10+21+1+99+8 = 139 */
}
