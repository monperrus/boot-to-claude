#include <ctype.h>

int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }
