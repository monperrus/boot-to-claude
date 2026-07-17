#include "lexer.h"

#include "../memory.h"
#include "../string.h"

static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int is_alnum(char c) {
    return is_alpha(c) || is_digit(c);
}

static void skip_ws_and_comments(lexer_t *lx) {
    for (;;) {
        char c = lx->src[lx->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            lx->pos++;
        } else if (c == '#') {
            while (lx->src[lx->pos] && lx->src[lx->pos] != '\n') {
                lx->pos++;
            }
        } else {
            break;
        }
    }
}

void lexer_init(lexer_t *lx, const char *src) {
    lx->src = src;
    lx->pos = 0;
}

token_t lexer_next(lexer_t *lx) {
    token_t tok;
    tok.string = NULL;
    tok.number = 0;
    tok.text[0] = '\0';

    skip_ws_and_comments(lx);
    char c = lx->src[lx->pos];

    if (c == '\0') {
        tok.type = TOK_EOF;
        return tok;
    }

    if (is_alpha(c)) {
        int start = lx->pos;
        while (is_alnum(lx->src[lx->pos])) {
            lx->pos++;
        }
        int len = lx->pos - start;
        if (len >= TOKEN_TEXT_MAX) {
            len = TOKEN_TEXT_MAX - 1;
        }
        memcpy(tok.text, lx->src + start, (size_t)len);
        tok.text[len] = '\0';
        tok.type = TOK_IDENT;
        return tok;
    }

    if (is_digit(c)) {
        int32_t val = 0;
        while (is_digit(lx->src[lx->pos])) {
            val = val * 10 + (lx->src[lx->pos] - '0');
            lx->pos++;
        }
        tok.number = val;
        tok.type = TOK_NUMBER;
        return tok;
    }

    if (c == '"') {
        lx->pos++;
        int start = lx->pos;
        while (lx->src[lx->pos] && lx->src[lx->pos] != '"') {
            lx->pos++;
        }
        int len = lx->pos - start;
        char *s = kmalloc((size_t)len + 1);
        memcpy(s, lx->src + start, (size_t)len);
        s[len] = '\0';
        if (lx->src[lx->pos] == '"') {
            lx->pos++;
        }
        tok.string = s;
        tok.type = TOK_STRING;
        return tok;
    }

    lx->pos++;
    switch (c) {
        case '(': tok.type = TOK_LPAREN; break;
        case ')': tok.type = TOK_RPAREN; break;
        case '{': tok.type = TOK_LBRACE; break;
        case '}': tok.type = TOK_RBRACE; break;
        case ',': tok.type = TOK_COMMA; break;
        case ';': tok.type = TOK_SEMI; break;
        case '+': tok.type = TOK_PLUS; break;
        case '-': tok.type = TOK_MINUS; break;
        case '*': tok.type = TOK_STAR; break;
        case '/': tok.type = TOK_SLASH; break;
        case '%': tok.type = TOK_PERCENT; break;
        case '=':
            if (lx->src[lx->pos] == '=') { lx->pos++; tok.type = TOK_EQ; }
            else { tok.type = TOK_ASSIGN; }
            break;
        case '!':
            if (lx->src[lx->pos] == '=') { lx->pos++; tok.type = TOK_NEQ; }
            else { tok.type = TOK_NOT; }
            break;
        case '<':
            if (lx->src[lx->pos] == '=') { lx->pos++; tok.type = TOK_LE; }
            else { tok.type = TOK_LT; }
            break;
        case '>':
            if (lx->src[lx->pos] == '=') { lx->pos++; tok.type = TOK_GE; }
            else { tok.type = TOK_GT; }
            break;
        case '&':
            if (lx->src[lx->pos] == '&') { lx->pos++; tok.type = TOK_AND; }
            else { tok.type = TOK_ERROR; }
            break;
        case '|':
            if (lx->src[lx->pos] == '|') { lx->pos++; tok.type = TOK_OR; }
            else { tok.type = TOK_ERROR; }
            break;
        default:
            tok.type = TOK_ERROR;
            break;
    }
    return tok;
}
