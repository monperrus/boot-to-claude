#include "cc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- symbol tables ---------------- */

typedef struct sym {
    char name[MAX_NAME];
    type_t *type;
    int frame_offset;     /* locals/params, EBP-relative */
    int is_global;
    int is_static_local;
    char asm_label[MAX_NAME];
    int is_typedef;
    int is_enum_const;
    int64_t enum_value;
    struct sym *next;
} sym_t;

typedef struct tag {
    char name[MAX_NAME];
    type_t *type;
    struct tag *next;
} tag_t;

typedef struct {
    token_t *toks;
    int n, pos;

    sym_t *globals;
    sym_t *locals;       /* current function's flat symbol list (params+locals) */
    tag_t *tags;

    int frame_size;        /* current function's local variable area size (grows as locals are declared) */
    int next_static_id;      /* for unique static-local asm labels */
    int next_str_id;           /* for unique string-literal asm labels */

    type_t *cur_func_ret;
    int had_error;
} parser_t;

static void perr_at(parser_t *p, token_t t, const char *msg) {
    fprintf(stderr, "cc: %s:%d: %s (near '%s')\n", t.file ? t.file : "?", t.line, msg,
            t.kind == TK_IDENT || t.kind == TK_STR ? t.text : "?");
    p->had_error = 1;
    exit(1); /* tinyC has no error recovery - not worth it for this codebase's size */
}

static void perr(parser_t *p, const char *msg) {
    perr_at(p, p->toks[p->pos], msg);
}

static token_t cur(parser_t *p) { return p->toks[p->pos]; }
static token_t advance(parser_t *p) { token_t t = p->toks[p->pos]; if (p->toks[p->pos].kind != TK_EOF) p->pos++; return t; }
static int check(parser_t *p, tok_kind_t k) { return cur(p).kind == k; }
static int match(parser_t *p, tok_kind_t k) { if (check(p, k)) { advance(p); return 1; } return 0; }
static token_t expect(parser_t *p, tok_kind_t k) {
    if (!check(p, k)) perr(p, "unexpected token");
    return advance(p);
}
static int is_kw(parser_t *p, const char *kw) {
    return check(p, TK_IDENT) && strcmp(cur(p).text, kw) == 0;
}
static int match_kw(parser_t *p, const char *kw) {
    if (is_kw(p, kw)) { advance(p); return 1; }
    return 0;
}

static sym_t *sym_find_local(parser_t *p, const char *name) {
    for (sym_t *s = p->locals; s; s = s->next) if (strcmp(s->name, name) == 0) return s;
    return NULL;
}
static sym_t *sym_find_global(parser_t *p, const char *name) {
    for (sym_t *s = p->globals; s; s = s->next) if (strcmp(s->name, name) == 0) return s;
    return NULL;
}
static sym_t *sym_find(parser_t *p, const char *name) {
    sym_t *s = sym_find_local(p, name);
    return s ? s : sym_find_global(p, name);
}
static tag_t *tag_find(parser_t *p, const char *name) {
    for (tag_t *t = p->tags; t; t = t->next) if (strcmp(t->name, name) == 0) return t;
    return NULL;
}
static int is_typedef_name(parser_t *p, const char *name) {
    sym_t *s = sym_find_global(p, name); /* typedefs are always file-scope in this codebase */
    return s && s->is_typedef;
}

static sym_t *add_global(parser_t *p, const char *name, type_t *type) {
    sym_t *s = calloc(1, sizeof(sym_t));
    safe_strcpy(s->name, name, MAX_NAME);
    s->type = type;
    s->is_global = 1;
    safe_strcpy(s->asm_label, name, MAX_NAME);
    s->next = p->globals;
    p->globals = s;
    return s;
}

static sym_t *add_local(parser_t *p, const char *name, type_t *type, int is_static) {
    sym_t *s = calloc(1, sizeof(sym_t));
    safe_strcpy(s->name, name, MAX_NAME);
    s->type = type;
    if (is_static) {
        s->is_static_local = 1;
        snprintf(s->asm_label, MAX_NAME, "__static_%d", p->next_static_id++);
    } else {
        int sz = type_size(type);
        p->frame_size += sz < 4 ? 4 : sz; /* min 4-byte slots - simplicity over density */
        p->frame_size = (p->frame_size + 3) & ~3;
        s->frame_offset = -p->frame_size;
    }
    s->next = p->locals;
    p->locals = s;
    return s;
}

/* ---------------- expressions (forward decls) ---------------- */

static node_t *parse_expr(parser_t *p);
static node_t *parse_assign(parser_t *p);
static node_t *parse_cast(parser_t *p);
static node_t *parse_unary(parser_t *p);
static type_t *parse_type_name(parser_t *p); /* for casts/sizeof(type) */
static node_t *parse_stmt(parser_t *p);
static node_t *parse_block(parser_t *p);
static node_t *make_binop(tok_kind_t op, node_t *a, node_t *b); /* for va_start/va_arg desugaring */
static type_t *parse_array_dims(parser_t *p, type_t *base); /* for multi-dim array param decay */

/* Array bounds and enum values in this codebase are often macro
 * constants that expand to a small arithmetic expression (e.g.
 * `TINYFS_INODE_BLOCK` -> `(1 + 1)`), not always a single literal
 * token, so bounds need real constant folding rather than reading one
 * token's ->num. */
static int64_t eval_const_node(parser_t *p, node_t *n) {
    switch (n->kind) {
        case ND_NUM: return n->num;
        case ND_UNARY_MINUS: return -eval_const_node(p, n->a);
        case ND_BITNOT: return ~eval_const_node(p, n->a);
        case ND_NOT: return !eval_const_node(p, n->a);
        case ND_BINOP: {
            int64_t l = eval_const_node(p, n->a), r = eval_const_node(p, n->b);
            switch (n->op) {
                case TK_PLUS: return l + r;
                case TK_MINUS: return l - r;
                case TK_STAR: return l * r;
                case TK_SLASH: return r != 0 ? l / r : 0;
                case TK_PERCENT: return r != 0 ? l % r : 0;
                case TK_SHL: return l << r;
                case TK_SHR: return l >> r;
                case TK_AMP: return l & r;
                case TK_PIPE: return l | r;
                case TK_CARET: return l ^ r;
                default: perr(p, "unsupported operator in constant expression"); return 0;
            }
        }
        default:
            perr(p, "expected a constant expression");
            return 0;
    }
}
static int64_t parse_const_expr(parser_t *p) {
    return eval_const_node(p, parse_assign(p));
}

/* ---------------- type specifiers & declarators ---------------- */

static member_t *parse_struct_members(parser_t *p);

static int is_type_start(parser_t *p) {
    if (!check(p, TK_IDENT)) return 0;
    const char *kws[] = { "void", "char", "short", "int", "long", "signed", "unsigned",
                           "struct", "union", "enum", "const", "volatile", "static", "extern", NULL };
    for (int i = 0; kws[i]; i++) if (strcmp(cur(p).text, kws[i]) == 0) return 1;
    return is_typedef_name(p, cur(p).text);
}

/* Parses base type specifiers + storage class; returns the type and sets
 * is_static and is_extern. Does not consume any declarator. */
static type_t *parse_type_spec(parser_t *p, int *is_static, int *is_extern) {
    int saw_void = 0, saw_char = 0, saw_short = 0, saw_int = 0, saw_long = 0, saw_unsigned = 0, saw_signed = 0;
    type_t *named = NULL;
    if (is_static) *is_static = 0;
    if (is_extern) *is_extern = 0;

    for (;;) {
        if (match_kw(p, "static")) { if (is_static) *is_static = 1; continue; }
        if (match_kw(p, "extern")) { if (is_extern) *is_extern = 1; continue; }
        if (match_kw(p, "const") || match_kw(p, "volatile") || match_kw(p, "inline")) continue;
        if (match_kw(p, "void")) { saw_void = 1; continue; }
        if (match_kw(p, "char")) { saw_char = 1; continue; }
        if (match_kw(p, "short")) { saw_short = 1; continue; }
        if (match_kw(p, "int")) { saw_int = 1; continue; }
        /* long == int on this 32-bit target (both 4 bytes) - no `long
         * long`/`unsigned long` anywhere in the codebase (see
         * docs/toolchain.md), so a same-size alias is enough. */
        if (match_kw(p, "long")) { saw_long = 1; continue; }
        if (match_kw(p, "signed")) { saw_signed = 1; continue; }
        if (match_kw(p, "unsigned")) { saw_unsigned = 1; continue; }
        if (is_kw(p, "struct") || is_kw(p, "union")) {
            int is_union = is_kw(p, "union");
            advance(p);
            char tagname[MAX_NAME] = {0};
            if (check(p, TK_IDENT)) { safe_strcpy(tagname, cur(p).text, MAX_NAME); advance(p); }
            type_t *st;
            tag_t *existing = tagname[0] ? tag_find(p, tagname) : NULL;
            if (check(p, TK_LBRACE)) {
                advance(p);
                /* Reuse the existing (possibly still-incomplete, from an
                 * earlier forward declaration) type_t instance rather than
                 * allocating a new one, so any typedef/pointer that
                 * already captured its address sees the real members once
                 * this definition completes it - see the forward-decl
                 * branch below. */
                st = existing ? existing->type : type_new(is_union ? TY_UNION : TY_STRUCT);
                safe_strcpy(st->tag, tagname, MAX_NAME);
                /* Register the tag *before* parsing the body, not after -
                 * a self-referential member (`struct node *next;` inside
                 * `struct node { ... }`) needs to find its own tag while
                 * still inside its own member list. */
                if (tagname[0] && !existing) {
                    tag_t *tg = calloc(1, sizeof(tag_t));
                    safe_strcpy(tg->name, tagname, MAX_NAME);
                    tg->type = st;
                    tg->next = p->tags;
                    p->tags = tg;
                }
                st->members = parse_struct_members(p);
                expect(p, TK_RBRACE);
                if (is_kw(p, "__attribute__")) {
                    advance(p); expect(p, TK_LPAREN); expect(p, TK_LPAREN);
                    if (is_kw(p, "packed")) { advance(p); st->is_packed = 1; }
                    expect(p, TK_RPAREN); expect(p, TK_RPAREN);
                }
                /* Compute member offsets now, not lazily on first
                 * type_size() call - find_member() (used for `.`/`->`
                 * anywhere later in this file) needs real offsets
                 * immediately, and nothing else is guaranteed to trigger
                 * layout before then. */
                type_size(st);
            } else if (existing) {
                st = existing->type; /* complete or still-incomplete - fine as long as only used via pointer */
            } else if (tagname[0]) {
                /* Forward declaration (`struct Foo;` or a typedef like
                 * `typedef struct ast_node ast_node_t;` before the real
                 * definition) - register an incomplete tag now so later
                 * pointer-only uses (and the eventual real definition,
                 * via the `existing` branch above) resolve to the same
                 * type_t instance. Only valid as `struct Foo *` until
                 * completed; type_size on a bare incomplete struct/union
                 * would still be meaningless, same as real C. */
                st = type_new(is_union ? TY_UNION : TY_STRUCT);
                safe_strcpy(st->tag, tagname, MAX_NAME);
                tag_t *tg = calloc(1, sizeof(tag_t));
                safe_strcpy(tg->name, tagname, MAX_NAME);
                tg->type = st;
                tg->next = p->tags;
                p->tags = tg;
            } else {
                perr(p, "anonymous struct/union with no body");
                return NULL;
            }
            named = st;
            continue;
        }
        if (is_kw(p, "enum")) {
            advance(p);
            char tagname[MAX_NAME] = {0};
            if (check(p, TK_IDENT)) { safe_strcpy(tagname, cur(p).text, MAX_NAME); advance(p); }
            type_t *et;
            if (check(p, TK_LBRACE)) {
                advance(p);
                et = type_new(TY_ENUM);
                safe_strcpy(et->tag, tagname, MAX_NAME);
                int64_t val = 0;
                while (!check(p, TK_RBRACE)) {
                    token_t nm = expect(p, TK_IDENT);
                    if (match(p, TK_ASSIGN)) val = parse_const_expr(p);
                    sym_t *s = calloc(1, sizeof(sym_t));
                    safe_strcpy(s->name, nm.text, MAX_NAME);
                    s->is_enum_const = 1;
                    s->enum_value = val;
                    s->type = et;
                    s->next = p->globals;
                    p->globals = s;
                    val++;
                    if (!match(p, TK_COMMA)) break;
                }
                expect(p, TK_RBRACE);
                if (tagname[0]) {
                    tag_t *tg = calloc(1, sizeof(tag_t));
                    safe_strcpy(tg->name, tagname, MAX_NAME);
                    tg->type = et;
                    tg->next = p->tags;
                    p->tags = tg;
                }
            } else {
                tag_t *existing = tag_find(p, tagname);
                if (!existing) { perr(p, "unknown enum tag"); return NULL; }
                et = existing->type;
            }
            named = et;
            continue;
        }
        if (check(p, TK_IDENT) && is_typedef_name(p, cur(p).text) && !named && !saw_void && !saw_char && !saw_short && !saw_int && !saw_long) {
            sym_t *s = sym_find_global(p, cur(p).text);
            named = s->type;
            advance(p);
            continue;
        }
        break;
    }

    if (named) return named;
    if (saw_void) return type_new(TY_VOID);
    type_t *t;
    if (saw_char) t = type_new(TY_CHAR);
    else if (saw_short) t = type_new(TY_SHORT);
    else t = type_new(TY_INT); /* default-int, also covers bare signed/unsigned */
    t->is_unsigned = saw_unsigned && !saw_signed ? 1 : (saw_unsigned ? 1 : 0);
    return t;
}

static member_t *parse_struct_members(parser_t *p) {
    member_t *head = NULL, *tail = NULL;
    while (!check(p, TK_RBRACE)) {
        type_t *base = parse_type_spec(p, NULL, NULL);
        for (;;) {
            type_t *mt = base;
            char name[MAX_NAME];
            while (match(p, TK_STAR)) mt = ptr_to(mt);
            token_t id = expect(p, TK_IDENT);
            safe_strcpy(name, id.text, MAX_NAME);
            /* Same bug as parse_declarator originally had (see
             * parse_array_dims's own comment, Phase T8): wrapping `mt`
             * in a new array type on *every* bracket group nests them
             * in the wrong order for 2+ dimensions - `T x[8][65536]`
             * needs the *first* bracket as the outermost dimension, not
             * the last. as.h's own `uint8_t sec_bytes[MAX_SECTIONS]
             * [65536]` is exactly this: the inverted nesting still adds
             * up to the same *total* size (multiplication commutes),
             * so it silently miscompiled only the *indexing* -
             * `sec_bytes[SEC_DATA]` landed 8 bytes into the buffer
             * instead of 65536, inside SEC_TEXT's own byte range. */
            if (check(p, TK_LBRACKET)) mt = parse_array_dims(p, mt);
            member_t *m = calloc(1, sizeof(member_t));
            safe_strcpy(m->name, name, MAX_NAME);
            m->type = mt;
            if (tail) tail->next = m; else head = m;
            tail = m;
            if (!match(p, TK_COMMA)) break;
        }
        expect(p, TK_SEMI);
    }
    return head;
}

static type_t *parse_param_list(parser_t *p, char names[][MAX_NAME], int *n_names) {
    type_t *head = NULL, *tail = NULL;
    int n = 0;
    if (check(p, TK_RPAREN)) return NULL;
    if (is_kw(p, "void") && p->toks[p->pos + 1].kind == TK_RPAREN) { advance(p); return NULL; }
    for (;;) {
        /* `...` (variadic tail, e.g. `fprintf`'s or codegen.c's own
         * `out(const char *fmt, ...)`): stops named-parameter parsing
         * here with no further storage of its own - call sites don't
         * check argument counts against a prototype (each call arg is
         * parsed/pushed independently, see codegen_call_or_builtin), so
         * nothing downstream needs to know this function is variadic
         * beyond "stop expecting more named params". */
        if (check(p, TK_ELLIPSIS)) { advance(p); break; }
        type_t *base = parse_type_spec(p, NULL, NULL);
        type_t *pt = base;
        char nm[MAX_NAME] = {0};
        while (match(p, TK_STAR)) pt = ptr_to(pt);
        if (check(p, TK_LPAREN) && p->toks[p->pos + 1].kind == TK_STAR) {
            /* function-pointer parameter: TYPE (*name)(PARAMS), e.g.
             * parse_binop_level's `ast_node_t *(*next)(parser_t *)` -
             * mirrors parse_declarator's identical form, duplicated here
             * (rather than delegating to it) because plain parameters in
             * this loop tolerate an unnamed parameter and parse_declarator
             * doesn't. */
            advance(p);
            expect(p, TK_STAR);
            if (check(p, TK_IDENT)) { safe_strcpy(nm, cur(p).text, MAX_NAME); advance(p); }
            expect(p, TK_RPAREN);
            expect(p, TK_LPAREN);
            type_t *functy = type_new(TY_FUNC);
            functy->base = pt;
            functy->param = parse_param_list(p, NULL, NULL);
            expect(p, TK_RPAREN);
            pt = ptr_to(functy);
        } else {
            if (check(p, TK_IDENT)) { safe_strcpy(nm, cur(p).text, MAX_NAME); advance(p); }
            if (check(p, TK_LBRACKET)) {
                /* Only the *outermost* dimension decays to a pointer -
                 * `T x[][M]` as a parameter is `T (*x)[M]`, not `T**`.
                 * (Real bug: parse_param_list's own declaration, right
                 * here in this file, is `char names[][MAX_NAME]` - Phase
                 * T8's self-hosting build is what first parsed this
                 * function's signature through CC instead of gcc, and
                 * the old always-ptr_to-every-bracket version silently
                 * mis-typed its own `names` parameter as char**, making
                 * every `names[n]` inside this same function index by
                 * 4-byte pointer strides instead of MAX_NAME-byte array
                 * strides - see docs/toolchain.md.) */
                advance(p); /* consume the outer '[' */
                if (!check(p, TK_RBRACKET)) advance(p); /* its size, if any - irrelevant for a param */
                expect(p, TK_RBRACKET);
                pt = parse_array_dims(p, pt); /* any further [N] groups stay real array dims */
                pt = ptr_to(pt);
            }
        }
        type_t *node = type_new(pt->kind);
        *node = *pt;
        node->next = NULL;
        if (tail) tail->next = node; else head = node;
        tail = node;
        if (names && n_names) safe_strcpy(names[n], nm, MAX_NAME);
        n++;
        if (!match(p, TK_COMMA)) break;
    }
    if (n_names) *n_names = n;
    return head;
}

/* Parses zero or more trailing `[N]` groups (e.g. `rels[MAX_SECTIONS]
 * [MAX_RELOCS]`, needed for Phase T8's self-hosting - see
 * docs/toolchain.md) and nests them the way C actually does: the
 * *first* bracket is the outermost array, so it must wrap a type built
 * from the *later* brackets inward, not the other way around - collect
 * the sizes first, then build from the last one back to the first. */
static type_t *parse_array_dims(parser_t *p, type_t *base) {
    int dims[8];
    int ndims = 0;
    while (match(p, TK_LBRACKET)) {
        int n = -1;
        if (!check(p, TK_RBRACKET)) n = (int)parse_const_expr(p);
        expect(p, TK_RBRACKET);
        dims[ndims++] = n;
    }
    type_t *t = base;
    for (int i = ndims - 1; i >= 0; i--) {
        type_t *arr = type_new(TY_ARRAY);
        arr->base = t;
        arr->array_len = dims[i];
        t = arr;
    }
    return t;
}

/* Parses a declarator (the part after the base type specifier) and
 * returns the full type, writing the declared name into name_out.
 * Scoped to what this codebase's declarators actually need (see
 * docs/toolchain.md): `*`-prefix pointers, `name`, `name[N]`/`name[]`,
 * `name(params)` (function), and the one parenthesized case,
 * `(*name)(params)` (function pointer) - not the fully general/
 * arbitrarily-nested C declarator grammar. */
static type_t *parse_declarator(parser_t *p, type_t *base, char *name_out,
                                  char param_names[][MAX_NAME], int *n_params) {
    while (match(p, TK_STAR)) base = ptr_to(base);

    if (match(p, TK_LPAREN)) {
        expect(p, TK_STAR);
        token_t id = expect(p, TK_IDENT);
        safe_strcpy(name_out, id.text, MAX_NAME);
        expect(p, TK_RPAREN);
        expect(p, TK_LPAREN);
        type_t *functy = type_new(TY_FUNC);
        functy->base = base;
        functy->param = parse_param_list(p, param_names, n_params);
        expect(p, TK_RPAREN);
        return ptr_to(functy);
    }

    token_t id = expect(p, TK_IDENT);
    safe_strcpy(name_out, id.text, MAX_NAME);

    if (check(p, TK_LBRACKET)) return parse_array_dims(p, base);
    if (match(p, TK_LPAREN)) {
        type_t *functy = type_new(TY_FUNC);
        functy->base = base;
        functy->param = parse_param_list(p, param_names, n_params);
        expect(p, TK_RPAREN);
        return functy;
    }
    return base;
}

static type_t *parse_type_name(parser_t *p) {
    type_t *base = parse_type_spec(p, NULL, NULL);
    while (match(p, TK_STAR)) base = ptr_to(base);
    if (check(p, TK_LBRACKET)) base = parse_array_dims(p, base);
    return base;
}

/* ---------------- expressions ---------------- */

static type_t *g_int_type;
static type_t *g_char_type;

static type_t *decay(type_t *t) {
    if (t->kind == TY_ARRAY) return ptr_to(t->base);
    return t;
}

static node_t *make_num(int64_t v) {
    node_t *n = node_new(ND_NUM);
    n->num = v;
    n->type = g_int_type;
    return n;
}

static node_t *resolve_ident(parser_t *p, const char *name, token_t at) {
    sym_t *s = sym_find(p, name);
    if (!s) { perr_at(p, at, "undeclared identifier"); return make_num(0); }
    if (s->is_enum_const) return make_num(s->enum_value);
    node_t *n = node_new(ND_VAR);
    safe_strcpy(n->name, name, MAX_NAME);
    n->type = s->type;
    n->is_global = s->is_global;
    n->is_static_local = s->is_static_local;
    n->frame_offset = s->frame_offset;
    safe_strcpy(n->asm_label, s->asm_label, MAX_NAME);
    return n;
}

static node_t *make_str(parser_t *p, const char *text) {
    node_t *n = node_new(ND_STR);
    safe_strcpy(n->str, text, sizeof(n->str));
    snprintf(n->asm_label, MAX_NAME, "__str_%d", p->next_str_id++);
    type_t *arr = type_new(TY_ARRAY);
    arr->base = g_char_type;
    arr->array_len = (int)strlen(text) + 1;
    n->type = arr;
    return n;
}

static member_t *find_member(type_t *st, const char *name) {
    for (member_t *m = st->members; m; m = m->next) if (strcmp(m->name, name) == 0) return m;
    return NULL;
}

static node_t *parse_primary(parser_t *p) {
    if (check(p, TK_NUM)) { node_t *n = make_num(advance(p).num); return n; }
    if (check(p, TK_CHAR)) { node_t *n = make_num(advance(p).num); n->type = g_char_type; return n; }
    if (check(p, TK_STR)) { return make_str(p, advance(p).text); }
    if (check(p, TK_IDENT)) {
        char name[MAX_NAME];
        token_t ident_tok = advance(p);
        safe_strcpy(name, ident_tok.text, MAX_NAME);
        if (check(p, TK_LPAREN)) {
            advance(p);
            node_t *call = node_new(ND_CALL);
            safe_strcpy(call->name, name, MAX_NAME);
            node_t *tail = NULL;
            while (!check(p, TK_RPAREN)) {
                node_t *arg = parse_assign(p);
                if (!call->list) call->list = arg; else tail->next = arg;
                tail = arg;
                if (!match(p, TK_COMMA)) break;
            }
            expect(p, TK_RPAREN);
            sym_t *fs = sym_find_global(p, name);
            call->type = (fs && fs->type->kind == TY_FUNC) ? fs->type->base : g_int_type;
            if (!(fs && fs->type->kind == TY_FUNC)) {
                /* Not a directly-defined function - if it resolves to a
                 * variable (local or global) instead, this is a call
                 * through a function-pointer value (e.g. tinyfs_init's
                 * read_fn/write_fn callbacks), not a direct `call name`.
                 * Locate the pointer the same way an ordinary variable
                 * reference would (see resolve_ident/codegen_addr) so
                 * codegen can load its value and `call eax`. */
                sym_t *vs = sym_find(p, name);
                if (vs && vs->type->kind == TY_PTR && vs->type->base->kind == TY_FUNC) {
                    call->is_indirect_call = 1;
                    call->is_global = vs->is_global;
                    call->is_static_local = vs->is_static_local;
                    call->frame_offset = vs->frame_offset;
                    safe_strcpy(call->asm_label, vs->asm_label, MAX_NAME);
                    call->type = vs->type->base->base;
                }
            }
            /* Struct/union-returning call: needs a hidden temp slot in
             * *this* function's frame to hold the result (the callee
             * writes into it via a hidden pointer argument - see
             * parse_func/codegen.c's ND_RETURN). Stashed in call->num
             * (unused by ND_CALL otherwise) rather than
             * call->frame_offset, which is_indirect_call above may
             * already be using for the callee variable's own location. */
            if (call->type->kind == TY_STRUCT || call->type->kind == TY_UNION) {
                p->frame_size += type_size(call->type);
                p->frame_size = (p->frame_size + 3) & ~3;
                call->num = -p->frame_size;
            }
            return call;
        }
        return resolve_ident(p, name, ident_tok);
    }
    if (match(p, TK_LPAREN)) {
        node_t *n = parse_expr(p);
        expect(p, TK_RPAREN);
        return n;
    }
    perr(p, "expected expression");
    return make_num(0);
}

static node_t *parse_postfix(parser_t *p) {
    node_t *n = parse_primary(p);
    for (;;) {
        if (match(p, TK_LBRACKET)) {
            node_t *idx = parse_expr(p);
            expect(p, TK_RBRACKET);
            node_t *add = node_new(ND_BINOP);
            add->op = TK_PLUS;
            add->a = n;
            add->b = idx;
            add->type = decay(n->type);
            node_t *deref = node_new(ND_DEREF);
            deref->a = add;
            deref->type = add->type->base;
            n = deref;
        } else if (match(p, TK_DOT)) {
            token_t id = expect(p, TK_IDENT);
            member_t *m = find_member(n->type, id.text);
            if (!m) { perr(p, "no such member"); return n; }
            node_t *mem = node_new(ND_MEMBER);
            mem->a = n;
            safe_strcpy(mem->name, id.text, MAX_NAME);
            mem->type = m->type;
            mem->num = m->offset;
            n = mem;
        } else if (match(p, TK_ARROW)) {
            token_t id = expect(p, TK_IDENT);
            member_t *m = find_member(n->type->base, id.text);
            if (!m) { perr(p, "no such member"); return n; }
            node_t *mem = node_new(ND_ARROW);
            mem->a = n;
            safe_strcpy(mem->name, id.text, MAX_NAME);
            mem->type = m->type;
            mem->num = m->offset;
            n = mem;
        } else if (match(p, TK_INC)) {
            node_t *pi = node_new(ND_POSTINC); pi->a = n; pi->type = n->type; n = pi;
        } else if (match(p, TK_DEC)) {
            node_t *pd = node_new(ND_POSTDEC); pd->a = n; pd->type = n->type; n = pd;
        } else break;
    }
    return n;
}

static int looks_like_type_name(parser_t *p) {
    /* Used after '(' inside unary() to disambiguate a cast from a
     * parenthesized expression. */
    return is_type_start(p);
}

static node_t *parse_unary(parser_t *p) {
    if (match(p, TK_PLUS)) return parse_cast(p);
    if (match(p, TK_MINUS)) { node_t *n = node_new(ND_UNARY_MINUS); n->a = parse_cast(p); n->type = n->a->type; return n; }
    if (match(p, TK_NOT)) { node_t *n = node_new(ND_NOT); n->a = parse_cast(p); n->type = g_int_type; return n; }
    if (match(p, TK_TILDE)) { node_t *n = node_new(ND_BITNOT); n->a = parse_cast(p); n->type = n->a->type; return n; }
    if (match(p, TK_AMP)) { node_t *n = node_new(ND_ADDR); n->a = parse_cast(p); n->type = ptr_to(n->a->type); return n; }
    if (match(p, TK_STAR)) {
        node_t *n = node_new(ND_DEREF);
        n->a = parse_cast(p);
        n->type = decay(n->a->type)->base;
        return n;
    }
    if (match(p, TK_INC)) { node_t *n = node_new(ND_PREINC); n->a = parse_unary(p); n->type = n->a->type; return n; }
    if (match(p, TK_DEC)) { node_t *n = node_new(ND_PREDEC); n->a = parse_unary(p); n->type = n->a->type; return n; }
    if (is_kw(p, "sizeof")) {
        advance(p);
        if (check(p, TK_LPAREN) && (p->toks[p->pos + 1].kind == TK_IDENT) ) {
            /* could be sizeof(type) or sizeof(expr) - peek for a type start */
            int save = p->pos;
            advance(p);
            if (looks_like_type_name(p)) {
                type_t *t = parse_type_name(p);
                expect(p, TK_RPAREN);
                return make_num(type_size(t));
            }
            p->pos = save;
        }
        node_t *operand = parse_unary(p);
        return make_num(type_size(operand->type));
    }
    /* va_start/va_arg/va_end: not real functions (va_arg's second
     * argument is a type, and CC's preprocessor has no function-like
     * macros to hide any of this behind - see toolchain/include/stdarg.h),
     * so these are parsed directly, like sizeof above. va_list is just
     * `char *`; a cdecl frame's named params sit below the varargs in
     * ascending address order, so:
     *   va_start(ap, last) desugars to  ap = (char*)&last + K
     *   va_arg(ap, T)       desugars to  *(T*)((ap += K) - K)
     * (the compound-assign-then-subtract reads the *old* ap while still
     * advancing it - the only way to get pre-increment-of-a-pointer
     * semantics as a single expression without a temporary), where K is
     * the (4-byte-rounded) size of the type being skipped/read. */
    if (is_kw(p, "va_start")) {
        advance(p);
        expect(p, TK_LPAREN);
        node_t *ap = parse_assign(p);
        expect(p, TK_COMMA);
        token_t id = expect(p, TK_IDENT);
        node_t *last = resolve_ident(p, id.text, id);
        expect(p, TK_RPAREN);
        int k = (type_size(last->type) + 3) & ~3;
        if (k < 4) k = 4;
        node_t *addr = node_new(ND_ADDR);
        addr->a = last;
        addr->type = ptr_to(last->type);
        node_t *cast = node_new(ND_CAST);
        cast->a = addr;
        cast->type = ptr_to(g_char_type);
        node_t *sum = make_binop(TK_PLUS, cast, make_num(k));
        node_t *assign = node_new(ND_ASSIGN);
        assign->a = ap;
        assign->b = sum;
        assign->type = ap->type;
        return assign;
    }
    if (is_kw(p, "va_arg")) {
        advance(p);
        expect(p, TK_LPAREN);
        node_t *ap = parse_assign(p);
        expect(p, TK_COMMA);
        type_t *t = parse_type_name(p);
        expect(p, TK_RPAREN);
        int k = (type_size(t) + 3) & ~3;
        if (k < 4) k = 4;
        node_t *advance_ap = node_new(ND_COMPOUND_ASSIGN);
        advance_ap->op = TK_PLUSEQ;
        advance_ap->a = ap;
        advance_ap->b = make_num(k);
        advance_ap->type = ap->type;
        node_t *old = make_binop(TK_MINUS, advance_ap, make_num(k));
        node_t *cast = node_new(ND_CAST);
        cast->a = old;
        cast->type = ptr_to(t);
        node_t *deref = node_new(ND_DEREF);
        deref->a = cast;
        deref->type = t;
        return deref;
    }
    if (is_kw(p, "va_end")) {
        advance(p);
        expect(p, TK_LPAREN);
        parse_assign(p); /* side-effect-free (a bare va_list variable) - discarded */
        expect(p, TK_RPAREN);
        return make_num(0);
    }
    return parse_postfix(p);
}

static node_t *parse_cast(parser_t *p) {
    if (check(p, TK_LPAREN)) {
        int save = p->pos;
        advance(p);
        if (looks_like_type_name(p)) {
            type_t *t = parse_type_name(p);
            expect(p, TK_RPAREN);
            node_t *n = node_new(ND_CAST);
            n->a = parse_cast(p);
            n->type = t;
            return n;
        }
        p->pos = save;
    }
    return parse_unary(p);
}

static int is_ptr(type_t *t) { return t->kind == TY_PTR || t->kind == TY_ARRAY; }

static node_t *make_binop(tok_kind_t op, node_t *a, node_t *b) {
    node_t *n = node_new(ND_BINOP);
    n->op = op;
    n->a = a;
    n->b = b;
    type_t *at = decay(a->type), *bt = decay(b->type);
    /* `ptr - ptr` (e.g. toolchain/as/elf_writer.c's `s - ctx->syms`, an
     * array-position computation - Phase T8's self-hosting build is
     * what first exercised this) yields ptrdiff_t, an *integer* element
     * count, not another pointer - must be checked before the general
     * "either side is a pointer" cases below, which would otherwise
     * mislabel it as pointer-typed and make codegen_binop's scaling
     * logic multiply an address by the element size instead of dividing
     * the byte difference by it (see docs/toolchain.md). */
    if (op == TK_MINUS && is_ptr(at) && is_ptr(bt)) n->type = g_int_type;
    else if ((op == TK_PLUS || op == TK_MINUS) && is_ptr(at)) n->type = at;
    else if (op == TK_PLUS && is_ptr(bt)) n->type = bt;
    else n->type = g_int_type;
    return n;
}

static node_t *parse_binop_level(parser_t *p, node_t *(*next)(parser_t *), const tok_kind_t *ops, int nops) {
    node_t *n = next(p);
    for (;;) {
        int matched = 0;
        for (int i = 0; i < nops; i++) {
            if (check(p, ops[i])) {
                tok_kind_t op = advance(p).kind;
                n = make_binop(op, n, next(p));
                matched = 1;
                break;
            }
        }
        if (!matched) return n;
    }
}

static node_t *parse_mul(parser_t *p) { static const tok_kind_t o[] = {TK_STAR, TK_SLASH, TK_PERCENT}; return parse_binop_level(p, parse_cast, o, 3); }
static node_t *parse_add(parser_t *p) { static const tok_kind_t o[] = {TK_PLUS, TK_MINUS}; return parse_binop_level(p, parse_mul, o, 2); }
static node_t *parse_shift(parser_t *p) { static const tok_kind_t o[] = {TK_SHL, TK_SHR}; return parse_binop_level(p, parse_add, o, 2); }
static node_t *parse_rel(parser_t *p) { static const tok_kind_t o[] = {TK_LT, TK_LE, TK_GT, TK_GE}; return parse_binop_level(p, parse_shift, o, 4); }
static node_t *parse_eq(parser_t *p) { static const tok_kind_t o[] = {TK_EQ, TK_NEQ}; return parse_binop_level(p, parse_rel, o, 2); }
static node_t *parse_bitand(parser_t *p) { static const tok_kind_t o[] = {TK_AMP}; return parse_binop_level(p, parse_eq, o, 1); }
static node_t *parse_bitxor(parser_t *p) { static const tok_kind_t o[] = {TK_CARET}; return parse_binop_level(p, parse_bitand, o, 1); }
static node_t *parse_bitor(parser_t *p) { static const tok_kind_t o[] = {TK_PIPE}; return parse_binop_level(p, parse_bitxor, o, 1); }

static node_t *parse_logand(parser_t *p) {
    node_t *n = parse_bitor(p);
    while (match(p, TK_AND)) { node_t *r = parse_bitor(p); node_t *a = node_new(ND_LOGAND); a->a = n; a->b = r; a->type = g_int_type; n = a; }
    return n;
}
static node_t *parse_logor(parser_t *p) {
    node_t *n = parse_logand(p);
    while (match(p, TK_OR)) { node_t *r = parse_logand(p); node_t *o = node_new(ND_LOGOR); o->a = n; o->b = r; o->type = g_int_type; n = o; }
    return n;
}

static node_t *parse_ternary(parser_t *p) {
    node_t *cond = parse_logor(p);
    if (match(p, TK_QUESTION)) {
        node_t *t = node_new(ND_TERNARY);
        t->a = cond;
        t->b = parse_expr(p);
        expect(p, TK_COLON);
        t->c = parse_assign(p);
        t->type = t->b->type;
        return t;
    }
    return cond;
}

static int is_assign_op(tok_kind_t k) {
    return k == TK_ASSIGN || k == TK_PLUSEQ || k == TK_MINUSEQ || k == TK_STAREQ || k == TK_SLASHEQ ||
           k == TK_PERCENTEQ || k == TK_ANDEQ || k == TK_OREQ || k == TK_XOREQ || k == TK_SHLEQ || k == TK_SHREQ;
}

static node_t *parse_assign(parser_t *p) {
    node_t *lhs = parse_ternary(p);
    if (is_assign_op(cur(p).kind)) {
        tok_kind_t op = advance(p).kind;
        node_t *rhs = parse_assign(p);
        if (op == TK_ASSIGN) {
            node_t *n = node_new(ND_ASSIGN);
            n->a = lhs; n->b = rhs; n->type = lhs->type;
            return n;
        }
        node_t *n = node_new(ND_COMPOUND_ASSIGN);
        n->op = op;
        n->a = lhs; n->b = rhs; n->type = lhs->type;
        return n;
    }
    return lhs;
}

/* The comma *operator* (evaluate left, discard, evaluate right - e.g. a
 * for-loop increment clause like `pn = pn->next, i++`), distinct from
 * comma as a plain separator (call args, declarator lists, initializer
 * elements) - those call parse_assign directly for each element, so
 * this only ever fires where a full expression was asked for. */
static node_t *parse_expr(parser_t *p) {
    node_t *n = parse_assign(p);
    while (match(p, TK_COMMA)) {
        node_t *rhs = parse_assign(p);
        node_t *c = node_new(ND_COMMA);
        c->a = n;
        c->b = rhs;
        c->type = rhs->type;
        n = c;
    }
    return n;
}

/* ---------------- initializers ---------------- */

/* `{ v1, v2, ... }` or a single scalar expression. Brace lists are only
 * meaningful for globals/statics (emitted directly as .data bytes) -
 * this codebase never initializes a plain (stack) local array/struct
 * with a brace list, only scalars, so codegen never needs to lower a
 * brace list into runtime store instructions. */
static node_t *parse_initializer(parser_t *p) {
    if (check(p, TK_LBRACE)) {
        advance(p);
        node_t *list = node_new(ND_BLOCK); /* reused as a plain node chain holder */
        node_t *tail = NULL;
        while (!check(p, TK_RBRACE)) {
            node_t *v = parse_initializer(p);
            if (!list->list) list->list = v; else tail->next = v;
            tail = v;
            if (!match(p, TK_COMMA)) break;
        }
        expect(p, TK_RBRACE);
        return list;
    }
    return parse_assign(p);
}

/* ---------------- statements ---------------- */

static node_t *parse_one_local_decl(parser_t *p, type_t *base, int is_static) {
    char name[MAX_NAME];
    type_t *t = parse_declarator(p, base, name, NULL, NULL);
    node_t *n = node_new(ND_VAR_DECL);
    safe_strcpy(n->name, name, MAX_NAME);
    n->type = t;
    n->is_static_local = is_static;
    if (match(p, TK_ASSIGN)) n->init = parse_initializer(p);
    /* Same `T name[] = { ... }` size-inference fix as parse_global_var -
     * needed here too for e.g. a function-local `static const T ops[] =
     * {...};` (used throughout the kernel's scripting-language parser
     * files) - see that function's comment for why array_len otherwise
     * silently means sizeof(name) == 0.
     *
     * Must run *before* add_local() below, not after (a real, previously-
     * latent bug this exact ordering caused: add_local() sizes the stack
     * slot via type_size(t), which for an unsized TY_ARRAY with
     * array_len still -1 comes back as 0, rounded up to the "minimum
     * 4-byte slot" fallback - so a genuinely 60-byte local array like
     * is_type_start's own `const char *kws[] = {...}` (Phase T8's
     * self-hosting build is what first compiled this function through
     * CC instead of gcc) got only 4 bytes reserved, and initializing
     * kws[1..14] then wrote straight through the saved ebp, the return
     * address, and the caller's own stack args - see docs/toolchain.md).
     * Inferring the size first means add_local always sees the real,
     * final array_len. */
    if (t->kind == TY_ARRAY && t->array_len < 0 && n->init && n->init->kind == ND_BLOCK) {
        int count = 0;
        for (node_t *v = n->init->list; v; v = v->next) count++;
        t->array_len = count;
    }
    sym_t *s = add_local(p, name, t, is_static);
    safe_strcpy(n->asm_label, s->asm_label, MAX_NAME);
    n->frame_offset = s->frame_offset;
    return n;
}

static node_t *parse_local_decl_stmt(parser_t *p) {
    int is_static = 0, is_extern = 0;
    type_t *base = parse_type_spec(p, &is_static, &is_extern);
    node_t *head = parse_one_local_decl(p, base, is_static);
    node_t *tail = head;
    while (match(p, TK_COMMA)) {
        node_t *n = parse_one_local_decl(p, base, is_static);
        tail->next = n;
        tail = n;
    }
    expect(p, TK_SEMI);
    return head;
}

static node_t *parse_stmt(parser_t *p) {
    if (check(p, TK_LBRACE)) return parse_block(p);

    if (match_kw(p, "if")) {
        expect(p, TK_LPAREN);
        node_t *n = node_new(ND_IF);
        n->a = parse_expr(p);
        expect(p, TK_RPAREN);
        n->b = parse_stmt(p);
        if (match_kw(p, "else")) n->c = parse_stmt(p);
        return n;
    }
    if (match_kw(p, "while")) {
        expect(p, TK_LPAREN);
        node_t *n = node_new(ND_WHILE);
        n->a = parse_expr(p);
        expect(p, TK_RPAREN);
        n->b = parse_stmt(p);
        return n;
    }
    if (match_kw(p, "do")) {
        node_t *n = node_new(ND_DOWHILE);
        n->b = parse_stmt(p);
        if (!match_kw(p, "while")) perr(p, "expected 'while'");
        expect(p, TK_LPAREN);
        n->a = parse_expr(p);
        expect(p, TK_RPAREN);
        expect(p, TK_SEMI);
        return n;
    }
    if (match_kw(p, "for")) {
        expect(p, TK_LPAREN);
        node_t *n = node_new(ND_FOR);
        if (!check(p, TK_SEMI)) {
            n->a = is_type_start(p) ? parse_local_decl_stmt(p) : NULL;
            if (!n->a) { n->a = parse_expr(p); expect(p, TK_SEMI); }
        } else { advance(p); }
        if (!check(p, TK_SEMI)) n->b = parse_expr(p);
        expect(p, TK_SEMI);
        if (!check(p, TK_RPAREN)) n->c = parse_expr(p);
        expect(p, TK_RPAREN);
        n->d = parse_stmt(p);
        return n;
    }
    if (match_kw(p, "return")) {
        node_t *n = node_new(ND_RETURN);
        if (!check(p, TK_SEMI)) n->a = parse_expr(p);
        expect(p, TK_SEMI);
        return n;
    }
    if (match_kw(p, "break")) { expect(p, TK_SEMI); return node_new(ND_BREAK); }
    if (match_kw(p, "continue")) { expect(p, TK_SEMI); return node_new(ND_CONTINUE); }
    if (match_kw(p, "switch")) {
        expect(p, TK_LPAREN);
        node_t *n = node_new(ND_SWITCH);
        n->a = parse_expr(p);
        expect(p, TK_RPAREN);
        /* Hidden frame slot to hold the switch value across codegen's
         * compare chain - same "min 4-byte slot" convention as
         * add_local, but with no symbol-table entry (never referenced
         * by name, so nothing else needs to find it). */
        p->frame_size += 4;
        p->frame_size = (p->frame_size + 3) & ~3;
        n->frame_offset = -p->frame_size;
        n->b = parse_block(p);
        return n;
    }
    /* `case`/`default` are zero-width label statements, not statements
     * with their own body (see codegen.c's ND_SWITCH) - parse_block's
     * ordinary statement loop naturally makes the *next* statement a
     * separate list entry, which is exactly what C's fallthrough
     * semantics need. */
    if (match_kw(p, "case")) {
        node_t *n = node_new(ND_CASE);
        n->num = parse_const_expr(p);
        expect(p, TK_COLON);
        return n;
    }
    if (match_kw(p, "default")) {
        node_t *n = node_new(ND_DEFAULT);
        expect(p, TK_COLON);
        return n;
    }
    if (match(p, TK_SEMI)) return node_new(ND_NOP);

    if (is_type_start(p)) return parse_local_decl_stmt(p);

    node_t *n = node_new(ND_EXPR_STMT);
    n->a = parse_expr(p);
    expect(p, TK_SEMI);
    return n;
}

static node_t *parse_block(parser_t *p) {
    expect(p, TK_LBRACE);
    node_t *blk = node_new(ND_BLOCK);
    node_t *tail = NULL;
    while (!check(p, TK_RBRACE)) {
        node_t *s = parse_stmt(p);
        if (!blk->list) blk->list = s; else tail->next = s;
        tail = s;
        while (tail->next) tail = tail->next; /* parse_local_decl_stmt may chain multiple decls */
    }
    expect(p, TK_RBRACE);
    return blk;
}

/* ---------------- top level ---------------- */

static node_t *parse_global_var(parser_t *p, const char *name, type_t *t, int is_extern, int is_static) {
    add_global(p, name, t);
    node_t *n = node_new(ND_VAR_DECL);
    safe_strcpy(n->name, name, MAX_NAME);
    n->type = t;
    n->is_global = 1;
    n->is_extern_decl = is_extern;
    n->is_file_static = is_static;
    safe_strcpy(n->asm_label, name, MAX_NAME);
    if (match(p, TK_ASSIGN)) n->init = parse_initializer(p);
    /* `T name[] = { ... };` - array_len was left at -1 by parse_declarator
     * (no size between the brackets), and type_size() treats a non-
     * positive array_len as size 0 - silently making sizeof(name) come
     * out as 0 instead of the real, initializer-inferred length. Fix it
     * up now that the initializer list is available to count. */
    if (t->kind == TY_ARRAY && t->array_len < 0 && n->init && n->init->kind == ND_BLOCK) {
        int count = 0;
        for (node_t *v = n->init->list; v; v = v->next) count++;
        t->array_len = count;
    }
    return n;
}

static node_t *parse_func(parser_t *p, const char *name, type_t *functy,
                            char param_names[][MAX_NAME], int n_params, int is_static) {
    sym_t *fs = add_global(p, name, functy);

    if (match(p, TK_SEMI)) return NULL; /* prototype only */

    p->locals = NULL;
    p->frame_size = 0;
    p->cur_func_ret = functy->base;

    node_t *n = node_new(ND_FUNC_DEF);
    safe_strcpy(n->name, name, MAX_NAME);
    n->type = functy->base;
    n->is_global = !is_static; /* controls whether codegen emits `global` - C's static limits linkage */
    safe_strcpy(n->asm_label, name, MAX_NAME);

    /* cdecl: EBP+8, +12, ... for params, in order - except when the
     * return type is a struct/union, which doesn't fit in EAX. That
     * uses the classic hidden-pointer convention instead: the caller
     * passes the address to write the result into as an implicit first
     * argument, at [ebp+8], pushing every real parameter's offset out
     * by 4 (see codegen.c's ND_RETURN and codegen_call_or_builtin). */
    int returns_struct = functy->base->kind == TY_STRUCT || functy->base->kind == TY_UNION;
    type_t *pt = functy->param;
    int off = returns_struct ? 12 : 8;
    node_t *ptail = NULL;
    for (int i = 0; i < n_params && pt; i++, pt = pt->next) {
        sym_t *s = calloc(1, sizeof(sym_t));
        safe_strcpy(s->name, param_names[i], MAX_NAME);
        s->type = pt;
        s->frame_offset = off;
        s->next = p->locals;
        p->locals = s;
        /* Struct/union parameters are passed by value as their real
         * (4-byte-rounded) size, not a flat 4 bytes like every scalar/
         * pointer param - the caller copies the whole struct onto the
         * stack (see codegen_call_or_builtin), so subsequent params'
         * offsets need to account for its actual width. */
        if (pt->kind == TY_STRUCT || pt->kind == TY_UNION) {
            int sz = (type_size(pt) + 3) & ~3;
            off += sz < 4 ? 4 : sz;
        } else {
            off += 4;
        }

        node_t *pn = node_new(ND_VAR_DECL);
        safe_strcpy(pn->name, param_names[i], MAX_NAME);
        pn->type = pt;
        pn->frame_offset = s->frame_offset;
        if (!n->list) n->list = pn; else ptail->next = pn;
        ptail = pn;
    }

    n->b = parse_block(p);
    n->num = p->frame_size;
    fs->type = functy;
    return n;
}

static node_t *parse_typedef(parser_t *p) {
    advance(p); /* 'typedef' */
    type_t *base = parse_type_spec(p, NULL, NULL);
    char name[MAX_NAME];
    type_t *t = parse_declarator(p, base, name, NULL, NULL);
    expect(p, TK_SEMI);
    sym_t *s = calloc(1, sizeof(sym_t));
    safe_strcpy(s->name, name, MAX_NAME);
    s->type = t;
    s->is_typedef = 1;
    s->next = p->globals;
    p->globals = s;
    return node_new(ND_NOP);
}

node_t *cc_parse(token_t *toks, int n_toks) {
    g_int_type = type_new(TY_INT);
    g_char_type = type_new(TY_CHAR);

    parser_t p = {0};
    p.toks = toks;
    p.n = n_toks;

    node_t *head = NULL, *tail = NULL;
    while (!check(&p, TK_EOF)) {
        node_t *decl = NULL;
        if (is_kw(&p, "typedef")) {
            decl = parse_typedef(&p);
        } else {
            int is_static = 0, is_extern = 0;
            type_t *base = parse_type_spec(&p, &is_static, &is_extern);
            if (match(&p, TK_SEMI)) {
                decl = node_new(ND_NOP); /* lone `struct Foo { ... };` with no variable */
            } else {
                char param_names[16][MAX_NAME];
                int n_params = 0;
                char name[MAX_NAME];
                type_t *t = parse_declarator(&p, base, name, param_names, &n_params);
                if (t->kind == TY_FUNC) {
                    decl = parse_func(&p, name, t, param_names, n_params, is_static);
                } else {
                    decl = parse_global_var(&p, name, t, is_extern, is_static);
                    while (match(&p, TK_COMMA)) {
                        char name2[MAX_NAME];
                        type_t *t2 = parse_declarator(&p, base, name2, NULL, NULL);
                        node_t *more = parse_global_var(&p, name2, t2, is_extern, is_static);
                        decl->next = more;
                    }
                    expect(&p, TK_SEMI);
                }
            }
        }
        if (decl) {
            if (!head) head = decl; else tail->next = decl;
            tail = decl;
            while (tail->next) tail = tail->next;
        }
    }
    return head;
}
