#pragma once

#include "../lexer.h"
#include "ast.h"

typedef struct {
    lexer_t lexer;
    token_t cur;
    int error;
    char error_msg[64];
} parser_t;

void parser_init(parser_t *p, const char *src);

/* Parses the whole source as a top-level AST_BLOCK of statements. Check
 * p->error afterwards before evaluating the result. */
ast_node_t *parse_program(parser_t *p);
