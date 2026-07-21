#pragma once
/* CC: the self-hosted C compiler. See docs/toolchain.md for the "tinyC"
 * dialect this supports and why. Emits AS-syntax assembly text (mirrors
 * `gcc -S | as`), for two targets: --target=kernel (freestanding, no
 * runtime) and --target=host (adds a _start that calls main then exits
 * via a raw syscall - see builtins.c). Native host tool, not OS code. */

#include <stdint.h>
#include <stddef.h>

static inline void safe_strcpy(char *dst, const char *src, size_t dstsize) {
    size_t i = 0;
    for (; i + 1 < dstsize && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

#define MAX_NAME 64
#define MAX_TOKENS 65536
#define MAX_INCLUDE_DEPTH 16

/* ---------------- lexer ---------------- */

typedef enum {
    TK_EOF, TK_IDENT, TK_NUM, TK_STR, TK_CHAR,
    /* punctuation/operators */
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE, TK_LBRACKET, TK_RBRACKET,
    TK_SEMI, TK_COMMA, TK_COLON, TK_QUESTION, TK_DOT, TK_ARROW, TK_ELLIPSIS,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_ASSIGN, TK_PLUSEQ, TK_MINUSEQ, TK_STAREQ, TK_SLASHEQ, TK_PERCENTEQ,
    TK_ANDEQ, TK_OREQ, TK_XOREQ, TK_SHLEQ, TK_SHREQ,
    TK_EQ, TK_NEQ, TK_LT, TK_LE, TK_GT, TK_GE,
    TK_AND, TK_OR, TK_NOT, TK_AMP, TK_PIPE, TK_CARET, TK_TILDE,
    TK_SHL, TK_SHR, TK_INC, TK_DEC,
    /* preprocessor-only (consumed by the preprocessor, never reach the parser) */
    TK_HASH, TK_NEWLINE,
} tok_kind_t;

typedef struct {
    tok_kind_t kind;
    char text[256];  /* TK_IDENT / TK_STR (decoded) */
    int64_t num;       /* TK_NUM / TK_CHAR */
    int line;
    const char *file;
    int bol; /* beginning of line (possibly after whitespace only) - for # detection */
} token_t;

typedef struct {
    const char *src;
    size_t pos, len;
    const char *filename;
    int line;
    int bol;
    int has_pushback;
    token_t pushback;
} lexer_t;

void lexer_init(lexer_t *lx, const char *src, const char *filename);
token_t lexer_next(lexer_t *lx);
/* Un-reads one token (at most one at a time) - the preprocessor's
 * #define handling needs this to detect "end of macro body line" by
 * reading one token past it. */
void lexer_push_back(lexer_t *lx, token_t t);
/* `<...>` header-name syntax (#include <foo.h>) is lexically special in
 * C - not a normal token, since `<`/`.`/`>` would otherwise tokenize as
 * operators. Only valid to call right after seeing the `include`
 * identifier. Returns a TK_STR token with the raw header name. */
token_t lexer_next_angle_include(lexer_t *lx);

/* ---------------- preprocessor ---------------- */

/* Tokenizes `path`, expanding #include/#define/#pragma once as it goes.
 * Appends tokens (already macro-substituted) into `out`, growing *out_n.
 * `search_dir` is the including file's own directory, for quote-include
 * resolution (falls back to `-I` dirs on miss - see docs/toolchain.md). */
void pp_process_file(const char *path, token_t *out, int *out_n, int cap);

/* -I search paths, set up by main.c from argv. */
#define MAX_INCLUDE_DIRS 8
extern const char *g_include_dirs[MAX_INCLUDE_DIRS];
extern int g_n_include_dirs;

/* ---------------- types ---------------- */

typedef enum {
    TY_VOID, TY_CHAR, TY_SHORT, TY_INT, TY_PTR, TY_ARRAY,
    TY_STRUCT, TY_UNION, TY_ENUM, TY_FUNC,
} type_kind_t;

typedef struct type type_t;
typedef struct member member_t;

struct member {
    char name[MAX_NAME];
    type_t *type;
    int offset;
    member_t *next;
};

struct type {
    type_kind_t kind;
    int is_unsigned;
    type_t *base;         /* pointee (TY_PTR), element (TY_ARRAY), return type (TY_FUNC) */
    int array_len;          /* TY_ARRAY, -1 if unspecified (`T[]` param) */
    member_t *members;        /* TY_STRUCT/TY_UNION */
    int is_packed;              /* TY_STRUCT/TY_UNION __attribute__((packed)) */
    char tag[MAX_NAME];          /* struct/union/enum tag, "" if anonymous */
    type_t *param;                 /* TY_FUNC: linked list of param types via ->next */
    type_t *next;
    int size;
    int align;
};

int type_size(type_t *t);
int type_align(type_t *t);
type_t *type_new(type_kind_t kind);
type_t *ptr_to(type_t *base);
int type_is_int(type_t *t); /* char/short/int/enum - arithmetic-capable, non-pointer */

/* ---------------- AST ---------------- */

typedef enum {
    ND_NUM, ND_STR, ND_VAR, ND_ASSIGN, ND_COMPOUND_ASSIGN, ND_BINOP, ND_LOGAND, ND_LOGOR,
    ND_UNARY_MINUS, ND_NOT, ND_BITNOT, ND_ADDR, ND_DEREF,
    ND_PREINC, ND_PREDEC, ND_POSTINC, ND_POSTDEC,
    ND_CALL, ND_INDEX, ND_MEMBER, ND_ARROW, ND_CAST, ND_SIZEOF, ND_TERNARY, ND_COMMA,
    ND_IF, ND_WHILE, ND_DOWHILE, ND_FOR, ND_RETURN, ND_BREAK, ND_CONTINUE,
    ND_SWITCH, ND_CASE, ND_DEFAULT,
    ND_BLOCK, ND_EXPR_STMT, ND_NOP,
    ND_FUNC_DEF, ND_VAR_DECL,
} node_kind_t;

typedef struct node node_t;
struct node {
    node_kind_t kind;
    type_t *type;      /* result type, filled in during parsing/type-checking */
    tok_kind_t op;        /* ND_BINOP / ND_COMPOUND_ASSIGN */

    int64_t num;
    char str[512];        /* ND_STR: decoded bytes; also used for the emitted label once assigned */
    char name[MAX_NAME];    /* ND_VAR / ND_CALL / ND_MEMBER / ND_ARROW / func/var decl name */

    node_t *a, *b, *c, *d; /* kind-specific children */
    node_t *list;            /* block statements / call args / decl init-list, via ->next */
    node_t *next;

    /* variable/function storage location, resolved during parsing */
    int is_global;
    int is_static_local;     /* function-local `static` - lives in .data/.bss, own label */
    int is_file_static;      /* file-scope `static T name;` - still needs storage (is_global
                               * stays 1 for that purpose) but must not get a `global` directive:
                               * two files each with their own file-private `static int foo;`
                               * would otherwise collide as duplicate global symbols at link time. */
    int frame_offset;          /* local/param: EBP-relative byte offset */
    char asm_label[MAX_NAME];   /* global / static-local: the label CC emits for it */
    int is_indirect_call;         /* ND_CALL: callee is a function-pointer variable
                                    * (the fields above locate it), not a directly
                                    * callable function symbol - `call eax` instead
                                    * of `call name` */

    node_t *init;      /* ND_VAR_DECL: initializer expression or {..} list (via ->list), or NULL */
    int is_extern_decl;
};

node_t *node_new(node_kind_t kind);

/* ---------------- parser ---------------- */

/* Parses the whole (already-preprocessed) token stream into a list of
 * top-level ND_FUNC_DEF / ND_VAR_DECL nodes (via ->next). */
node_t *cc_parse(token_t *toks, int n_toks);

/* ---------------- codegen ---------------- */

typedef enum { TARGET_KERNEL, TARGET_HOST } cc_target_t;

/* Emits AS-syntax assembly text for the whole translation unit to `out`
 * (a FILE*, opaque here to keep cc.h freestanding-includable elsewhere
 * if ever needed - codegen.c does the actual #include <stdio.h>). */
void codegen_emit(node_t *prog, void *out_file, cc_target_t target);
