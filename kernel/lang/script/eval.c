#include "eval.h"

#include "../../console.h"
#include "../../fs/tinyfs.h"
#include "../../memory.h"
#include "../../serial.h"
#include "../../string.h"
#include "ast.h"
#include "parser.h"

typedef enum { VAL_INT, VAL_STR } value_type_t;

typedef struct {
    value_type_t type;
    union {
        int32_t i;
        char *s;
    } as;
} value_t;

#define MAX_VARS 32

typedef struct {
    char names[MAX_VARS][TOKEN_TEXT_MAX];
    value_t values[MAX_VARS];
    int count;
} env_t;

#define MAX_FUNCS 16

typedef struct {
    char name[TOKEN_TEXT_MAX];
    ast_node_t *def;
} func_entry_t;

typedef struct {
    func_entry_t funcs[MAX_FUNCS];
    int func_count;
} interp_t;

typedef enum { EXEC_NORMAL, EXEC_RETURN } exec_signal_t;

typedef struct {
    exec_signal_t signal;
    value_t return_value;
} exec_result_t;

static const value_t VAL_ZERO = { VAL_INT, { .i = 0 } };

static int env_find(env_t *env, const char *name) {
    for (int i = 0; i < env->count; i++) {
        if (strcmp(env->names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

static value_t env_get(env_t *env, const char *name) {
    int i = env_find(env, name);
    return i < 0 ? VAL_ZERO : env->values[i];
}

static void env_set(env_t *env, const char *name, value_t v) {
    int i = env_find(env, name);
    if (i >= 0) {
        env->values[i] = v;
        return;
    }
    if (env->count < MAX_VARS) {
        int n = 0;
        while (name[n] && n < TOKEN_TEXT_MAX - 1) {
            env->names[env->count][n] = name[n];
            n++;
        }
        env->names[env->count][n] = '\0';
        env->values[env->count] = v;
        env->count++;
    }
    /* else: variable table full - silently dropped, tiny scope */
}

static int to_int(value_t v) {
    if (v.type == VAL_INT) {
        return v.as.i;
    }
    return (v.as.s && v.as.s[0]) ? 1 : 0;
}

static char *str_concat(const char *a, const char *b) {
    size_t la = strlen(a);
    size_t lb = strlen(b);
    char *r = kmalloc(la + lb + 1);
    memcpy(r, a, la);
    memcpy(r + la, b, lb);
    r[la + lb] = '\0';
    return r;
}

static void print_value(value_t v) {
    if (v.type == VAL_STR) {
        console_write(v.as.s);
        serial_write(v.as.s);
    } else {
        char buf[16];
        itoa(v.as.i, buf, 10);
        console_write(buf);
        serial_write(buf);
    }
}

static value_t eval_binop(token_type_t op, value_t l, value_t r) {
    if (op == TOK_PLUS && (l.type == VAL_STR || r.type == VAL_STR)) {
        char lb[16], rb[16];
        const char *ls = (l.type == VAL_STR) ? l.as.s : itoa(l.as.i, lb, 10);
        const char *rs = (r.type == VAL_STR) ? r.as.s : itoa(r.as.i, rb, 10);
        return (value_t){ VAL_STR, { .s = str_concat(ls, rs) } };
    }
    if (op == TOK_EQ || op == TOK_NEQ) {
        int eq;
        if (l.type == VAL_STR && r.type == VAL_STR) {
            eq = strcmp(l.as.s, r.as.s) == 0;
        } else if (l.type == VAL_INT && r.type == VAL_INT) {
            eq = l.as.i == r.as.i;
        } else {
            eq = 0;
        }
        return (value_t){ VAL_INT, { .i = (op == TOK_EQ) ? eq : !eq } };
    }

    int li = to_int(l);
    int ri = to_int(r);
    switch (op) {
        case TOK_PLUS:    return (value_t){ VAL_INT, { .i = li + ri } };
        case TOK_MINUS:   return (value_t){ VAL_INT, { .i = li - ri } };
        case TOK_STAR:    return (value_t){ VAL_INT, { .i = li * ri } };
        case TOK_SLASH:   return (value_t){ VAL_INT, { .i = ri != 0 ? li / ri : 0 } };
        case TOK_PERCENT: return (value_t){ VAL_INT, { .i = ri != 0 ? li % ri : 0 } };
        case TOK_LT:      return (value_t){ VAL_INT, { .i = li < ri } };
        case TOK_LE:      return (value_t){ VAL_INT, { .i = li <= ri } };
        case TOK_GT:      return (value_t){ VAL_INT, { .i = li > ri } };
        case TOK_GE:      return (value_t){ VAL_INT, { .i = li >= ri } };
        default:          return VAL_ZERO;
    }
}

static ast_node_t *find_func(interp_t *interp, const char *name) {
    for (int i = 0; i < interp->func_count; i++) {
        if (strcmp(interp->funcs[i].name, name) == 0) {
            return interp->funcs[i].def;
        }
    }
    return NULL;
}

static void register_func(interp_t *interp, ast_node_t *def) {
    if (interp->func_count >= MAX_FUNCS) {
        return;
    }
    func_entry_t *e = &interp->funcs[interp->func_count++];
    int n = 0;
    while (def->name[n] && n < TOKEN_TEXT_MAX - 1) {
        e->name[n] = def->name[n];
        n++;
    }
    e->name[n] = '\0';
    e->def = def;
}

static value_t eval_expr(interp_t *interp, env_t *env, ast_node_t *node);
static exec_result_t exec_stmt(interp_t *interp, env_t *env, ast_node_t *node);

static exec_result_t exec_block(interp_t *interp, env_t *env, ast_node_t *block) {
    exec_result_t r = { EXEC_NORMAL, VAL_ZERO };
    for (ast_node_t *s = block->list; s; s = s->next) {
        r = exec_stmt(interp, env, s);
        if (r.signal == EXEC_RETURN) {
            return r;
        }
    }
    return r;
}

static value_t eval_call(interp_t *interp, env_t *env, ast_node_t *node) {
    value_t args[8];
    int argc = 0;
    for (ast_node_t *a = node->list; a && argc < 8; a = a->next) {
        args[argc++] = eval_expr(interp, env, a);
    }

    if (strcmp(node->name, "print") == 0) {
        for (int i = 0; i < argc; i++) {
            if (i > 0) {
                console_write(" ");
                serial_write(" ");
            }
            print_value(args[i]);
        }
        console_write("\n");
        serial_write("\n");
        return VAL_ZERO;
    }

    if (strcmp(node->name, "fread") == 0 && argc >= 1 && args[0].type == VAL_STR) {
        static char buf[TINYFS_MAX_FILE_SIZE + 1];
        uint32_t len = 0;
        if (tinyfs_read(args[0].as.s, buf, sizeof(buf) - 1, &len) == 0) {
            buf[len] = '\0';
            char *out = kmalloc(len + 1);
            memcpy(out, buf, len + 1);
            return (value_t){ VAL_STR, { .s = out } };
        }
        return (value_t){ VAL_STR, { .s = "" } };
    }

    if (strcmp(node->name, "fwrite") == 0 && argc >= 2 && args[0].type == VAL_STR) {
        const char *content = (args[1].type == VAL_STR) ? args[1].as.s : "";
        tinyfs_create(args[0].as.s); /* ignored if it already exists */
        int rc = tinyfs_write(args[0].as.s, content, (uint32_t)strlen(content));
        return (value_t){ VAL_INT, { .i = rc == 0 } };
    }

    if (strcmp(node->name, "flist") == 0) {
        tinyfs_dirent_t entries[TINYFS_NUM_INODES];
        int n = tinyfs_list(entries, TINYFS_NUM_INODES);
        for (int i = 0; i < n; i++) {
            console_write(entries[i].name);
            console_write("\n");
            serial_write(entries[i].name);
            serial_write("\n");
        }
        return (value_t){ VAL_INT, { .i = n } };
    }

    ast_node_t *def = find_func(interp, node->name);
    if (!def) {
        return VAL_ZERO; /* unknown function - tiny scope, no error propagation */
    }

    env_t local;
    local.count = 0;
    int i = 0;
    for (ast_node_t *pn = def->list; pn; pn = pn->next, i++) {
        env_set(&local, pn->name, i < argc ? args[i] : VAL_ZERO);
    }

    exec_result_t r = exec_block(interp, &local, def->b);
    return r.signal == EXEC_RETURN ? r.return_value : VAL_ZERO;
}

static value_t eval_expr(interp_t *interp, env_t *env, ast_node_t *node) {
    switch (node->kind) {
        case AST_NUM:
            return (value_t){ VAL_INT, { .i = node->num } };
        case AST_STR:
            return (value_t){ VAL_STR, { .s = node->str } };
        case AST_VAR:
            return env_get(env, node->name);
        case AST_ASSIGN: {
            value_t v = eval_expr(interp, env, node->a);
            env_set(env, node->name, v);
            return v;
        }
        case AST_UNOP: {
            value_t v = eval_expr(interp, env, node->a);
            if (node->op == TOK_MINUS) {
                return (value_t){ VAL_INT, { .i = -to_int(v) } };
            }
            return (value_t){ VAL_INT, { .i = !to_int(v) } };
        }
        case AST_BINOP: {
            if (node->op == TOK_AND) {
                value_t l = eval_expr(interp, env, node->a);
                if (!to_int(l)) {
                    return VAL_ZERO;
                }
                value_t r = eval_expr(interp, env, node->b);
                return (value_t){ VAL_INT, { .i = to_int(r) != 0 } };
            }
            if (node->op == TOK_OR) {
                value_t l = eval_expr(interp, env, node->a);
                if (to_int(l)) {
                    return (value_t){ VAL_INT, { .i = 1 } };
                }
                value_t r = eval_expr(interp, env, node->b);
                return (value_t){ VAL_INT, { .i = to_int(r) != 0 } };
            }
            value_t l = eval_expr(interp, env, node->a);
            value_t r = eval_expr(interp, env, node->b);
            return eval_binop(node->op, l, r);
        }
        case AST_CALL:
            return eval_call(interp, env, node);
        default:
            return VAL_ZERO;
    }
}

static exec_result_t exec_stmt(interp_t *interp, env_t *env, ast_node_t *node) {
    exec_result_t normal = { EXEC_NORMAL, VAL_ZERO };

    switch (node->kind) {
        case AST_BLOCK:
            return exec_block(interp, env, node);
        case AST_IF: {
            value_t c = eval_expr(interp, env, node->a);
            if (to_int(c)) {
                return exec_stmt(interp, env, node->b);
            }
            if (node->c) {
                return exec_stmt(interp, env, node->c);
            }
            return normal;
        }
        case AST_WHILE:
            while (to_int(eval_expr(interp, env, node->a))) {
                exec_result_t r = exec_stmt(interp, env, node->b);
                if (r.signal == EXEC_RETURN) {
                    return r;
                }
            }
            return normal;
        case AST_FUNC_DEF:
            /* top-level defs are pre-registered by script_run; a nested
             * func statement (inside a block) registers here instead */
            register_func(interp, node);
            return normal;
        case AST_RETURN: {
            exec_result_t r = { EXEC_RETURN, VAL_ZERO };
            if (node->a) {
                r.return_value = eval_expr(interp, env, node->a);
            }
            return r;
        }
        case AST_EXPR_STMT:
            eval_expr(interp, env, node->a);
            return normal;
        default:
            return normal;
    }
}

void script_run(const char *src) {
    parser_t p;
    parser_init(&p, src);
    ast_node_t *prog = parse_program(&p);

    if (p.error) {
        console_write("script: parse error: ");
        console_write(p.error_msg);
        console_write("\n");
        serial_write("script: parse error: ");
        serial_write(p.error_msg);
        serial_write("\n");
        return;
    }

    interp_t interp;
    interp.func_count = 0;
    env_t global;
    global.count = 0;

    for (ast_node_t *s = prog->list; s; s = s->next) {
        if (s->kind == AST_FUNC_DEF) {
            register_func(&interp, s);
        }
    }
    for (ast_node_t *s = prog->list; s; s = s->next) {
        if (s->kind != AST_FUNC_DEF) {
            exec_result_t r = exec_stmt(&interp, &global, s);
            if (r.signal == EXEC_RETURN) {
                break;
            }
        }
    }
}
