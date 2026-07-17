#include "codegen.h"

#include "../../string.h"
#include "../bytecode.h"
#include "../lexer.h"

typedef struct {
    uint8_t *code;
    uint32_t code_cap;
    uint32_t code_len;

    char func_names[VM_FUNCS_MAX][TOKEN_TEXT_MAX];
    tbc_func_t funcs[VM_FUNCS_MAX];
    int func_count;

    char global_names[VM_GLOBALS_MAX][TOKEN_TEXT_MAX];
    int global_count;

    char local_names[VM_LOCALS_PER_FRAME_MAX][TOKEN_TEXT_MAX];
    int local_count;
    int in_function;

    int error;
    char error_msg[64];
} compiler_t;

static void copy_name(char *dst, const char *src) {
    int i = 0;
    while (src[i] && i < TOKEN_TEXT_MAX - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void compile_error(compiler_t *c, const char *msg) {
    if (!c->error) {
        c->error = 1;
        copy_name(c->error_msg, msg); /* error_msg is 64 bytes, plenty for these */
    }
}

static void emit_byte(compiler_t *c, uint8_t b) {
    if (c->code_len >= c->code_cap) {
        compile_error(c, "compiled program too large");
        return;
    }
    c->code[c->code_len++] = b;
}

static void emit_op(compiler_t *c, bc_opcode_t op) {
    emit_byte(c, (uint8_t)op);
}

static void emit_u16(compiler_t *c, uint16_t v) {
    emit_byte(c, (uint8_t)(v & 0xFF));
    emit_byte(c, (uint8_t)((v >> 8) & 0xFF));
}

static void emit_i32(compiler_t *c, int32_t v) {
    uint32_t u = (uint32_t)v;
    emit_byte(c, (uint8_t)(u & 0xFF));
    emit_byte(c, (uint8_t)((u >> 8) & 0xFF));
    emit_byte(c, (uint8_t)((u >> 16) & 0xFF));
    emit_byte(c, (uint8_t)((u >> 24) & 0xFF));
}

static void patch_u16(compiler_t *c, uint32_t pos, uint16_t v) {
    c->code[pos] = (uint8_t)(v & 0xFF);
    c->code[pos + 1] = (uint8_t)((v >> 8) & 0xFF);
}

static int find_local(compiler_t *c, const char *name) {
    for (int i = 0; i < c->local_count; i++) {
        if (strcmp(c->local_names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_or_add_local(compiler_t *c, const char *name) {
    int i = find_local(c, name);
    if (i >= 0) {
        return i;
    }
    if (c->local_count >= VM_LOCALS_PER_FRAME_MAX) {
        compile_error(c, "too many local variables in one function");
        return 0;
    }
    copy_name(c->local_names[c->local_count], name);
    return c->local_count++;
}

static int find_global(compiler_t *c, const char *name) {
    for (int i = 0; i < c->global_count; i++) {
        if (strcmp(c->global_names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_or_add_global(compiler_t *c, const char *name) {
    int i = find_global(c, name);
    if (i >= 0) {
        return i;
    }
    if (c->global_count >= VM_GLOBALS_MAX) {
        compile_error(c, "too many global variables");
        return 0;
    }
    copy_name(c->global_names[c->global_count], name);
    return c->global_count++;
}

static int find_func_index(compiler_t *c, const char *name) {
    for (int i = 0; i < c->func_count; i++) {
        if (strcmp(c->func_names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

static void compile_expr(compiler_t *c, ast_node_t *node);
static void compile_stmt(compiler_t *c, ast_node_t *node);

static void compile_var_load(compiler_t *c, const char *name) {
    if (c->in_function) {
        int slot = find_local(c, name);
        if (slot < 0) {
            compile_error(c, "undefined local variable");
            slot = 0;
        }
        emit_op(c, OP_LOAD_LOCAL);
        emit_byte(c, (uint8_t)slot);
    } else {
        int slot = find_global(c, name);
        if (slot < 0) {
            compile_error(c, "undefined global variable");
            slot = 0;
        }
        emit_op(c, OP_LOAD_GLOBAL);
        emit_byte(c, (uint8_t)slot);
    }
}

static void compile_var_store(compiler_t *c, const char *name) {
    if (c->in_function) {
        int slot = find_or_add_local(c, name);
        emit_op(c, OP_STORE_LOCAL);
        emit_byte(c, (uint8_t)slot);
    } else {
        int slot = find_or_add_global(c, name);
        emit_op(c, OP_STORE_GLOBAL);
        emit_byte(c, (uint8_t)slot);
    }
}

static const bc_opcode_t BINOP_TABLE[] = {
    [TOK_PLUS] = OP_ADD, [TOK_MINUS] = OP_SUB, [TOK_STAR] = OP_MUL,
    [TOK_SLASH] = OP_DIV, [TOK_PERCENT] = OP_MOD,
    [TOK_EQ] = OP_EQ, [TOK_NEQ] = OP_NEQ,
    [TOK_LT] = OP_LT, [TOK_LE] = OP_LE, [TOK_GT] = OP_GT, [TOK_GE] = OP_GE,
    [TOK_AND] = OP_AND, [TOK_OR] = OP_OR,
};

static void compile_expr(compiler_t *c, ast_node_t *node) {
    switch (node->kind) {
        case AST_NUM:
            emit_op(c, OP_PUSH_CONST);
            emit_i32(c, node->num);
            return;
        case AST_STR:
            compile_error(c, "strings are not supported in the compiled language");
            return;
        case AST_VAR:
            compile_var_load(c, node->name);
            return;
        case AST_ASSIGN:
            compile_expr(c, node->a);
            emit_op(c, OP_DUP); /* assignment evaluates to the assigned value */
            compile_var_store(c, node->name);
            return;
        case AST_UNOP:
            compile_expr(c, node->a);
            emit_op(c, node->op == TOK_MINUS ? OP_NEG : OP_NOT);
            return;
        case AST_BINOP: {
            compile_expr(c, node->a);
            compile_expr(c, node->b);
            if ((size_t)node->op >= sizeof(BINOP_TABLE) / sizeof(BINOP_TABLE[0])) {
                compile_error(c, "unsupported operator");
                return;
            }
            emit_op(c, BINOP_TABLE[node->op]);
            return;
        }
        case AST_CALL: {
            int argc = 0;
            for (ast_node_t *a = node->list; a; a = a->next) {
                compile_expr(c, a);
                argc++;
            }
            if (strcmp(node->name, "print") == 0) {
                if (argc != 1) {
                    compile_error(c, "print takes exactly one argument");
                }
                emit_op(c, OP_SYS_PRINT);
                return;
            }
            int idx = find_func_index(c, node->name);
            if (idx < 0) {
                compile_error(c, "call to unknown function");
                return;
            }
            if (argc != c->funcs[idx].param_count) {
                compile_error(c, "argument count mismatch");
                return;
            }
            if (argc > VM_MAX_CALL_ARGS) {
                compile_error(c, "too many arguments");
                return;
            }
            emit_op(c, OP_CALL);
            emit_u16(c, (uint16_t)idx);
            return;
        }
        default:
            compile_error(c, "expression expected");
            return;
    }
}

static void compile_stmt(compiler_t *c, ast_node_t *node) {
    switch (node->kind) {
        case AST_BLOCK:
            for (ast_node_t *s = node->list; s; s = s->next) {
                compile_stmt(c, s);
            }
            return;
        case AST_IF: {
            compile_expr(c, node->a);
            emit_op(c, OP_JMP_IF_FALSE);
            uint32_t else_jump = c->code_len;
            emit_u16(c, 0);

            compile_stmt(c, node->b);

            if (node->c) {
                emit_op(c, OP_JMP);
                uint32_t end_jump = c->code_len;
                emit_u16(c, 0);

                patch_u16(c, else_jump, (uint16_t)c->code_len);
                compile_stmt(c, node->c);
                patch_u16(c, end_jump, (uint16_t)c->code_len);
            } else {
                patch_u16(c, else_jump, (uint16_t)c->code_len);
            }
            return;
        }
        case AST_WHILE: {
            uint32_t loop_start = c->code_len;
            compile_expr(c, node->a);
            emit_op(c, OP_JMP_IF_FALSE);
            uint32_t exit_jump = c->code_len;
            emit_u16(c, 0);

            compile_stmt(c, node->b);
            emit_op(c, OP_JMP);
            emit_u16(c, (uint16_t)loop_start);

            patch_u16(c, exit_jump, (uint16_t)c->code_len);
            return;
        }
        case AST_RETURN:
            if (node->a) {
                compile_expr(c, node->a);
            } else {
                emit_op(c, OP_PUSH_CONST);
                emit_i32(c, 0);
            }
            emit_op(c, OP_RET);
            return;
        case AST_EXPR_STMT:
            compile_expr(c, node->a);
            emit_op(c, OP_POP);
            return;
        case AST_FUNC_DEF:
            compile_error(c, "nested functions are not supported");
            return;
        default:
            compile_error(c, "statement expected");
            return;
    }
}

static void compile_function(compiler_t *c, ast_node_t *def) {
    int idx = find_func_index(c, def->name);
    tbc_func_t *fe = &c->funcs[idx];
    fe->entry_addr = (uint16_t)c->code_len;

    c->in_function = 1;
    c->local_count = 0;

    int param_count = 0;
    for (ast_node_t *pn = def->list; pn; pn = pn->next) {
        find_or_add_local(c, pn->name);
        param_count++;
    }
    fe->param_count = (uint8_t)param_count;

    compile_stmt(c, def->b);

    /* Implicit `return 0;` if the body falls through without one. */
    emit_op(c, OP_PUSH_CONST);
    emit_i32(c, 0);
    emit_op(c, OP_RET);

    fe->local_count = (uint8_t)c->local_count;
    c->in_function = 0;
}

int codegen_compile(ast_node_t *prog, uint8_t *out_buf, uint32_t out_buf_size,
                     uint32_t *out_len, char *error_msg, uint32_t error_msg_size) {
    static uint8_t code_buf[4096];

    compiler_t c;
    memset(&c, 0, sizeof(c));
    c.code = code_buf;
    c.code_cap = sizeof(code_buf);

    for (ast_node_t *s = prog->list; s; s = s->next) {
        if (s->kind == AST_FUNC_DEF) {
            if (c.func_count >= VM_FUNCS_MAX) {
                compile_error(&c, "too many functions");
                break;
            }
            copy_name(c.func_names[c.func_count], s->name);
            c.func_count++;
        }
    }

    for (ast_node_t *s = prog->list; s && !c.error; s = s->next) {
        if (s->kind == AST_FUNC_DEF) {
            compile_function(&c, s);
        }
    }

    uint16_t entry_pc = (uint16_t)c.code_len;
    c.in_function = 0;
    for (ast_node_t *s = prog->list; s && !c.error; s = s->next) {
        if (s->kind != AST_FUNC_DEF) {
            compile_stmt(&c, s);
        }
    }
    emit_op(&c, OP_HALT);

    if (c.error) {
        copy_name(error_msg, c.error_msg);
        (void)error_msg_size;
        return -1;
    }

    tbc_header_t hdr;
    hdr.magic = TBC_MAGIC;
    hdr.entry_pc = entry_pc;
    hdr.global_count = (uint16_t)c.global_count;
    hdr.func_count = (uint16_t)c.func_count;
    hdr.code_size = (uint16_t)c.code_len;

    uint32_t total = sizeof(hdr) + (uint32_t)c.func_count * sizeof(tbc_func_t) + c.code_len;
    if (total > out_buf_size) {
        copy_name(error_msg, "compiled program too large for output buffer");
        return -1;
    }

    uint32_t off = 0;
    memcpy(out_buf + off, &hdr, sizeof(hdr));
    off += sizeof(hdr);
    for (int i = 0; i < c.func_count; i++) {
        memcpy(out_buf + off, &c.funcs[i], sizeof(tbc_func_t));
        off += sizeof(tbc_func_t);
    }
    memcpy(out_buf + off, c.code, c.code_len);
    off += c.code_len;

    *out_len = off;
    return 0;
}
