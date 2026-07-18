#include "cc.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

/* Naive, non-optimizing codegen: every intermediate expression value
 * lives in EAX; binary operators spill the left operand to the real
 * machine stack (push/pop) while the right operand is computed - no
 * register allocation. Emits AS-syntax assembly text (mirrors
 * `gcc -S | as`). See docs/toolchain.md for the calling convention and
 * builtin mechanism. */

static FILE *g_out;
static cc_target_t g_target;
static int g_label_id;
static char g_cur_func[MAX_NAME];

typedef struct loop_ctx {
    char break_label[MAX_NAME];
    char continue_label[MAX_NAME];
    struct loop_ctx *parent;
} loop_ctx_t;
static loop_ctx_t *g_loop;

static void out(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_out, fmt, ap);
    va_end(ap);
}

static void new_label(char *buf) {
    snprintf(buf, MAX_NAME, ".L%d", g_label_id++);
}

static void codegen_expr(node_t *n);
static void codegen_addr(node_t *n);
static void codegen_stmt(node_t *n);
static void codegen_call_or_builtin(node_t *n);

/* ---------------- loads/stores sized per C type ---------------- */

static int is_byte_type(type_t *t) { return t->kind == TY_CHAR; }

/* Address already in EAX; loads the value (sized per `t`) into EAX. */
static void load_value(type_t *t) {
    if (t->kind == TY_ARRAY) return; /* arrays decay to their own address - nothing to load */
    if (is_byte_type(t)) {
        out("    movzx eax, byte [eax]\n");
    } else {
        out("    mov eax, [eax]\n");
    }
}

/* Stores EAX's value into the address in ECX. */
static void store_value(type_t *t) {
    if (is_byte_type(t)) {
        out("    mov [ecx], al\n");
    } else {
        out("    mov [ecx], eax\n");
    }
}

/* ---------------- lvalue addresses ---------------- */

/* Address of a variable-like storage location, given the is_global/
 * is_static_local/frame_offset/asm_label fields ND_VAR and an indirect
 * ND_CALL's callee both carry (see cc.h). */
static void codegen_var_addr(node_t *n) {
    if (n->is_global || n->is_static_local) {
        out("    lea eax, [%s]\n", n->asm_label);
    } else {
        out("    lea eax, [ebp%+d]\n", n->frame_offset);
    }
}

static void codegen_addr(node_t *n) {
    switch (n->kind) {
        case ND_VAR:
            codegen_var_addr(n);
            return;
        case ND_MEMBER:
            codegen_addr(n->a);
            if (n->num) out("    add eax, %lld\n", (long long)n->num);
            return;
        case ND_ARROW:
            codegen_expr(n->a);
            if (n->num) out("    add eax, %lld\n", (long long)n->num);
            return;
        case ND_DEREF:
            codegen_expr(n->a);
            return;
        default:
            fprintf(stderr, "cc: internal error: not an lvalue (kind %d)\n", n->kind);
            exit(1);
    }
}

/* ---------------- expressions ---------------- */

static void codegen_binop(node_t *n) {
    int a_is_ptr = n->a->type->kind == TY_PTR || n->a->type->kind == TY_ARRAY;
    codegen_expr(n->b);
    out("    push eax\n");
    codegen_expr(n->a);
    out("    pop ecx\n");
    /* Pointer arithmetic: scale whichever side is the integer operand.
     * eax=a, ecx=b at this point (per the evaluation order above). */
    if ((n->op == TK_PLUS || n->op == TK_MINUS) && (n->type->kind == TY_PTR || n->type->kind == TY_ARRAY)) {
        int sz = type_size(n->type->base);
        if (a_is_ptr) {
            if (sz > 1) out("    imul ecx, ecx, %d\n", sz);
        } else {
            /* eax=int, ecx=pointer - scale eax then swap via the stack
             * (no xchg instruction in this ISA) so eax ends up holding
             * the pointer and ecx the scaled offset, matching every
             * other case's eax-is-left-operand convention below. */
            if (sz > 1) out("    imul eax, eax, %d\n", sz);
            out("    push eax\n    mov eax, ecx\n    pop ecx\n");
        }
    }

    switch (n->op) {
        case TK_PLUS: out("    add eax, ecx\n"); return;
        case TK_MINUS: out("    sub eax, ecx\n"); return;
        case TK_STAR: out("    imul eax, ecx\n"); return;
        case TK_SLASH: out("    cdq\n    idiv ecx\n"); return;
        case TK_PERCENT: out("    cdq\n    idiv ecx\n    mov eax, edx\n"); return;
        case TK_AMP: out("    and eax, ecx\n"); return;
        case TK_PIPE: out("    or eax, ecx\n"); return;
        case TK_CARET: out("    xor eax, ecx\n"); return;
        /* eax=value, ecx=count already, from the evaluation order above -
         * the shift-count-in-CL hardware requirement is satisfied
         * directly, no register shuffling needed. */
        case TK_SHL: out("    shl eax, cl\n"); return;
        case TK_SHR: out("    shr eax, cl\n"); return;
        case TK_EQ: out("    cmp eax, ecx\n    sete al\n    movzx eax, al\n"); return;
        case TK_NEQ: out("    cmp eax, ecx\n    setne al\n    movzx eax, al\n"); return;
        case TK_LT: out("    cmp eax, ecx\n    setl al\n    movzx eax, al\n"); return;
        case TK_LE: out("    cmp eax, ecx\n    setle al\n    movzx eax, al\n"); return;
        case TK_GT: out("    cmp eax, ecx\n    setg al\n    movzx eax, al\n"); return;
        case TK_GE: out("    cmp eax, ecx\n    setge al\n    movzx eax, al\n"); return;
        default:
            fprintf(stderr, "cc: internal error: unsupported binop\n");
            exit(1);
    }
}

static void codegen_expr(node_t *n) {
    switch (n->kind) {
        case ND_NUM:
            out("    mov eax, %lld\n", (long long)n->num);
            return;
        case ND_STR:
            out("    lea eax, [%s]\n", n->asm_label);
            return;
        case ND_VAR:
            codegen_addr(n);
            /* A bare function name (e.g. passed as a callback: tinyfs_init(
             * disk_block_read, ...)) decays to its own address - there's no
             * separate "function value" stored anywhere to load, unlike an
             * ordinary global whose address holds its value. Loading here
             * would dereference the function's own machine code bytes. */
            if (n->type->kind != TY_FUNC) load_value(n->type);
            return;
        case ND_ASSIGN: {
            codegen_addr(n->a);
            out("    push eax\n");
            codegen_expr(n->b);
            out("    pop ecx\n");
            store_value(n->a->type);
            return;
        }
        case ND_COMPOUND_ASSIGN: {
            /* eax/ecx get clobbered (idiv even clobbers edx) while
             * computing the new value, so &a is kept on the real stack
             * throughout and only popped fresh at the very end - a
             * register copy of it wouldn't survive an idiv/cdq. */
            codegen_addr(n->a);
            out("    push eax\n");             /* [&a] */
            codegen_expr(n->b);
            out("    push eax\n");               /* [&a, b_value] */
            out("    pop ecx\n");                 /* ecx = b_value */
            out("    mov edx, [esp]\n");            /* edx = &a (peek, stack still [&a]) */
            out("    mov eax, [edx]\n");              /* eax = current value of a */
            switch (n->op) {
                case TK_PLUSEQ: out("    add eax, ecx\n"); break;
                case TK_MINUSEQ: out("    sub eax, ecx\n"); break;
                case TK_ANDEQ: out("    and eax, ecx\n"); break;
                case TK_OREQ: out("    or eax, ecx\n"); break;
                case TK_XOREQ: out("    xor eax, ecx\n"); break;
                case TK_STAREQ: out("    imul eax, ecx\n"); break;
                case TK_SLASHEQ: out("    cdq\n    idiv ecx\n"); break;
                case TK_PERCENTEQ: out("    cdq\n    idiv ecx\n    mov eax, edx\n"); break;
                /* ecx already holds b_value (the shift count) in CL, no
                 * register shuffling needed - same as codegen_binop. */
                case TK_SHLEQ: out("    shl eax, cl\n"); break;
                case TK_SHREQ: out("    shr eax, cl\n"); break;
                default: fprintf(stderr, "cc: internal error: unsupported compound-assign op\n"); exit(1);
            }
            out("    pop ecx\n");    /* ecx = &a, fresh from the stack (not the possibly-clobbered edx copy) */
            store_value(n->a->type);
            return;
        }
        case ND_BINOP:
            codegen_binop(n);
            return;
        case ND_LOGAND: {
            char lfalse[MAX_NAME], lend[MAX_NAME];
            new_label(lfalse); new_label(lend);
            codegen_expr(n->a);
            out("    cmp eax, 0\n    je %s\n", lfalse);
            codegen_expr(n->b);
            out("    cmp eax, 0\n    je %s\n", lfalse);
            out("    mov eax, 1\n    jmp %s\n", lend);
            out("%s:\n    mov eax, 0\n%s:\n", lfalse, lend);
            return;
        }
        case ND_LOGOR: {
            char ltrue[MAX_NAME], lend[MAX_NAME];
            new_label(ltrue); new_label(lend);
            codegen_expr(n->a);
            out("    cmp eax, 0\n    jne %s\n", ltrue);
            codegen_expr(n->b);
            out("    cmp eax, 0\n    jne %s\n", ltrue);
            out("    mov eax, 0\n    jmp %s\n", lend);
            out("%s:\n    mov eax, 1\n%s:\n", ltrue, lend);
            return;
        }
        case ND_UNARY_MINUS:
            codegen_expr(n->a);
            out("    neg eax\n");
            return;
        case ND_NOT:
            codegen_expr(n->a);
            out("    cmp eax, 0\n    sete al\n    movzx eax, al\n");
            return;
        case ND_BITNOT:
            codegen_expr(n->a);
            out("    not eax\n");
            return;
        case ND_ADDR:
            codegen_addr(n->a);
            return;
        case ND_DEREF:
            codegen_expr(n->a);
            load_value(n->type);
            return;
        case ND_PREINC: case ND_PREDEC: case ND_POSTINC: case ND_POSTDEC: {
            /* int/pointer only (4-byte) - ++/-- on a char never appears
             * in this codebase, see docs/toolchain.md. */
            int is_dec = (n->kind == ND_PREDEC || n->kind == ND_POSTDEC);
            int is_post = (n->kind == ND_POSTINC || n->kind == ND_POSTDEC);
            int step = (n->a->type->kind == TY_PTR) ? type_size(n->a->type->base) : 1;
            codegen_addr(n->a);
            out("    mov ecx, eax\n");     /* ecx = address */
            out("    mov eax, [ecx]\n");     /* eax = old value */
            if (is_post) out("    mov edx, eax\n");
            out("    %s eax, %d\n", is_dec ? "sub" : "add", step);
            out("    mov [ecx], eax\n");
            if (is_post) out("    mov eax, edx\n");
            return;
        }
        case ND_CALL:
            codegen_call_or_builtin(n);
            return;
        case ND_CAST:
            codegen_expr(n->a);
            if (is_byte_type(n->type)) out("    movzx eax, al\n");
            return;
        case ND_SIZEOF:
            out("    mov eax, %d\n", type_size(n->type));
            return;
        case ND_TERNARY: {
            char lfalse[MAX_NAME], lend[MAX_NAME];
            new_label(lfalse); new_label(lend);
            codegen_expr(n->a);
            out("    cmp eax, 0\n    je %s\n", lfalse);
            codegen_expr(n->b);
            out("    jmp %s\n%s:\n", lend, lfalse);
            codegen_expr(n->c);
            out("%s:\n", lend);
            return;
        }
        case ND_MEMBER: case ND_ARROW:
            codegen_addr(n);
            load_value(n->type);
            return;
        default:
            fprintf(stderr, "cc: internal error: unsupported expression kind %d\n", n->kind);
            exit(1);
    }
}

/* ---------------- calls + builtins ---------------- */

extern int builtin_emit_call(const char *name, node_t *args, cc_target_t target,
                              void (*emit_arg)(node_t *), void (*emitf)(const char *, ...));

static void codegen_call_or_builtin(node_t *n) {
    if (!n->is_indirect_call && builtin_emit_call(n->name, n->list, g_target, codegen_expr, out)) return;

    int argc = 0;
    node_t *args[16];
    for (node_t *a = n->list; a; a = a->next) args[argc++] = a;
    for (int i = argc - 1; i >= 0; i--) {
        codegen_expr(args[i]);
        out("    push eax\n");
    }
    if (n->is_indirect_call) {
        /* Callee is a function-pointer variable (e.g. tinyfs_init's
         * read_fn/write_fn callbacks), not a directly callable symbol -
         * load its stored address (always a plain 4-byte pointer, so no
         * load_value/is_byte_type dance needed) and call through it.
         * Done last, after all args are already pushed, so this doesn't
         * need to protect eax from being clobbered by arg evaluation. */
        codegen_var_addr(n);
        out("    mov eax, [eax]\n");
        out("    call eax\n");
    } else {
        out("    call %s\n", n->name);
    }
    if (argc) out("    add esp, %d\n", argc * 4);
}

/* ---------------- statements ---------------- */

static void codegen_stmt(node_t *n) {
    switch (n->kind) {
        case ND_BLOCK:
            for (node_t *s = n->list; s; s = s->next) codegen_stmt(s);
            return;
        case ND_EXPR_STMT:
            codegen_expr(n->a);
            return;
        case ND_VAR_DECL:
            if (n->is_static_local) return; /* emitted into .data/.bss separately */
            /* Brace-init lists (`{1,2,3}`) are only supported for
             * globals/statics (compile-time .data, see emit_data_bytes)
             * - this codebase never brace-initializes a plain (stack)
             * local, only scalars. */
            if (n->init) {
                out("    lea eax, [ebp%+d]\n", n->frame_offset);
                out("    push eax\n");
                codegen_expr(n->init);
                out("    pop ecx\n");
                store_value(n->type);
            }
            return;
        case ND_IF: {
            char lelse[MAX_NAME], lend[MAX_NAME];
            new_label(lelse); new_label(lend);
            codegen_expr(n->a);
            out("    cmp eax, 0\n    je %s\n", lelse);
            codegen_stmt(n->b);
            out("    jmp %s\n", lend);
            out("%s:\n", lelse);
            if (n->c) codegen_stmt(n->c);
            out("%s:\n", lend);
            return;
        }
        case ND_WHILE: {
            char ltop[MAX_NAME], lend[MAX_NAME];
            new_label(ltop); new_label(lend);
            loop_ctx_t lc = { "", "", g_loop };
            safe_strcpy(lc.break_label, lend, MAX_NAME);
            safe_strcpy(lc.continue_label, ltop, MAX_NAME);
            g_loop = &lc;
            out("%s:\n", ltop);
            codegen_expr(n->a);
            out("    cmp eax, 0\n    je %s\n", lend);
            codegen_stmt(n->b);
            out("    jmp %s\n%s:\n", ltop, lend);
            g_loop = lc.parent;
            return;
        }
        case ND_DOWHILE: {
            char ltop[MAX_NAME], lend[MAX_NAME], lcont[MAX_NAME];
            new_label(ltop); new_label(lend); new_label(lcont);
            loop_ctx_t lc = { "", "", g_loop };
            safe_strcpy(lc.break_label, lend, MAX_NAME);
            safe_strcpy(lc.continue_label, lcont, MAX_NAME);
            g_loop = &lc;
            out("%s:\n", ltop);
            codegen_stmt(n->b);
            out("%s:\n", lcont);
            codegen_expr(n->a);
            out("    cmp eax, 0\n    jne %s\n", ltop);
            out("%s:\n", lend);
            g_loop = lc.parent;
            return;
        }
        case ND_FOR: {
            char ltop[MAX_NAME], lend[MAX_NAME], lcont[MAX_NAME];
            new_label(ltop); new_label(lend); new_label(lcont);
            if (n->a) {
                if (n->a->kind == ND_VAR_DECL) {
                    for (node_t *d = n->a; d; d = d->next) codegen_stmt(d);
                } else {
                    codegen_expr(n->a);
                }
            }
            loop_ctx_t lc = { "", "", g_loop };
            safe_strcpy(lc.break_label, lend, MAX_NAME);
            safe_strcpy(lc.continue_label, lcont, MAX_NAME);
            g_loop = &lc;
            out("%s:\n", ltop);
            if (n->b) { codegen_expr(n->b); out("    cmp eax, 0\n    je %s\n", lend); }
            codegen_stmt(n->d);
            out("%s:\n", lcont);
            if (n->c) codegen_expr(n->c);
            out("    jmp %s\n%s:\n", ltop, lend);
            g_loop = lc.parent;
            return;
        }
        case ND_RETURN:
            if (n->a) codegen_expr(n->a);
            out("    mov esp, ebp\n    pop ebp\n    ret\n");
            return;
        case ND_BREAK:
            out("    jmp %s\n", g_loop->break_label);
            return;
        case ND_CONTINUE:
            out("    jmp %s\n", g_loop->continue_label);
            return;
        case ND_NOP:
            return;
        default:
            fprintf(stderr, "cc: internal error: unsupported statement kind %d\n", n->kind);
            exit(1);
    }
}

/* ---------------- top level ---------------- */

static void emit_data_bytes(node_t *init, type_t *t);

static void emit_scalar_bytes(node_t *v, type_t *t) {
    if (t->kind == TY_CHAR) out("    db %lld\n", (long long)v->num);
    else out("    dd %lld\n", (long long)v->num);
}

static void emit_data_bytes(node_t *init, type_t *t) {
    if (!init) {
        out("    resb %d\n", type_size(t));
        return;
    }
    if (t->kind == TY_ARRAY && init->kind == ND_BLOCK) {
        int count = 0;
        for (node_t *v = init->list; v; v = v->next) { emit_scalar_bytes(v, t->base); count++; }
        int pad = t->array_len - count;
        for (int i = 0; i < pad; i++) out("    resb %d\n", type_size(t->base));
        return;
    }
    emit_scalar_bytes(init, t);
}

static void codegen_func(node_t *fn) {
    safe_strcpy(g_cur_func, fn->name, MAX_NAME);
    if (fn->is_global) out("global %s\n", fn->name); /* static functions keep internal linkage - no `global` */
    out("%s:\n", fn->name);
    out("    push ebp\n    mov ebp, esp\n");
    if (fn->num) out("    sub esp, %d\n", (int)fn->num);
    codegen_stmt(fn->b);
    out("    mov eax, 0\n    mov esp, ebp\n    pop ebp\n    ret\n");
}

extern int is_builtin_name(const char *name);

#define MAX_EXTERNS 64
static char g_externs[MAX_EXTERNS][MAX_NAME];
static int g_n_externs;

static int already_extern(const char *name) {
    for (int i = 0; i < g_n_externs; i++) if (strcmp(g_externs[i], name) == 0) return 1;
    return 0;
}

#define MAX_STRINGS 256
static node_t *g_strings[MAX_STRINGS];

#define MAX_STATICS 64
static node_t *g_statics[MAX_STATICS];
static int g_n_statics;
static int g_n_strings;

/* Walks every reachable node (function bodies, global initializers - a/b/
 * c/d/list/init/next cover every kind of child link used anywhere in the
 * AST), collecting (not yet emitting - the caller isn't in the right
 * section context here) two things in one pass: names of called-but-not-
 * locally-defined, non-builtin functions (need `extern`), and string
 * literal nodes (need their bytes emitted into a data section later). */
static void walk_collect(node_t *n, node_t *prog) {
    if (!n) return;
    if (n->kind == ND_CALL) {
        int is_local = 0;
        for (node_t *f = prog; f; f = f->next) {
            if (f->kind == ND_FUNC_DEF && strcmp(f->name, n->name) == 0) { is_local = 1; break; }
        }
        if (!is_local && !is_builtin_name(n->name) && !already_extern(n->name) && g_n_externs < MAX_EXTERNS) {
            safe_strcpy(g_externs[g_n_externs++], n->name, MAX_NAME);
        }
    }
    if (n->kind == ND_STR && g_n_strings < MAX_STRINGS) {
        g_strings[g_n_strings++] = n;
    }
    /* static locals live inside a function body's statement list, not
     * prog's top-level chain - the .data/.bss emission below only scans
     * top-level decls, so without this they'd be referenced (lea eax,
     * [__static_N]) but never actually allocated storage. */
    if (n->kind == ND_VAR_DECL && n->is_static_local && g_n_statics < MAX_STATICS) {
        g_statics[g_n_statics++] = n;
    }
    walk_collect(n->a, prog);
    walk_collect(n->b, prog);
    walk_collect(n->c, prog);
    walk_collect(n->d, prog);
    walk_collect(n->list, prog);
    walk_collect(n->init, prog);
    walk_collect(n->next, prog);
}

void codegen_emit(node_t *prog, void *out_file, cc_target_t target) {
    g_out = (FILE *)out_file;
    g_target = target;
    g_label_id = 0;
    g_loop = NULL;
    g_n_externs = 0;
    g_n_strings = 0;
    g_n_statics = 0;

    out("BITS 32\n");

    for (node_t *n = prog; n; n = n->next) {
        /* walk_collect recurses into ->next (needed for sibling statement
         * chains reached via ->list/->init) - passing a top-level node
         * straight through would also walk the rest of the top-level
         * chain, which this loop is already advancing over itself,
         * causing every node after the first to be collected once per
         * remaining prefix (quadratic duplication of externs/strings).
         * Cut the top-level ->next link before recursing into this one
         * node's own subtree, then restore it. */
        node_t *saved_next = n->next;
        n->next = NULL;
        if (n->kind != ND_FUNC_DEF) walk_collect(n, prog); /* globals: just for string literals in initializers */
        else walk_collect(n->b, prog);                       /* function bodies */
        n->next = saved_next;
    }
    for (int i = 0; i < g_n_externs; i++) out("extern %s\n", g_externs[i]);

    out("section .text\n");
    for (node_t *n = prog; n; n = n->next) {
        if (n->kind == ND_FUNC_DEF) codegen_func(n);
    }
    int has_main = 0;
    for (node_t *n = prog; n; n = n->next) {
        if (n->kind == ND_FUNC_DEF && strcmp(n->name, "main") == 0) { has_main = 1; break; }
    }
    if (target == TARGET_HOST && has_main) {
        /* Only the one compilation unit that actually defines main should
         * provide _start - host programs are typically built from several
         * --target=host files (e.g. mkfs_tinyfs.c + tinyfs.c + string.c),
         * and emitting a `call main` in every one of them would leave an
         * unresolvable reference in every file that doesn't itself define
         * main (there's no separate crt0.o in this toolchain - see
         * docs/toolchain.md). Still inside `section .text` - must stay
         * there, not after the .rodata switch below. */
        out("global _start\n_start:\n");
        /* At kernel entry, [esp]=argc and [esp+4] is the start of the
         * argv[] pointer array - i.e. "argv" (the char** value) is the
         * address (esp+4), not a value stored on the stack. A plain
         * `call main` would push a return address first, shifting argc
         * into place by coincidence but leaving the second cdecl slot
         * holding argv[0]'s string pointer instead of &argv[0]. Load
         * both explicitly before the call. */
        out("    mov eax, [esp]\n");
        out("    lea ecx, [esp+4]\n");
        out("    push ecx\n");
        out("    push eax\n");
        out("    call main\n");
        out("    mov ebx, eax\n    mov eax, 1\n    int 0x80\n");
    }

    if (g_n_strings) {
        out("section .rodata\n");
        for (int i = 0; i < g_n_strings; i++) {
            node_t *s = g_strings[i];
            out("global %s\n%s:\n", s->asm_label, s->asm_label);
            for (const char *p = s->str; ; p++) {
                out("    db %d\n", (int)(unsigned char)*p);
                if (!*p) break;
            }
        }
    }

    int any_data = 0, any_bss = 0;
    for (node_t *n = prog; n; n = n->next) {
        if (n->kind == ND_VAR_DECL && n->is_global) {
            if (n->init) any_data = 1; else any_bss = 1;
        }
    }
    for (int i = 0; i < g_n_statics; i++) {
        if (g_statics[i]->init) any_data = 1; else any_bss = 1;
    }

    if (any_data) {
        out("section .data\n");
        for (node_t *n = prog; n; n = n->next) {
            if (n->kind == ND_VAR_DECL && n->is_global && n->init) {
                out("global %s\n%s:\n", n->asm_label, n->asm_label);
                emit_data_bytes(n->init, n->type);
            }
        }
        for (int i = 0; i < g_n_statics; i++) {
            node_t *n = g_statics[i];
            if (n->init) {
                out("global %s\n%s:\n", n->asm_label, n->asm_label);
                emit_data_bytes(n->init, n->type);
            }
        }
    }
    if (any_bss) {
        out("section .bss\n");
        for (node_t *n = prog; n; n = n->next) {
            if (n->kind == ND_VAR_DECL && n->is_global && !n->init) {
                out("global %s\n%s:\n    resb %d\n", n->asm_label, n->asm_label, type_size(n->type));
            }
        }
        for (int i = 0; i < g_n_statics; i++) {
            node_t *n = g_statics[i];
            if (!n->init) {
                out("global %s\n%s:\n    resb %d\n", n->asm_label, n->asm_label, type_size(n->type));
            }
        }
    }
}
