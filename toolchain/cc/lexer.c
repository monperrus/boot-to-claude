#include "cc.h"

#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void lexer_init(lexer_t *lx, const char *src, const char *filename) {
    lx->src = src;
    lx->pos = 0;
    lx->len = strlen(src);
    lx->filename = filename;
    lx->line = 1;
    lx->bol = 1;
    lx->has_pushback = 0;
}

void lexer_push_back(lexer_t *lx, token_t t) {
    lx->pushback = t;
    lx->has_pushback = 1;
}

static int peekc(lexer_t *lx) { return lx->pos < lx->len ? (unsigned char)lx->src[lx->pos] : -1; }
static int peekc2(lexer_t *lx) { return lx->pos + 1 < lx->len ? (unsigned char)lx->src[lx->pos + 1] : -1; }
static int advc(lexer_t *lx) {
    int c = peekc(lx);
    lx->pos++;
    if (c == '\n') { lx->line++; }
    return c;
}

static token_t mk(lexer_t *lx, tok_kind_t k) {
    token_t t;
    memset(&t, 0, sizeof(t));
    t.kind = k;
    t.line = lx->line;
    t.file = lx->filename;
    t.bol = lx->bol;
    return t;
}

static int read_escape(lexer_t *lx) {
    int c = advc(lx);
    switch (c) {
        case 'n': return '\n';
        case 't': return '\t';
        case 'r': return '\r';
        case '0': return '\0';
        case '\\': return '\\';
        case '\'': return '\'';
        case '"': return '"';
        default: return c;
    }
}

token_t lexer_next(lexer_t *lx) {
    if (lx->has_pushback) { lx->has_pushback = 0; return lx->pushback; }
    for (;;) {
        int c = peekc(lx);
        if (c == ' ' || c == '\t' || c == '\r') { lx->pos++; continue; }
        if (c == '\n') { advc(lx); lx->bol = 1; continue; }
        if (c == '/' && peekc2(lx) == '/') {
            while (peekc(lx) != -1 && peekc(lx) != '\n') lx->pos++;
            continue;
        }
        if (c == '/' && peekc2(lx) == '*') {
            lx->pos += 2;
            while (peekc(lx) != -1 && !(peekc(lx) == '*' && peekc2(lx) == '/')) advc(lx);
            if (peekc(lx) != -1) lx->pos += 2;
            continue;
        }
        break;
    }

    int was_bol = lx->bol;
    lx->bol = 0;
    int c = peekc(lx);
    if (c == -1) { token_t t = mk(lx, TK_EOF); t.bol = was_bol; return t; }

    if (c == '#' && was_bol) { lx->pos++; token_t t = mk(lx, TK_HASH); t.bol = 1; return t; }

    if (isalpha(c) || c == '_') {
        size_t start = lx->pos;
        while (isalnum(peekc(lx)) || peekc(lx) == '_') lx->pos++;
        token_t t = mk(lx, TK_IDENT);
        t.bol = was_bol;
        size_t n = lx->pos - start;
        if (n >= sizeof(t.text)) n = sizeof(t.text) - 1;
        memcpy(t.text, lx->src + start, n);
        t.text[n] = '\0';
        return t;
    }

    if (isdigit(c)) {
        size_t start = lx->pos;
        while (isalnum(peekc(lx))) lx->pos++;
        token_t t = mk(lx, TK_NUM);
        t.bol = was_bol;
        int len = (int)(lx->pos - start);
        const char *p = lx->src + start;
        if (len > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            t.num = strtoll(p + 2, NULL, 16);
        } else {
            t.num = strtoll(p, NULL, 10);
        }
        return t;
    }

    if (c == '"') {
        lx->pos++;
        char buf[256];
        int n = 0;
        while (peekc(lx) != -1 && peekc(lx) != '"') {
            int ch = advc(lx);
            if (ch == '\\') ch = read_escape(lx);
            if (n < (int)sizeof(buf) - 1) buf[n++] = (char)ch;
        }
        if (peekc(lx) == '"') lx->pos++;
        buf[n] = '\0';
        token_t t = mk(lx, TK_STR);
        t.bol = was_bol;
        memcpy(t.text, buf, (size_t)n + 1);
        return t;
    }

    if (c == '\'') {
        lx->pos++;
        int ch = advc(lx);
        if (ch == '\\') ch = read_escape(lx);
        if (peekc(lx) == '\'') lx->pos++;
        token_t t = mk(lx, TK_CHAR);
        t.bol = was_bol;
        t.num = ch;
        return t;
    }

#define TWO(c1, c2, kind) if (c == (c1) && peekc2(lx) == (c2)) { lx->pos += 2; token_t t = mk(lx, kind); t.bol = was_bol; return t; }
    TWO('=', '=', TK_EQ) TWO('!', '=', TK_NEQ) TWO('<', '=', TK_LE) TWO('>', '=', TK_GE)
    TWO('&', '&', TK_AND) TWO('|', '|', TK_OR) TWO('+', '+', TK_INC) TWO('-', '-', TK_DEC)
    TWO('-', '>', TK_ARROW) TWO('+', '=', TK_PLUSEQ) TWO('-', '=', TK_MINUSEQ)
    TWO('*', '=', TK_STAREQ) TWO('/', '=', TK_SLASHEQ) TWO('%', '=', TK_PERCENTEQ)
    TWO('&', '=', TK_ANDEQ) TWO('|', '=', TK_OREQ) TWO('^', '=', TK_XOREQ)
#undef TWO
    if (c == '<' && peekc2(lx) == '<') {
        if (lx->pos + 2 < lx->len && lx->src[lx->pos + 2] == '=') { lx->pos += 3; token_t t = mk(lx, TK_SHLEQ); t.bol = was_bol; return t; }
        lx->pos += 2; token_t t = mk(lx, TK_SHL); t.bol = was_bol; return t;
    }
    if (c == '>' && peekc2(lx) == '>') {
        if (lx->pos + 2 < lx->len && lx->src[lx->pos + 2] == '=') { lx->pos += 3; token_t t = mk(lx, TK_SHREQ); t.bol = was_bol; return t; }
        lx->pos += 2; token_t t = mk(lx, TK_SHR); t.bol = was_bol; return t;
    }

    lx->pos++;
    tok_kind_t k;
    switch (c) {
        case '(': k = TK_LPAREN; break;
        case ')': k = TK_RPAREN; break;
        case '{': k = TK_LBRACE; break;
        case '}': k = TK_RBRACE; break;
        case '[': k = TK_LBRACKET; break;
        case ']': k = TK_RBRACKET; break;
        case ';': k = TK_SEMI; break;
        case ',': k = TK_COMMA; break;
        case ':': k = TK_COLON; break;
        case '?': k = TK_QUESTION; break;
        case '.': k = TK_DOT; break;
        case '+': k = TK_PLUS; break;
        case '-': k = TK_MINUS; break;
        case '*': k = TK_STAR; break;
        case '/': k = TK_SLASH; break;
        case '%': k = TK_PERCENT; break;
        case '=': k = TK_ASSIGN; break;
        case '<': k = TK_LT; break;
        case '>': k = TK_GT; break;
        case '!': k = TK_NOT; break;
        case '&': k = TK_AMP; break;
        case '|': k = TK_PIPE; break;
        case '^': k = TK_CARET; break;
        case '~': k = TK_TILDE; break;
        default:
            fprintf(stderr, "cc: %s:%d: unexpected character '%c'\n", lx->filename, lx->line, c);
            exit(1);
    }
    token_t t = mk(lx, k);
    t.bol = was_bol;
    return t;
}

token_t lexer_next_angle_include(lexer_t *lx) {
    while (peekc(lx) == ' ' || peekc(lx) == '\t') lx->pos++;
    token_t t = mk(lx, TK_STR);
    if (peekc(lx) != '<') { fprintf(stderr, "cc: %s:%d: expected <file> after #include\n", lx->filename, lx->line); exit(1); }
    lx->pos++;
    int n = 0;
    while (peekc(lx) != -1 && peekc(lx) != '>' && n < (int)sizeof(t.text) - 1) {
        t.text[n++] = (char)advc(lx);
    }
    t.text[n] = '\0';
    if (peekc(lx) == '>') lx->pos++;
    return t;
}
