#include "as.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

symbol_t *sym_find(asm_ctx_t *ctx, const char *name) {
    for (int i = 0; i < ctx->nsyms; i++) {
        if (strcmp(ctx->syms[i].name, name) == 0) return &ctx->syms[i];
    }
    return NULL;
}

symbol_t *sym_get_or_create(asm_ctx_t *ctx, const char *name) {
    symbol_t *s = sym_find(ctx, name);
    if (s) return s;
    if (ctx->nsyms >= MAX_SYMS) { fprintf(stderr, "as: too many symbols\n"); exit(1); }
    s = &ctx->syms[ctx->nsyms++];
    memset(s, 0, sizeof(*s));
    safe_strcpy(s->name, name, MAX_NAME);
    return s;
}

/* --- per-line token buffer --- */

#define MAX_LINE_TOKS 32
typedef struct {
    token_t toks[MAX_LINE_TOKS];
    int n;
    int pos;
} line_t;

static token_t line_peek(line_t *ln) {
    if (ln->pos < ln->n) return ln->toks[ln->pos];
    /* Not a compound literal (CC doesn't support them - see
     * docs/toolchain.md, Phase T3/T8): memset then set the one field
     * that matters, same zeroed-rest semantics as `{ .kind = TK_EOF }`. */
    token_t t;
    memset(&t, 0, sizeof(t));
    t.kind = TK_EOF;
    return t;
}
static token_t line_next(line_t *ln) {
    token_t t = line_peek(ln);
    if (ln->pos < ln->n) ln->pos++;
    return t;
}
static int ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}
static int line_is_ident(line_t *ln, const char *kw) {
    token_t t = line_peek(ln);
    return t.kind == TK_IDENT && ieq(t.text, kw);
}

/* --- state carried across the whole parse --- */

typedef struct {
    asm_ctx_t *ctx;
    int bits;
    section_kind_t cur_section;
    uint32_t offsets[MAX_SECTIONS]; /* running location counter per section */
    const char *filename;
    int line_no;
    int had_error;
} pstate_t;

static void perr(pstate_t *ps, const char *msg) {
    fprintf(stderr, "as: %s:%d: %s\n", ps->filename, ps->line_no, msg);
    ps->had_error = 1;
}

static void mark_section_used(asm_ctx_t *ctx, section_kind_t s) {
    ctx->sec_used[s] = 1;
}

/* --- register table --- */

typedef struct { const char *name; reg_t reg; int size; } regdef_t;
static const regdef_t REGS[] = {
    {"al",R_AL,1},{"cl",R_CL,1},{"dl",R_DL,1},{"bl",R_BL,1},
    {"ah",R_AH,1},{"ch",R_CH,1},{"dh",R_DH,1},{"bh",R_BH,1},
    {"ax",R_AX,2},{"cx",R_CX,2},{"dx",R_DX,2},{"bx",R_BX,2},
    {"sp",R_SP,2},{"bp",R_BP,2},{"si",R_SI,2},{"di",R_DI,2},
    {"eax",R_EAX,4},{"ecx",R_ECX,4},{"edx",R_EDX,4},{"ebx",R_EBX,4},
    {"esp",R_ESP,4},{"ebp",R_EBP,4},{"esi",R_ESI,4},{"edi",R_EDI,4},
    {"es",R_ES,2},{"cs",R_CS,2},{"ss",R_SS,2},{"ds",R_DS,2},{"fs",R_FS,2},{"gs",R_GS,2},
    {"cr0",R_CR0,4},
};

static const regdef_t *reg_lookup(const char *name) {
    for (size_t i = 0; i < sizeof(REGS) / sizeof(REGS[0]); i++) {
        if (strcmp(REGS[i].name, name) == 0) return &REGS[i];
    }
    return NULL;
}

/* --- expression evaluation ---
 * Returns a value that's either a pure constant or "one symbol + constant
 * addend" - the only two shapes this codebase's expressions ever need
 * (see docs/toolchain.md). Symbols already defined fold to constants
 * immediately; at most one *undefined-so-far* symbol may appear. */

typedef struct {
    int has_sym;
    char sym[MAX_NAME];
    int64_t val; /* constant, or addend when has_sym */
} value_t;

static value_t val_const(int64_t v) { value_t r = {0, {0}, v}; return r; }

static value_t eval_primary(pstate_t *ps, line_t *ln);
static value_t eval_expr(pstate_t *ps, line_t *ln);

/* NASM-style local labels (`.name`) are scoped to the most recent
 * preceding global label - qualify to a unique internal name so
 * `.hang`/`.loop`/etc. reused across many labels don't collide. */
static void qualify_local(pstate_t *ps, const char *raw, char *out, size_t outsz) {
    if (raw[0] == '.') {
        size_t i = 0;
        const char *g = ps->ctx->last_global_label;
        for (; i + 1 < outsz && g[i]; i++) out[i] = g[i];
        for (size_t j = 0; i + 1 < outsz && raw[j]; i++, j++) out[i] = raw[j];
        out[i] = '\0';
    } else {
        safe_strcpy(out, raw, outsz);
    }
}

static value_t val_add(pstate_t *ps, value_t a, value_t b, int negate_b) {
    if (a.has_sym && b.has_sym) {
        /* A difference of two labels already defined in the same
         * section is link-time-invariant (just a byte count), so it's
         * safe to fold to a plain constant regardless of context (EQU
         * or an operand/data value) - this is what gdt_end - gdt_start
         * and CODE_SEG/DATA_SEG rely on. Anything else combining two
         * still-symbolic values isn't needed by this codebase. */
        if (negate_b) {
            symbol_t *sa = sym_find(ps->ctx, a.sym);
            symbol_t *sb = sym_find(ps->ctx, b.sym);
            if (sa && sa->defined && sb && sb->defined && sa->section == sb->section) {
                int64_t diff = (int64_t)sa->offset - (int64_t)sb->offset;
                return val_const(diff + a.val - b.val);
            }
        }
        perr(ps, "expression combines two unresolved symbols");
        return val_const(0);
    }
    value_t r;
    r.has_sym = a.has_sym || b.has_sym;
    if (r.has_sym) {
        if (a.has_sym) { safe_strcpy(r.sym, a.sym, MAX_NAME); r.val = a.val + (negate_b ? -b.val : b.val); }
        else { safe_strcpy(r.sym, b.sym, MAX_NAME); r.val = (negate_b ? -b.val : b.val) + a.val; }
    } else {
        r.val = a.val + (negate_b ? -b.val : b.val);
        r.sym[0] = '\0';
    }
    return r;
}

static value_t eval_primary(pstate_t *ps, line_t *ln) {
    token_t t = line_peek(ln);
    if (t.kind == TK_NUMBER) { line_next(ln); return val_const(t.number); }
    if (t.kind == TK_MINUS) {
        line_next(ln);
        value_t v = eval_primary(ps, ln);
        if (v.has_sym) { perr(ps, "cannot negate a symbol"); return val_const(0); }
        return val_const(-v.val);
    }
    if (t.kind == TK_LPAREN) {
        line_next(ln);
        value_t v = eval_expr(ps, ln);
        if (line_peek(ln).kind == TK_RPAREN) line_next(ln); else perr(ps, "expected )");
        return v;
    }
    if (t.kind == TK_DOLLAR) {
        line_next(ln);
        return val_const((int64_t)ps->offsets[ps->cur_section]);
    }
    if (t.kind == TK_DOLLARDOLLAR) {
        line_next(ln);
        return val_const(0); /* start of the current section - always offset 0 */
    }
    if (t.kind == TK_IDENT) {
        line_next(ln);
        char qname[MAX_NAME];
        qualify_local(ps, t.text, qname, sizeof(qname));
        symbol_t *s = sym_find(ps->ctx, qname);
        if (s && s->is_equ) return val_const(s->equ_value);
        /* Labels always stay symbolic, even once defined, so callers
         * route them through resolve_abs/resolve_pcrel in encode.c,
         * which apply ORG (-f bin) or emit relocations (-f elf32) as
         * needed - see the val_add note above for the one exception. */
        value_t r = {1, {0}, 0};
        safe_strcpy(r.sym, qname, MAX_NAME);
        return r;
    }
    perr(ps, "expected a value");
    return val_const(0);
}

static value_t eval_mul(pstate_t *ps, line_t *ln) {
    value_t v = eval_primary(ps, ln);
    for (;;) {
        token_t t = line_peek(ln);
        if (t.kind == TK_STAR || t.kind == TK_SLASH || t.kind == TK_SHL || t.kind == TK_SHR) {
            line_next(ln);
            value_t r = eval_primary(ps, ln);
            if (v.has_sym || r.has_sym) { perr(ps, "cannot multiply/shift a symbol"); continue; }
            switch (t.kind) {
                case TK_STAR: v = val_const(v.val * r.val); break;
                case TK_SLASH: v = val_const(r.val != 0 ? v.val / r.val : 0); break;
                case TK_SHL: v = val_const(v.val << r.val); break;
                case TK_SHR: v = val_const(v.val >> r.val); break;
                default: break;
            }
        } else break;
    }
    return v;
}

static value_t eval_expr(pstate_t *ps, line_t *ln) {
    value_t v = eval_mul(ps, ln);
    for (;;) {
        token_t t = line_peek(ln);
        if (t.kind == TK_PLUS) { line_next(ln); v = val_add(ps, v, eval_mul(ps, ln), 0); }
        else if (t.kind == TK_MINUS) { line_next(ln); v = val_add(ps, v, eval_mul(ps, ln), 1); }
        else break;
    }
    return v;
}

/* --- operand parsing --- */

static int size_keyword(const char *w, int *size) {
    if (ieq(w, "byte")) { *size = 1; return 1; }
    if (ieq(w, "word")) { *size = 2; return 1; }
    if (ieq(w, "dword")) { *size = 4; return 1; }
    return 0;
}

static operand_t parse_mem(pstate_t *ps, line_t *ln) {
    operand_t o = {0};
    o.kind = OPND_MEM;
    /* [reg], [reg+disp], [reg-disp], [sym], [sym+disp] */
    token_t t = line_peek(ln);
    if (t.kind == TK_IDENT) {
        const regdef_t *r = reg_lookup(t.text);
        if (r) { o.reg = r->reg; line_next(ln); }
    }
    if (o.reg == R_NONE) {
        value_t v = eval_expr(ps, ln);
        if (v.has_sym) { o.has_sym = 1; safe_strcpy(o.sym, v.sym, MAX_NAME); o.disp = v.val; }
        else { o.disp = v.val; }
    } else if (line_peek(ln).kind == TK_PLUS || line_peek(ln).kind == TK_MINUS) {
        int neg = line_peek(ln).kind == TK_MINUS;
        line_next(ln);
        value_t v = eval_expr(ps, ln);
        o.disp = neg ? -v.val : v.val; /* our only reg+/-disp forms use plain constants */
    }
    if (line_peek(ln).kind == TK_RBRACK) line_next(ln); else perr(ps, "expected ]");
    return o;
}

static operand_t parse_operand(pstate_t *ps, line_t *ln) {
    operand_t o = {0};
    int sz = 0;
    if (line_peek(ln).kind == TK_IDENT && size_keyword(line_peek(ln).text, &sz)) {
        line_next(ln);
        if (line_peek(ln).kind == TK_LBRACK) {
            /* `word|dword [mem]` */
            line_next(ln);
            o = parse_mem(ps, ln);
            o.size_override = sz;
            return o;
        }
        /* `dword N` - a size hint on an immediate (e.g. `push dword 0`) */
        value_t v = eval_expr(ps, ln);
        o.kind = OPND_IMM;
        o.has_sym = v.has_sym;
        if (v.has_sym) safe_strcpy(o.sym, v.sym, MAX_NAME);
        o.disp = v.val;
        o.size_override = sz;
        return o;
    }
    if (line_peek(ln).kind == TK_LBRACK) {
        line_next(ln);
        o = parse_mem(ps, ln);
        return o;
    }
    if (line_peek(ln).kind == TK_IDENT) {
        const regdef_t *r = reg_lookup(line_peek(ln).text);
        if (r) {
            line_next(ln);
            o.kind = OPND_REG;
            o.reg = r->reg;
            return o;
        }
    }
    value_t v = eval_expr(ps, ln);
    o.kind = OPND_IMM;
    o.has_sym = v.has_sym;
    if (v.has_sym) safe_strcpy(o.sym, v.sym, MAX_NAME);
    o.disp = v.val;
    return o;
}

/* --- statement/label emission --- */

static void push_stmt(pstate_t *ps, stmt_t st) {
    asm_ctx_t *ctx = ps->ctx;
    st.section = ps->cur_section;
    st.offset = ps->offsets[ps->cur_section];
    st.bits = ps->bits;
    st.line_no = ps->line_no;
    int len = stmt_encode(ctx, &st, NULL, 0); /* dry run: length only, no relocations yet */
    st.length = (uint32_t)len;
    if (ctx->nstmts >= MAX_STMTS) { fprintf(stderr, "as: too many statements\n"); exit(1); }
    ctx->stmts[ctx->nstmts++] = st;
    ps->offsets[ps->cur_section] += (uint32_t)len;
    mark_section_used(ctx, ps->cur_section);
}

static void define_label(pstate_t *ps, const char *raw_name) {
    asm_ctx_t *ctx = ps->ctx;
    char qname[MAX_NAME];
    qualify_local(ps, raw_name, qname, sizeof(qname));
    if (raw_name[0] != '.') {
        safe_strcpy(ctx->last_global_label, raw_name, MAX_NAME);
    }
    symbol_t *s = sym_get_or_create(ctx, qname);
    if (s->defined) { perr(ps, "duplicate label"); return; }
    s->defined = 1;
    s->section = ps->cur_section;
    s->offset = ps->offsets[ps->cur_section];
}

static section_kind_t normalize_section(const char *name) {
    if (ieq(name, ".text") || ieq(name, ".text.entry")) return SEC_TEXT;
    if (ieq(name, ".data")) return SEC_DATA;
    if (ieq(name, ".bss")) return SEC_BSS;
    if (ieq(name, ".rodata")) return SEC_RODATA;
    return SEC_TEXT;
}

/* --- data directive value lists (db/dw/dd/dq), split one scalar per stmt --- */

static void emit_data_value(pstate_t *ps, const char *mnem, value_t v) {
    stmt_t st = {0};
    safe_strcpy(st.mnemonic, mnem, sizeof(st.mnemonic));
    st.nops = 1;
    st.ops[0].kind = OPND_IMM;
    st.ops[0].has_sym = v.has_sym;
    if (v.has_sym) safe_strcpy(st.ops[0].sym, v.sym, MAX_NAME);
    st.ops[0].disp = v.val;
    push_stmt(ps, st);
}

static void parse_data_directive(pstate_t *ps, line_t *ln, const char *mnem) {
    for (;;) {
        if (line_peek(ln).kind == TK_STRING) {
            token_t t = line_next(ln);
            for (const char *p = t.text; *p; p++) {
                emit_data_value(ps, mnem, val_const((unsigned char)*p));
            }
        } else {
            value_t v = eval_expr(ps, ln);
            emit_data_value(ps, mnem, v);
        }
        if (line_peek(ln).kind == TK_COMMA) { line_next(ln); continue; }
        break;
    }
}

/* --- one line --- */

static void parse_rest_of_line(pstate_t *ps, line_t *ln);

static void parse_line(pstate_t *ps, line_t *ln) {
    if (line_peek(ln).kind == TK_EOF) return;

    /* label: */
    if (line_peek(ln).kind == TK_IDENT) {
        line_t save = *ln;
        token_t name = line_next(ln);
        if (line_peek(ln).kind == TK_COLON) {
            line_next(ln);
            define_label(ps, name.text);
            parse_rest_of_line(ps, ln);
            return;
        }
        /* NAME equ EXPR */
        if (line_is_ident(ln, "equ")) {
            line_next(ln);
            value_t v = eval_expr(ps, ln);
            if (v.has_sym) { perr(ps, "equ value must be constant"); return; }
            symbol_t *s = sym_get_or_create(ps->ctx, name.text);
            s->is_equ = 1;
            s->equ_value = v.val;
            return;
        }
        *ln = save;
    }
    parse_rest_of_line(ps, ln);
}

static void parse_rest_of_line(pstate_t *ps, line_t *ln) {
    if (line_peek(ln).kind == TK_EOF || line_peek(ln).kind == TK_NEWLINE) return;
    if (line_peek(ln).kind != TK_IDENT) { perr(ps, "expected directive/instruction"); return; }

    token_t head = line_next(ln);

    if (ieq(head.text, "BITS")) { ps->bits = (int)line_next(ln).number; return; }
    if (ieq(head.text, "ORG")) {
        value_t v = eval_expr(ps, ln);
        ps->ctx->org = (uint32_t)v.val;
        ps->ctx->have_org = 1;
        return;
    }
    if (ieq(head.text, "section")) {
        token_t name = line_next(ln);
        ps->cur_section = normalize_section(name.text);
        return;
    }
    if (ieq(head.text, "global")) {
        token_t name = line_next(ln);
        sym_get_or_create(ps->ctx, name.text)->is_global = 1;
        return;
    }
    if (ieq(head.text, "extern")) {
        token_t name = line_next(ln);
        sym_get_or_create(ps->ctx, name.text)->is_extern = 1;
        return;
    }
    if (ieq(head.text, "align")) {
        value_t v = eval_expr(ps, ln);
        uint32_t n = (uint32_t)v.val;
        uint32_t cur = ps->offsets[ps->cur_section];
        uint32_t pad = (n - (cur % n)) % n;
        stmt_t st = {0};
        safe_strcpy(st.mnemonic, ".resb", sizeof(st.mnemonic));
        st.nops = 1;
        st.ops[0].kind = OPND_IMM;
        st.ops[0].disp = pad;
        push_stmt(ps, st);
        return;
    }
    if (ieq(head.text, "resb")) {
        value_t v = eval_expr(ps, ln);
        stmt_t st = {0};
        safe_strcpy(st.mnemonic, ".resb", sizeof(st.mnemonic));
        st.nops = 1;
        st.ops[0].kind = OPND_IMM;
        st.ops[0].disp = v.val;
        push_stmt(ps, st);
        return;
    }
    if (ieq(head.text, "times")) {
        value_t count = eval_expr(ps, ln);
        token_t sizekw = line_next(ln); /* db/dw/dd */
        value_t val = eval_expr(ps, ln);
        (void)sizekw; /* our only usage is `times N db V` */
        stmt_t st = {0};
        safe_strcpy(st.mnemonic, ".pad", sizeof(st.mnemonic));
        st.nops = 2;
        st.ops[0].disp = count.val;
        st.ops[1].disp = val.val;
        push_stmt(ps, st);
        return;
    }
    if (ieq(head.text, "db")) { parse_data_directive(ps, ln, ".db"); return; }
    if (ieq(head.text, "dw")) { parse_data_directive(ps, ln, ".dw"); return; }
    if (ieq(head.text, "dd")) { parse_data_directive(ps, ln, ".dd"); return; }
    if (ieq(head.text, "dq")) { parse_data_directive(ps, ln, ".dq"); return; }

    /* instruction */
    stmt_t st = {0};
    safe_strcpy(st.mnemonic, head.text, sizeof(st.mnemonic));
    for (char *p = st.mnemonic; *p; p++) *p = (char)tolower((unsigned char)*p);

    if (line_peek(ln).kind != TK_NEWLINE && line_peek(ln).kind != TK_EOF) {
        st.ops[0] = parse_operand(ps, ln);
        st.nops = 1;
        /* far jmp: `jmp SEG:OFFSET` - the value just parsed as ops[0] is
         * actually the segment; re-read it as ops[0].far_seg and parse
         * the offset as the real ops[0]. */
        if (ieq(st.mnemonic, "jmp") && line_peek(ln).kind == TK_COLON) {
            line_next(ln);
            operand_t seg = st.ops[0];
            operand_t off = parse_operand(ps, ln);
            operand_t far = {0};
            far.kind = OPND_FAR;
            far.has_sym = off.has_sym;
            safe_strcpy(far.sym, off.sym, MAX_NAME);
            far.disp = off.disp;
            far.far_seg = seg.disp;
            far.far_seg_has_sym = seg.has_sym; /* e.g. a forward-referenced EQU like CODE_SEG - deferred to pass 2 */
            safe_strcpy(far.far_seg_sym, seg.sym, MAX_NAME);
            st.ops[0] = far;
        } else if (line_peek(ln).kind == TK_COMMA) {
            line_next(ln);
            st.ops[1] = parse_operand(ps, ln);
            st.nops = 2;
            /* only `imul r32, r/m32, imm32` uses a 3rd operand - without
             * this, a trailing `, imm` was silently dropped (never
             * consumed, never reported as an error) and the immediate
             * scale factor was lost, e.g. `imul ecx, ecx, 4` for array
             * indexing silently became `imul ecx, ecx` (self-square
             * instead of *4) - see docs/toolchain.md. */
            if (line_peek(ln).kind == TK_COMMA) {
                line_next(ln);
                st.ops[2] = parse_operand(ps, ln);
                st.nops = 3;
            }
        }
    }
    push_stmt(ps, st);
}

/* --- driver --- */

int as_parse_pass1(asm_ctx_t *ctx, const char *src, const char *filename) {
    pstate_t ps = {0};
    ps.ctx = ctx;
    ps.bits = 16;
    ps.cur_section = SEC_TEXT;
    ps.filename = filename;

    lexer_t lx;
    lexer_init(&lx, src);

    for (;;) {
        line_t ln = {0};
        for (;;) {
            token_t t = lexer_next(&lx);
            if (t.kind == TK_NEWLINE || t.kind == TK_EOF) {
                if (ln.n < MAX_LINE_TOKS) ln.toks[ln.n++] = t;
                break;
            }
            if (ln.n < MAX_LINE_TOKS) ln.toks[ln.n++] = t;
        }
        ps.line_no++;
        int is_eof = ln.toks[ln.n - 1].kind == TK_EOF;
        parse_line(&ps, &ln);
        if (is_eof) break;
    }

    return ps.had_error ? -1 : 0;
}
