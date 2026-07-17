#pragma once

#include "../lexer.h"

typedef enum {
    AST_NUM,
    AST_STR,
    AST_VAR,
    AST_ASSIGN,
    AST_BINOP,
    AST_UNOP,
    AST_CALL,
    AST_IF,
    AST_WHILE,
    AST_FUNC_DEF,
    AST_RETURN,
    AST_BLOCK,
    AST_EXPR_STMT,
} ast_kind_t;

typedef struct ast_node ast_node_t;

/* One generic node shape for the whole tree, in the tradition of tiny
 * interpreters: `a`/`b`/`c` are kind-specific children, `list` is a
 * singly-linked chain (block statements, call args, function params),
 * and `next` links siblings within such a chain. See eval.c/parser.c for
 * which fields each AST_* kind actually uses. */
struct ast_node {
    ast_kind_t kind;
    token_type_t op; /* AST_BINOP / AST_UNOP */

    int32_t num;  /* AST_NUM */
    char *str;     /* AST_STR - shares the token's kmalloc'd buffer */
    char name[TOKEN_TEXT_MAX]; /* AST_VAR / AST_ASSIGN / AST_CALL / AST_FUNC_DEF */

    ast_node_t *a, *b, *c;
    ast_node_t *list;
    ast_node_t *next;
};

ast_node_t *ast_new(ast_kind_t kind);
