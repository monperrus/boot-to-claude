#pragma once
/* AS: NASM-subset assembler. See docs/toolchain.md for exactly what's
 * supported and why. Native host tool (build infrastructure), not OS
 * code - built with the bootstrap compiler in Phase T1/T7, and later
 * with CC itself in Phase T8. */

#include <stdint.h>
#include <stddef.h>

/* strncpy doesn't guarantee null-termination when src is >= dstsize;
 * this always does. */
static inline void safe_strcpy(char *dst, const char *src, size_t dstsize) {
    size_t i = 0;
    for (; i + 1 < dstsize && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

#define MAX_SYMS 512
#define MAX_STMTS 4096
#define MAX_RELOCS 256
#define MAX_LINE 512
#define MAX_NAME 64
#define MAX_SECTIONS 8

typedef enum { SEC_NONE = -1, SEC_TEXT = 0, SEC_DATA, SEC_BSS, SEC_RODATA } section_kind_t;

/* A "section" here is one of a small fixed set of NASM section names we
 * actually see (`.text`, `.text.entry`, `.data`, `.bss`), normalized to
 * section_kind_t - `.text` and `.text.entry` both map to SEC_TEXT
 * (entry.S relies on `.text.entry` sorting first via the linker script,
 * which is LD's concern, not AS's - AS just needs one bucket per byte
 * stream). */

typedef struct {
    char name[MAX_NAME];
    int defined;       /* 0 = only seen via `extern` so far */
    int is_extern;
    int is_global;
    int is_equ;
    int64_t equ_value;  /* valid if is_equ */
    section_kind_t section; /* valid if defined && !is_equ */
    uint32_t offset;         /* byte offset within its section, valid if defined && !is_equ */
} symbol_t;

/* Operand forms actually needed - see docs/toolchain.md. */
typedef enum {
    OPND_NONE,
    OPND_REG,
    OPND_IMM,   /* constant expression, possibly symbol-relative (see sym[]) */
    OPND_MEM,   /* [sym], [sym+disp], [reg], [reg+disp] */
    OPND_FAR,   /* SEG:OFFSET - only for far jmp; imm.reg-less seg value + sym offset */
} operand_kind_t;

typedef enum {
    R_NONE = 0,
    R_AL, R_CL, R_DL, R_BL, R_AH, R_CH, R_DH, R_BH,
    R_AX, R_CX, R_DX, R_BX, R_SP, R_BP, R_SI, R_DI,
    R_EAX, R_ECX, R_EDX, R_EBX, R_ESP, R_EBP, R_ESI, R_EDI,
    R_ES, R_CS, R_SS, R_DS, R_FS, R_GS,
    R_CR0,
    R_DX_PORT, /* `dx` used as an I/O port operand - same encoding as R_DX, kept distinct only for clarity at parse time */
} reg_t;

typedef struct {
    operand_kind_t kind;
    reg_t reg;         /* OPND_REG, or base register for OPND_MEM (R_NONE = no base, direct address) */
    int has_sym;         /* OPND_IMM/OPND_MEM/OPND_FAR: value is symbol-relative */
    char sym[MAX_NAME];
    int64_t disp;          /* constant part; for OPND_IMM with no symbol, this is the whole value */
    int size_override;      /* 0 = none, 1/2/4 = byte/word/dword */
    int64_t far_seg;         /* OPND_FAR only: segment selector constant part */
    int far_seg_has_sym;      /* OPND_FAR only: segment selector is symbol-relative (e.g. a forward-referenced EQU) */
    char far_seg_sym[MAX_NAME];
} operand_t;

typedef struct {
    char mnemonic[16];
    operand_t ops[2];
    int nops;
    section_kind_t section;
    uint32_t offset;   /* byte offset within section where this statement's bytes start */
    uint32_t length;    /* encoded length in bytes, fixed once computed in pass 1 */
    int bits;            /* 16 or 32 - effective mode when this statement was parsed */
    int line_no;
} stmt_t;

typedef enum { RELOC_PC32, RELOC_ABS32 } reloc_kind_t;

typedef struct {
    section_kind_t section;
    uint32_t offset;     /* where the 4-byte field to patch lives, within `section` */
    char sym[MAX_NAME];
    int32_t addend;
    reloc_kind_t kind;
} reloc_t;

typedef struct {
    symbol_t syms[MAX_SYMS];
    int nsyms;

    stmt_t stmts[MAX_STMTS];
    int nstmts;

    reloc_t relocs[MAX_RELOCS];
    int nrelocs;

    uint8_t sec_bytes[MAX_SECTIONS][65536];
    uint32_t sec_len[MAX_SECTIONS];
    int sec_used[MAX_SECTIONS]; /* has this section appeared in the source at all */

    uint32_t org;     /* -f bin only */
    int have_org;

    char last_global_label[MAX_NAME]; /* for `.local` label scoping */
} asm_ctx_t;

/* lexer.c */
typedef enum {
    TK_EOF, TK_NEWLINE, TK_IDENT, TK_NUMBER, TK_STRING,
    TK_LBRACK, TK_RBRACK, TK_COMMA, TK_COLON, TK_PLUS, TK_MINUS,
    TK_STAR, TK_SLASH, TK_SHL, TK_SHR, TK_LPAREN, TK_RPAREN, TK_DOLLAR, TK_DOLLARDOLLAR,
} tok_kind_t;

typedef struct {
    tok_kind_t kind;
    char text[MAX_LINE];
    int64_t number;
} token_t;

typedef struct {
    const char *src;
    size_t pos;
    size_t len;
} lexer_t;

void lexer_init(lexer_t *lx, const char *src);
token_t lexer_next(lexer_t *lx); /* stops at end of line -> TK_NEWLINE; ';' comments skipped */

/* parser.c: pass 1 - reads the whole source, populates ctx (symbols with
 * addresses, statement list with fixed lengths), handling directives
 * immediately as encountered. */
int as_parse_pass1(asm_ctx_t *ctx, const char *src, const char *filename);

/* encode.c: pass 2 - walks ctx->stmts again and emits real bytes into
 * ctx->sec_bytes[]/sec_len[], plus ctx->relocs[] for anything not
 * resolvable within the same section. */
int as_encode_pass2(asm_ctx_t *ctx);

/* Shared by pass 1 (length-only, strict=0: unresolved symbols read as 0,
 * no error, no relocations) and pass 2 (strict=1: unresolved local
 * symbols are a hard error; extern/cross-section symbols emit a
 * relocation instead). Always returns the encoded length; `out` (if
 * non-NULL) receives the bytes. */
int stmt_encode(asm_ctx_t *ctx, stmt_t *st, uint8_t *out, int strict);

/* symbol/expr helpers shared by parser.c and encode.c */
symbol_t *sym_find(asm_ctx_t *ctx, const char *name);
symbol_t *sym_get_or_create(asm_ctx_t *ctx, const char *name);

/* bin_writer.c / elf_writer.c */
int write_bin(asm_ctx_t *ctx, const char *path);
int write_elf32(asm_ctx_t *ctx, const char *path);
