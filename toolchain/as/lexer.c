#include "as.h"

#include <ctype.h>
#include <string.h>

void lexer_init(lexer_t *lx, const char *src) {
    lx->src = src;
    lx->pos = 0;
    lx->len = strlen(src);
}

static int peekc(lexer_t *lx) {
    return lx->pos < lx->len ? (unsigned char)lx->src[lx->pos] : -1;
}
static int peekc2(lexer_t *lx) {
    return lx->pos + 1 < lx->len ? (unsigned char)lx->src[lx->pos + 1] : -1;
}

static int is_ident_start(int c) {
    return isalpha(c) || c == '_' || c == '.';
}
static int is_ident_cont(int c) {
    return isalnum(c) || c == '_';
}

static token_t make(tok_kind_t k) {
    token_t t;
    memset(&t, 0, sizeof(t));
    t.kind = k;
    return t;
}

static int64_t parse_number(const char *text, int len) {
    if (len > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        int64_t v = 0;
        for (int i = 2; i < len; i++) {
            char c = text[i];
            v *= 16;
            if (c >= '0' && c <= '9') v += c - '0';
            else if (c >= 'a' && c <= 'f') v += 10 + (c - 'a');
            else if (c >= 'A' && c <= 'F') v += 10 + (c - 'A');
        }
        return v;
    }
    if (len > 1 && (text[len - 1] == 'b' || text[len - 1] == 'B')) {
        int all01 = 1;
        for (int i = 0; i < len - 1; i++) {
            if (text[i] != '0' && text[i] != '1') { all01 = 0; break; }
        }
        if (all01) {
            int64_t v = 0;
            for (int i = 0; i < len - 1; i++) {
                v = v * 2 + (text[i] - '0');
            }
            return v;
        }
    }
    int64_t v = 0;
    for (int i = 0; i < len; i++) {
        v = v * 10 + (text[i] - '0');
    }
    return v;
}

token_t lexer_next(lexer_t *lx) {
    for (;;) {
        int c = peekc(lx);
        if (c == ' ' || c == '\t' || c == '\r') { lx->pos++; continue; }
        if (c == ';') {
            while (peekc(lx) != -1 && peekc(lx) != '\n') lx->pos++;
            continue;
        }
        break;
    }

    int c = peekc(lx);
    if (c == -1) return make(TK_EOF);
    if (c == '\n') { lx->pos++; return make(TK_NEWLINE); }

    if (is_ident_start(c)) {
        size_t start = lx->pos;
        lx->pos++;
        while (is_ident_cont(peekc(lx))) lx->pos++;
        token_t t = make(TK_IDENT);
        size_t n = lx->pos - start;
        if (n >= sizeof(t.text)) n = sizeof(t.text) - 1;
        memcpy(t.text, lx->src + start, n);
        t.text[n] = '\0';
        return t;
    }

    if (isdigit(c)) {
        size_t start = lx->pos;
        lx->pos++;
        while (isalnum(peekc(lx))) lx->pos++;
        token_t t = make(TK_NUMBER);
        int n = (int)(lx->pos - start);
        t.number = parse_number(lx->src + start, n);
        return t;
    }

    if (c == '"' || c == '\'') {
        int quote = c;
        lx->pos++;
        size_t start = lx->pos;
        while (peekc(lx) != -1 && peekc(lx) != quote) lx->pos++;
        token_t t = make(TK_STRING);
        size_t n = lx->pos - start;
        if (n >= sizeof(t.text)) n = sizeof(t.text) - 1;
        memcpy(t.text, lx->src + start, n);
        t.text[n] = '\0';
        if (peekc(lx) == quote) lx->pos++;
        return t;
    }

    if (c == '$' && peekc2(lx) == '$') { lx->pos += 2; return make(TK_DOLLARDOLLAR); }
    if (c == '$') { lx->pos++; return make(TK_DOLLAR); }
    if (c == '[') { lx->pos++; return make(TK_LBRACK); }
    if (c == ']') { lx->pos++; return make(TK_RBRACK); }
    if (c == ',') { lx->pos++; return make(TK_COMMA); }
    if (c == ':') { lx->pos++; return make(TK_COLON); }
    if (c == '+') { lx->pos++; return make(TK_PLUS); }
    if (c == '-') { lx->pos++; return make(TK_MINUS); }
    if (c == '*') { lx->pos++; return make(TK_STAR); }
    if (c == '(') { lx->pos++; return make(TK_LPAREN); }
    if (c == ')') { lx->pos++; return make(TK_RPAREN); }
    if (c == '<' && peekc2(lx) == '<') { lx->pos += 2; return make(TK_SHL); }
    if (c == '>' && peekc2(lx) == '>') { lx->pos += 2; return make(TK_SHR); }
    if (c == '/') { lx->pos++; return make(TK_SLASH); }

    /* Unknown character: skip it so we make forward progress; the
     * parser will fail loudly on the resulting malformed statement. */
    lx->pos++;
    return lexer_next(lx);
}
