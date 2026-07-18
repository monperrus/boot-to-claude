/* '\b' (and friends) must lex to their real control-character values,
 * not fall through to the literal letter - kernel/keyboard.c's
 * lower_table[] and kernel/shell/shell.c's `if (c == '\b')` both depend
 * on this (a wrong '\b' == 'b' made every typed letter 'b' act like a
 * backspace keypress instead). */
int main(void) {
    char b = '\b';
    char t = '\t';
    char n = '\n';
    char bs = '\\';
    char q = '\'';
    return (int)b + (int)t + (int)n + (int)bs + (int)q;
    /* 8 + 9 + 10 + 92 + 39 = 158 */
}
