#include "parser.h"

#include "../../string.h"

static void copy_name(char *dst, const char *src) {
    int i = 0;
    while (src[i] && i < TOKEN_TEXT_MAX - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void advance(parser_t *p) {
    p->cur = lexer_next(&p->lexer);
}

void parser_init(parser_t *p, const char *src) {
    lexer_init(&p->lexer, src);
    p->error = 0;
    p->error_msg[0] = '\0';
    advance(p);
}

static void parse_error(parser_t *p, const char *msg) {
    if (!p->error) {
        p->error = 1;
        copy_name(p->error_msg, msg); /* error_msg is 64 bytes, plenty for our short messages */
    }
}

static int check(parser_t *p, token_type_t t) {
    return p->cur.type == t;
}

static int match(parser_t *p, token_type_t t) {
    if (check(p, t)) {
        advance(p);
        return 1;
    }
    return 0;
}

static void expect(parser_t *p, token_type_t t) {
    if (!match(p, t)) {
        parse_error(p, "unexpected token");
    }
}

static int is_kw(parser_t *p, const char *kw) {
    return check(p, TOK_IDENT) && strcmp(p->cur.text, kw) == 0;
}

static ast_node_t *parse_expression(parser_t *p);
static ast_node_t *parse_statement(parser_t *p);

static ast_node_t *parse_block(parser_t *p) {
    expect(p, TOK_LBRACE);
    ast_node_t *blk = ast_new(AST_BLOCK);
    ast_node_t *tail = NULL;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF) && !p->error) {
        ast_node_t *s = parse_statement(p);
        if (!blk->list) { blk->list = s; } else { tail->next = s; }
        tail = s;
    }
    expect(p, TOK_RBRACE);
    return blk;
}

static ast_node_t *parse_primary(parser_t *p) {
    if (check(p, TOK_NUMBER)) {
        ast_node_t *n = ast_new(AST_NUM);
        n->num = p->cur.number;
        advance(p);
        return n;
    }
    if (check(p, TOK_STRING)) {
        ast_node_t *n = ast_new(AST_STR);
        n->str = p->cur.string;
        advance(p);
        return n;
    }
    if (check(p, TOK_IDENT)) {
        char name[TOKEN_TEXT_MAX];
        copy_name(name, p->cur.text);
        advance(p);
        if (check(p, TOK_LPAREN)) {
            advance(p);
            ast_node_t *n = ast_new(AST_CALL);
            copy_name(n->name, name);
            ast_node_t *tail = NULL;
            if (!check(p, TOK_RPAREN)) {
                for (;;) {
                    ast_node_t *arg = parse_expression(p);
                    if (!n->list) { n->list = arg; } else { tail->next = arg; }
                    tail = arg;
                    if (!match(p, TOK_COMMA)) {
                        break;
                    }
                }
            }
            expect(p, TOK_RPAREN);
            return n;
        }
        ast_node_t *n = ast_new(AST_VAR);
        copy_name(n->name, name);
        return n;
    }
    if (match(p, TOK_LPAREN)) {
        ast_node_t *n = parse_expression(p);
        expect(p, TOK_RPAREN);
        return n;
    }
    parse_error(p, "expected expression");
    advance(p); /* avoid getting stuck */
    return ast_new(AST_NUM);
}

static ast_node_t *parse_unary(parser_t *p) {
    if (check(p, TOK_MINUS) || check(p, TOK_NOT)) {
        token_type_t op = p->cur.type;
        advance(p);
        ast_node_t *n = ast_new(AST_UNOP);
        n->op = op;
        n->a = parse_unary(p);
        return n;
    }
    return parse_primary(p);
}

static ast_node_t *parse_binop_level(parser_t *p, ast_node_t *(*next)(parser_t *),
                                      const token_type_t *ops, int nops) {
    ast_node_t *left = next(p);
    for (;;) {
        int matched = 0;
        for (int i = 0; i < nops; i++) {
            if (check(p, ops[i])) {
                token_type_t op = p->cur.type;
                advance(p);
                ast_node_t *right = next(p);
                ast_node_t *n = ast_new(AST_BINOP);
                n->op = op;
                n->a = left;
                n->b = right;
                left = n;
                matched = 1;
                break;
            }
        }
        if (!matched) {
            return left;
        }
    }
}

static ast_node_t *parse_multiplicative(parser_t *p) {
    static const token_type_t ops[] = { TOK_STAR, TOK_SLASH, TOK_PERCENT };
    return parse_binop_level(p, parse_unary, ops, 3);
}

static ast_node_t *parse_additive(parser_t *p) {
    static const token_type_t ops[] = { TOK_PLUS, TOK_MINUS };
    return parse_binop_level(p, parse_multiplicative, ops, 2);
}

static ast_node_t *parse_relational(parser_t *p) {
    static const token_type_t ops[] = { TOK_LT, TOK_LE, TOK_GT, TOK_GE };
    return parse_binop_level(p, parse_additive, ops, 4);
}

static ast_node_t *parse_equality(parser_t *p) {
    static const token_type_t ops[] = { TOK_EQ, TOK_NEQ };
    return parse_binop_level(p, parse_relational, ops, 2);
}

static ast_node_t *parse_and(parser_t *p) {
    static const token_type_t ops[] = { TOK_AND };
    return parse_binop_level(p, parse_equality, ops, 1);
}

static ast_node_t *parse_or(parser_t *p) {
    static const token_type_t ops[] = { TOK_OR };
    return parse_binop_level(p, parse_and, ops, 1);
}

static ast_node_t *parse_assignment(parser_t *p) {
    if (check(p, TOK_IDENT)) {
        parser_t save = *p; /* plain-old-data struct, safe to snapshot/restore */
        char name[TOKEN_TEXT_MAX];
        copy_name(name, p->cur.text);
        advance(p);
        if (check(p, TOK_ASSIGN)) {
            advance(p);
            ast_node_t *value = parse_assignment(p); /* right-associative */
            ast_node_t *n = ast_new(AST_ASSIGN);
            copy_name(n->name, name);
            n->a = value;
            return n;
        }
        *p = save; /* not an assignment - rewind and parse as a normal expression */
    }
    return parse_or(p);
}

static ast_node_t *parse_expression(parser_t *p) {
    return parse_assignment(p);
}

static ast_node_t *parse_statement(parser_t *p) {
    if (check(p, TOK_LBRACE)) {
        return parse_block(p);
    }

    if (is_kw(p, "if")) {
        advance(p);
        expect(p, TOK_LPAREN);
        ast_node_t *cond = parse_expression(p);
        expect(p, TOK_RPAREN);
        ast_node_t *then_blk = parse_block(p);
        ast_node_t *else_blk = NULL;
        if (is_kw(p, "else")) {
            advance(p);
            else_blk = is_kw(p, "if") ? parse_statement(p) : parse_block(p);
        }
        ast_node_t *n = ast_new(AST_IF);
        n->a = cond;
        n->b = then_blk;
        n->c = else_blk;
        return n;
    }

    if (is_kw(p, "while")) {
        advance(p);
        expect(p, TOK_LPAREN);
        ast_node_t *cond = parse_expression(p);
        expect(p, TOK_RPAREN);
        ast_node_t *body = parse_block(p);
        ast_node_t *n = ast_new(AST_WHILE);
        n->a = cond;
        n->b = body;
        return n;
    }

    if (is_kw(p, "func")) {
        advance(p);
        char name[TOKEN_TEXT_MAX];
        if (check(p, TOK_IDENT)) {
            copy_name(name, p->cur.text);
            advance(p);
        } else {
            parse_error(p, "expected function name");
            name[0] = '\0';
        }
        expect(p, TOK_LPAREN);
        ast_node_t *params = NULL;
        ast_node_t *ptail = NULL;
        if (!check(p, TOK_RPAREN)) {
            for (;;) {
                if (check(p, TOK_IDENT)) {
                    ast_node_t *pn = ast_new(AST_VAR);
                    copy_name(pn->name, p->cur.text);
                    advance(p);
                    if (!params) { params = pn; } else { ptail->next = pn; }
                    ptail = pn;
                } else {
                    parse_error(p, "expected parameter name");
                    break;
                }
                if (!match(p, TOK_COMMA)) {
                    break;
                }
            }
        }
        expect(p, TOK_RPAREN);
        ast_node_t *body = parse_block(p);
        ast_node_t *n = ast_new(AST_FUNC_DEF);
        copy_name(n->name, name);
        n->list = params;
        n->b = body;
        return n;
    }

    if (is_kw(p, "return")) {
        advance(p);
        ast_node_t *n = ast_new(AST_RETURN);
        if (!check(p, TOK_SEMI)) {
            n->a = parse_expression(p);
        }
        expect(p, TOK_SEMI);
        return n;
    }

    ast_node_t *e = parse_expression(p);
    expect(p, TOK_SEMI);
    ast_node_t *n = ast_new(AST_EXPR_STMT);
    n->a = e;
    return n;
}

ast_node_t *parse_program(parser_t *p) {
    ast_node_t *prog = ast_new(AST_BLOCK);
    ast_node_t *tail = NULL;
    while (!check(p, TOK_EOF) && !p->error) {
        ast_node_t *s = parse_statement(p);
        if (!prog->list) { prog->list = s; } else { tail->next = s; }
        tail = s;
    }
    return prog;
}
