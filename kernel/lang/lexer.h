#pragma once

/* Shared tokenizer used by both the scripting language (kernel/lang/script)
 * and the tiny compiler (kernel/lang/compiler). Keywords are not baked in
 * here - callers recognize keywords by comparing TOK_IDENT text, since the
 * two languages don't share a keyword set. */

#include <stdint.h>

typedef enum {
    TOK_EOF,
    TOK_IDENT,
    TOK_NUMBER,
    TOK_STRING,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_COMMA,
    TOK_SEMI,
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_ASSIGN,
    TOK_EQ,
    TOK_NEQ,
    TOK_LT,
    TOK_LE,
    TOK_GT,
    TOK_GE,
    TOK_NOT,
    TOK_AND,
    TOK_OR,
    TOK_ERROR,
} token_type_t;

#define TOKEN_TEXT_MAX 32

typedef struct {
    token_type_t type;
    char text[TOKEN_TEXT_MAX]; /* TOK_IDENT */
    int32_t number;             /* TOK_NUMBER */
    char *string;                /* TOK_STRING - kmalloc'd, owned by the AST node it ends up in */
} token_t;

typedef struct {
    const char *src;
    int pos;
} lexer_t;

void lexer_init(lexer_t *lx, const char *src);
token_t lexer_next(lexer_t *lx);
