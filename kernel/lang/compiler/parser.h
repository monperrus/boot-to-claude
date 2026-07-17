#pragma once

#include "../lexer.h"
#include "../script/ast.h"

typedef struct {
    lexer_t lexer;
    token_t cur;
    int error;
    char error_msg[64];
} tc_parser_t;

void tc_parser_init(tc_parser_t *p, const char *src);

/* Parses the whole source as a top-level AST_BLOCK of statements. Check
 * p->error afterwards. Unlike the scripting language's parser, string
 * literals are a parse error here - the compiled language is int-only. */
ast_node_t *tc_parse_program(tc_parser_t *p);
