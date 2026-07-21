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
static void codegen_init_local(int base_offset, type_t *t, node_t *init);
static void codegen_call_or_builtin(node_t *n);

/* ---------------- loads/stores sized per C type ---------------- */

static int is_byte_type(type_t *t) { return t->kind == TY_CHAR; }
static int is_short_type(type_t *t) { return t->kind == TY_SHORT; }

/* Address already in EAX; loads the value (sized per `t`) into EAX. */
static void load_value(type_t *t) {
    if (t->kind == TY_ARRAY) return; /* arrays decay to their own address - nothing to load */
    if (t->kind == TY_STRUCT || t->kind == TY_UNION) return; /* same - see store_value's struct-copy branch */
    if (is_byte_type(t)) {
        out("    movzx eax, byte [eax]\n");
    } else if (is_short_type(t)) {
        /* mov ax, [eax] reads through the same register that holds the
         * address (ax is eax's low 16 bits) - fine, the CPU reads the
         * memory operand before writing the result back. movzx then
         * clears the high 16 bits, which the mov left as whatever
         * garbage eax's upper half held before. */
        out("    mov ax, [eax]\n    movzx eax, ax\n");
    } else {
        out("    mov eax, [eax]\n");
    }
}

/* Byte-by-byte copy, EAX=src address, ECX=dst address (the same
 * convention store_value's other branches use for "value" and
 * "destination"). Needed because a struct/union doesn't fit in a
 * register - whole-struct assignment (`a = b;`, or a non-brace
 * initializer like `parser_t save = *p;`) needs an actual memcpy, not a
 * single mov. AS has no rep movsb, so this is a plain compare-and-loop
 * like every other loop this codegen emits. */
static void codegen_struct_copy(type_t *t) {
    char ltop[MAX_NAME], lend[MAX_NAME];
    new_label(ltop); new_label(lend);
    out("    push eax\n");    /* [esp+4] = src */
    out("    push ecx\n");      /* [esp] = dst */
    out("    mov edx, 0\n");      /* edx = i - a register, not a stack slot,
                                    * so every compare/add below already has
                                    * a register operand and never needs an
                                    * explicit dword-size hint on bare
                                    * memory + immediate. */
    out("%s:\n", ltop);
    out("    cmp edx, %d\n", type_size(t));
    out("    jge %s\n", lend);
    out("    mov eax, [esp+4]\n");   /* src */
    out("    add eax, edx\n");         /* + i */
    out("    movzx eax, byte [eax]\n");  /* eax = src[i] */
    out("    mov ecx, [esp]\n");           /* dst */
    out("    add ecx, edx\n");               /* + i */
    out("    mov [ecx], al\n");                /* dst[i] = src[i] */
    out("    add edx, 1\n");
    out("    jmp %s\n", ltop);
    out("%s:\n", lend);
    out("    add esp, 8\n");
}

/* Stores EAX's value into the address in ECX. */
static void store_value(type_t *t) {
    if (t->kind == TY_STRUCT || t->kind == TY_UNION) {
        codegen_struct_copy(t);
    } else if (is_byte_type(t)) {
        out("    mov [ecx], al\n");
    } else if (is_short_type(t)) {
        out("    mov [ecx], ax\n");
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
        case ND_CALL:
            /* A struct/union-returning call used directly as an lvalue
             * (e.g. as/parser.c's `line_peek(ln).kind` - a very common
             * idiom, needed for Phase T8's self-hosting - see
             * docs/toolchain.md). codegen_call_or_builtin already leaves
             * the hidden result slot's address in eax for exactly this
             * case (see its own comment) - the "address of a struct-
             * typed rvalue" convention load_value/codegen_expr already
             * use everywhere else, so this is just an ordinary call. */
            codegen_expr(n);
            return;
        default:
            fprintf(stderr, "cc: internal error: not an lvalue (kind %d)\n", n->kind);
            exit(1);
    }
}

/* ---------------- expressions ---------------- */

static void codegen_binop(node_t *n) {
    int a_is_ptr = n->a->type->kind == TY_PTR || n->a->type->kind == TY_ARRAY;
    int b_is_ptr = n->b->type->kind == TY_PTR || n->b->type->kind == TY_ARRAY;
    codegen_expr(n->b);
    out("    push eax\n");
    codegen_expr(n->a);
    out("    pop ecx\n");
    /* ptr - ptr (e.g. elf_writer.c's `s - ctx->syms`, an array-position
     * computation): the *byte* difference divided by the element size,
     * per make_binop's comment - eax=a, ecx=b already (see the
     * evaluation order above). Must come before the general pointer-
     * arithmetic scaling below: n->type is plain int here (make_binop
     * never labels this result as a pointer), so that code wouldn't
     * fire anyway, but this makes the actual division explicit rather
     * than silently falling through to a plain integer subtraction. */
    if (n->op == TK_MINUS && a_is_ptr && b_is_ptr) {
        int sz = type_size(n->a->type->base);
        out("    sub eax, ecx\n");
        if (sz > 1) out("    cdq\n    mov ecx, %d\n    idiv ecx\n", sz);
        return;
    }
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
        case ND_COMMA:
            /* Evaluate and discard the left operand, then evaluate the
             * right - both land in eax by convention, so evaluating `b`
             * second naturally overwrites `a`'s (unused) result. */
            codegen_expr(n->a);
            codegen_expr(n->b);
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
    int total_bytes = 0;
    for (int i = argc - 1; i >= 0; i--) {
        codegen_expr(args[i]);
        if (args[i]->type->kind == TY_STRUCT || args[i]->type->kind == TY_UNION) {
            /* Struct/union by value: codegen_expr already yields its
             * address (see load_value), not its bytes - copy the whole
             * thing onto the stack rather than pushing a 4-byte pointer,
             * matching parse_func's real-size parameter offsets. */
            int sz = type_size(args[i]->type);
            sz = (sz + 3) & ~3;
            if (sz < 4) sz = 4;
            out("    mov ecx, eax\n");   /* save src - sub esp doesn't touch ecx */
            out("    sub esp, %d\n", sz);
            out("    mov eax, ecx\n");     /* src */
            out("    mov ecx, esp\n");       /* dst = the space just reserved */
            codegen_struct_copy(args[i]->type);
            total_bytes += sz;
        } else {
            out("    push eax\n");
            total_bytes += 4;
        }
    }
    /* Struct/union return: pass the hidden temp slot's address (see
     * parse_primary's ND_CALL construction) as an implicit extra
     * argument, pushed last so it lands first from the callee's side
     * ([ebp+8] - matches parse_func's `returns_struct` offset shift). */
    int is_struct_return = n->type->kind == TY_STRUCT || n->type->kind == TY_UNION;
    if (is_struct_return) {
        out("    lea eax, [ebp%+lld]\n", (long long)n->num);
        out("    push eax\n");
        total_bytes += 4;
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
    if (total_bytes) out("    add esp, %d\n", total_bytes);
    /* eax already holds the hidden slot's address on return (ND_RETURN
     * leaves it there) - exactly the "address of a struct-typed rvalue"
     * convention load_value/codegen_expr already use elsewhere, so
     * nothing further to do here for is_struct_return. */
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
            /* codegen_init_local handles both brace-init lists (e.g.
             * eval.c's `exec_result_t r = { EXEC_NORMAL, VAL_ZERO };`)
             * and plain scalar initializers - a bare `int x = 1;` is
             * just its one-element base case. */
            if (n->init) codegen_init_local(n->frame_offset, n->type, n->init);
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
            if (n->a && (n->a->type->kind == TY_STRUCT || n->a->type->kind == TY_UNION)) {
                /* Hidden-pointer struct return (see parse_func /
                 * codegen_call_or_builtin): copy the result into the
                 * caller-supplied slot at [ebp+8], then hand that same
                 * address back in eax - the call site relies on it
                 * being there, same convention as any other
                 * struct-typed rvalue address. */
                codegen_expr(n->a);          /* eax = src address */
                out("    mov ecx, [ebp+8]\n");   /* ecx = dst address */
                codegen_struct_copy(n->a->type);
                out("    mov eax, [ebp+8]\n");
            } else if (n->a) {
                codegen_expr(n->a);
            }
            out("    mov esp, ebp\n    pop ebp\n    ret\n");
            return;
        case ND_BREAK:
            out("    jmp %s\n", g_loop->break_label);
            return;
        case ND_CONTINUE:
            out("    jmp %s\n", g_loop->continue_label);
            return;
        case ND_SWITCH: {
            /* n->b is an ND_BLOCK whose ->list is a flat chain of
             * case/default markers interleaved with ordinary statements -
             * parsed via the regular parse_block, since `case`/`default`
             * are just zero-width label statements (see parser.c). Walking
             * that chain linearly and falling through unless a `break`
             * jumps out reproduces C's fallthrough semantics for free. */
            char lend[MAX_NAME];
            new_label(lend);
            codegen_expr(n->a);
            out("    mov [ebp%+d], eax\n", n->frame_offset);

            char default_label[MAX_NAME] = "";
            for (node_t *s = n->b->list; s; s = s->next) {
                if (s->kind == ND_CASE || s->kind == ND_DEFAULT) new_label(s->asm_label);
                if (s->kind == ND_DEFAULT) safe_strcpy(default_label, s->asm_label, MAX_NAME);
            }
            for (node_t *s = n->b->list; s; s = s->next) {
                if (s->kind != ND_CASE) continue;
                out("    mov eax, [ebp%+d]\n", n->frame_offset);
                out("    cmp eax, %lld\n", (long long)s->num);
                out("    je %s\n", s->asm_label);
            }
            out("    jmp %s\n", default_label[0] ? default_label : lend);

            loop_ctx_t lc = { "", "", g_loop };
            safe_strcpy(lc.break_label, lend, MAX_NAME);
            /* `continue` inside a switch targets the nearest *enclosing
             * loop*, not the switch itself - inherit the parent frame's
             * continue_label rather than giving this frame its own. */
            safe_strcpy(lc.continue_label, g_loop ? g_loop->continue_label : "", MAX_NAME);
            g_loop = &lc;
            for (node_t *s = n->b->list; s; s = s->next) {
                if (s->kind == ND_CASE || s->kind == ND_DEFAULT) out("%s:\n", s->asm_label);
                else codegen_stmt(s);
            }
            g_loop = lc.parent;
            out("%s:\n", lend);
            return;
        }
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
    /* A string-literal initializer for a pointer-typed field (e.g.
     * toolchain/as/parser.c's `static const regdef_t REGS[] =
     * {{"al",R_AL,1}, ...}` - Phase T8's self-hosting build is what
     * first exercised a string literal inside a struct-array global
     * initializer) needs the string's own label, not v->num - an ND_STR
     * node never sets ->num at all, so this silently emitted `dd 0`
     * (a NULL pointer) instead of a relocation to the string's bytes. */
    if (v->kind == ND_STR) { out("    dd %s\n", v->asm_label); return; }
    if (t->kind == TY_CHAR) out("    db %lld\n", (long long)v->num);
    else out("    dd %lld\n", (long long)v->num);
}

static void emit_data_bytes(node_t *init, type_t *t) {
    if (!init) {
        out("    resb %d\n", type_size(t));
        return;
    }
    /* `char x[N] = "literal"` (no braces - distinct from `char x[] =
     * {'a','b',0}`): real C semantics copy the string's own bytes
     * (including its NUL) into x's storage, zero-padding any remaining
     * space - not store a pointer, which is what falling through to
     * emit_scalar_bytes's ND_STR case (a few lines down, meant for
     * pointer-typed fields like a struct's `const char *name`) would do
     * instead. First hit by AS's own `char default_label[MAX_NAME] =
     * "";` (Phase T8's self-hosting build). */
    if (t->kind == TY_ARRAY && init->kind == ND_STR) {
        int n = (int)strlen(init->str) + 1;
        for (int i = 0; i < n; i++) out("    db %d\n", (unsigned char)init->str[i]);
        int pad = type_size(t) - n;
        if (pad > 0) out("    resb %d\n", pad);
        return;
    }
    if (t->kind == TY_ARRAY && init->kind == ND_BLOCK) {
        int count = 0;
        for (node_t *v = init->list; v; v = v->next) { emit_data_bytes(v, t->base); count++; }
        int pad = t->array_len - count;
        for (int i = 0; i < pad; i++) out("    resb %d\n", type_size(t->base));
        return;
    }
    if ((t->kind == TY_STRUCT || t->kind == TY_UNION) && init->kind == ND_BLOCK) {
        /* Positional: walk declared members alongside the initializer
         * list (no designated-initializer support - see
         * docs/toolchain.md). A union brace-init like value_t's
         * `{ VAL_INT, { 0 } }` naturally works here too: its one element
         * targets the first member at offset 0, and the trailing-padding
         * check below fills out the rest of the union's size. */
        member_t *m = t->members;
        node_t *v = init->list;
        int cur_off = 0;
        for (; m && v; m = m->next, v = v->next) {
            if (m->offset > cur_off) out("    resb %d\n", m->offset - cur_off);
            emit_data_bytes(v, m->type);
            cur_off = m->offset + type_size(m->type);
        }
        int total = type_size(t);
        if (total > cur_off) out("    resb %d\n", total - cur_off);
        return;
    }
    emit_scalar_bytes(init, t);
}

/* Runtime equivalent of emit_data_bytes, for a local (stack) variable's
 * brace-init - e.g. `exec_result_t r = { EXEC_NORMAL, VAL_ZERO };` (used
 * in kernel/lang/script/eval.c). Recurses the same way, but instead of
 * writing bytes it emits store instructions relative to a base
 * ebp-offset; a plain scalar just evaluates and stores at that offset,
 * which also serves as the non-aggregate base case (a bare `int x = 1;`
 * behaves like a one-element "aggregate" of itself). */
static void codegen_zero_range(int base_offset, int size) {
    char ltop[MAX_NAME], lend[MAX_NAME];
    new_label(ltop); new_label(lend);
    out("    push ecx\n");
    out("    mov ecx, 0\n");
    out("%s:\n", ltop);
    out("    cmp ecx, %d\n", size);
    out("    jge %s\n", lend);
    out("    lea eax, [ebp%+d]\n", base_offset);
    out("    add eax, ecx\n");
    out("    mov byte [eax], 0\n");
    out("    add ecx, 1\n");
    out("    jmp %s\n", ltop);
    out("%s:\n", lend);
    out("    pop ecx\n");
}

static void codegen_init_local(int base_offset, type_t *t, node_t *init) {
    /* A short brace-init list (fewer values than the aggregate has
     * elements/members - e.g. `line_t ln = {0};` for a struct with three
     * members, one of them a whole array) leaves every uncovered byte at
     * whatever garbage was already on the stack, not zero - the same gap
     * emit_data_bytes already closes for *global* aggregates via a
     * trailing `resb`, but nothing did for a local one until now (see
     * docs/toolchain.md, Phase T8: this exact gap, on this exact
     * pattern, sent as/parser.c's own line-tokenizing loop into an
     * infinite loop reading uninitialized bounds/EOF-check fields). Zero
     * the whole range up front (redundant on nested recursive calls, but
     * harmless and simplest) so every element/member not explicitly
     * listed below is correctly zero rather than garbage. */
    if ((t->kind == TY_ARRAY || t->kind == TY_STRUCT || t->kind == TY_UNION) && init->kind == ND_BLOCK) {
        codegen_zero_range(base_offset, type_size(t));
    }
    /* `char x[N] = "literal"` (no braces): same real-C-semantics gap as
     * emit_data_bytes's own ND_STR branch (see its comment) - falling
     * through to the plain-scalar case below would treat this as a
     * pointer store (the string's *address*, via codegen_expr's ND_STR
     * case) instead of copying the string's bytes into x's own stack
     * storage. First hit by codegen.c's own `char default_label[MAX_NAME]
     * = "";` inside its ND_SWITCH case (Phase T8's self-hosting build):
     * only the first 4 bytes of default_label ever got written (with an
     * unrelated pointer value, not zero), so `default_label[0]` read as
     * non-zero garbage and `jmp %s` emitted a jump to a garbage label
     * name instead of ever falling back to `lend`. */
    if (t->kind == TY_ARRAY && init->kind == ND_STR) {
        codegen_zero_range(base_offset, type_size(t));
        int n = (int)strlen(init->str) + 1;
        for (int i = 0; i < n; i++) {
            out("    lea eax, [ebp%+d]\n", base_offset + i);
            out("    mov byte [eax], %d\n", (unsigned char)init->str[i]);
        }
        return;
    }
    if (t->kind == TY_ARRAY && init->kind == ND_BLOCK) {
        int off = 0;
        for (node_t *v = init->list; v; v = v->next) {
            codegen_init_local(base_offset + off, t->base, v);
            off += type_size(t->base);
        }
        return;
    }
    if ((t->kind == TY_STRUCT || t->kind == TY_UNION) && init->kind == ND_BLOCK) {
        member_t *m = t->members;
        node_t *v = init->list;
        for (; m && v; m = m->next, v = v->next) {
            codegen_init_local(base_offset + m->offset, m->type, v);
        }
        return;
    }
    out("    lea eax, [ebp%+d]\n", base_offset);
    out("    push eax\n");
    codegen_expr(init);
    out("    pop ecx\n");
    store_value(t);
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
/* skip_next=1 for the one call the top-level driver makes per top-level
 * node (see codegen_emit) - that loop already advances over prog's own
 * ->next chain itself, and recursing into ->next there too would
 * re-walk (and re-collect) the whole remaining top-level chain on every
 * iteration. An earlier version of this fix instead temporarily NULLed
 * n->next around that call, which corrupted prog's chain *while it was
 * still being scanned* by the is_local check below whenever that scan
 * reached the currently-truncated node - any forward-referenced same-
 * file function (called from a function defined earlier in the file)
 * would then look "not local" and get a spurious `extern`, colliding at
 * link time with the real definition once another file defined a same-
 * named local of its own. skip_next avoids the mutation entirely: below
 * ->next is walked normally for every *nested* recursion (statement
 * sibling chains reached via ->list/->init still need it), just not for
 * this one top-level entry point. */
static void walk_collect_ex(node_t *n, node_t *prog, int skip_next) {
    if (!n) return;
    /* is_indirect_call: the callee is a variable (see cc.h/parser.c),
     * not a function symbol at all - e.g. tinyfs.c's `block_read(...)`
     * calls through a function-pointer global. Its linkage is handled
     * entirely by the ordinary global/extern-variable machinery
     * (is_extern_decl below); treating n->name as a possibly-external
     * *function* name here as well double-declared it (`extern
     * block_read` *and* a real local `global block_read` definition in
     * the same file), which left AS unable to tell which one a
     * `lea eax, [block_read]` reference should resolve to. */
    if (n->kind == ND_CALL && !n->is_indirect_call) {
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
    walk_collect_ex(n->a, prog, 0);
    walk_collect_ex(n->b, prog, 0);
    walk_collect_ex(n->c, prog, 0);
    walk_collect_ex(n->d, prog, 0);
    walk_collect_ex(n->list, prog, 0);
    walk_collect_ex(n->init, prog, 0);
    if (!skip_next) walk_collect_ex(n->next, prog, 0);
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
        /* skip_next=1: this loop already advances over prog's top-level
         * chain itself (see walk_collect_ex's comment). */
        if (n->kind != ND_FUNC_DEF) walk_collect_ex(n, prog, 1); /* globals: just for string literals in initializers */
        else walk_collect_ex(n->b, prog, 1);                       /* function bodies */
    }
    /* extern variable declarations (e.g. idt.c's `extern uint32_t
     * isr_stub_table[32];`, defined in idt_stubs.S) need the same
     * `extern NAME` treatment as called-but-not-locally-defined
     * functions above - walk_collect only looks at ND_CALL, never plain
     * variable references, so this is a separate pass. Without it, AS
     * drops any relocation against a name with no local definition and
     * no explicit `extern` (see docs/toolchain.md) - the reference
     * would silently resolve to nothing instead of erroring. */
    for (node_t *n = prog; n; n = n->next) {
        if (n->kind == ND_VAR_DECL && n->is_extern_decl && !already_extern(n->name) && g_n_externs < MAX_EXTERNS) {
            safe_strcpy(g_externs[g_n_externs++], n->name, MAX_NAME);
        }
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
            /* Not `global`: __str_N is numbered per translation unit
             * (parser_t's next_str_id resets for every file CC runs on),
             * only ever referenced from within the same file, and
             * marking it global made every file's __str_0 collide with
             * every other file's at link time. */
            node_t *s = g_strings[i];
            out("%s:\n", s->asm_label);
            for (const char *p = s->str; ; p++) {
                out("    db %d\n", (int)(unsigned char)*p);
                if (!*p) break;
            }
        }
    }

    int any_data = 0, any_bss = 0;
    for (node_t *n = prog; n; n = n->next) {
        /* is_extern_decl: a declaration only (`extern T name;`), not a
         * definition - it must never get its own storage, real or
         * otherwise it silently duplicates whatever file actually
         * defines the symbol (see the extern-collection pass above). */
        if (n->kind == ND_VAR_DECL && n->is_global && !n->is_extern_decl) {
            if (n->init) any_data = 1; else any_bss = 1;
        }
    }
    for (int i = 0; i < g_n_statics; i++) {
        if (g_statics[i]->init) any_data = 1; else any_bss = 1;
    }

    if (any_data) {
        out("section .data\n");
        for (node_t *n = prog; n; n = n->next) {
            if (n->kind == ND_VAR_DECL && n->is_global && !n->is_extern_decl && n->init) {
                /* file-scope `static` keeps internal linkage - no `global`,
                 * same reasoning as function-local statics below (two
                 * files' own file-private `static T name;` must not
                 * collide as duplicate global symbols). */
                if (!n->is_file_static) out("global %s\n", n->asm_label);
                out("%s:\n", n->asm_label);
                emit_data_bytes(n->init, n->type);
            }
        }
        for (int i = 0; i < g_n_statics; i++) {
            /* Not `global`, same reasoning as __str_N above -
             * __static_N is only numbered uniquely within one file. */
            node_t *n = g_statics[i];
            if (n->init) {
                out("%s:\n", n->asm_label);
                emit_data_bytes(n->init, n->type);
            }
        }
    }
    if (any_bss) {
        out("section .bss\n");
        for (node_t *n = prog; n; n = n->next) {
            if (n->kind == ND_VAR_DECL && n->is_global && !n->is_extern_decl && !n->init) {
                if (!n->is_file_static) out("global %s\n", n->asm_label);
                out("%s:\n    resb %d\n", n->asm_label, type_size(n->type));
            }
        }
        for (int i = 0; i < g_n_statics; i++) {
            node_t *n = g_statics[i];
            if (!n->init) {
                out("%s:\n    resb %d\n", n->asm_label, type_size(n->type));
            }
        }
    }
}
