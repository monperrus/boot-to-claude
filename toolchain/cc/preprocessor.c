#include "cc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *g_include_dirs[MAX_INCLUDE_DIRS];
int g_n_include_dirs = 0;

#define MAX_MACROS 128
#define MAX_MACRO_BODY 32
#define MAX_PRAGMA_ONCE 128

typedef struct {
    char name[MAX_NAME];
    token_t body[MAX_MACRO_BODY];
    int n;
} macro_t;

static macro_t g_macros[MAX_MACROS];
static int g_n_macros = 0;

static char g_pragma_once_files[MAX_PRAGMA_ONCE][512];
static int g_n_pragma_once = 0;

static macro_t *macro_find(const char *name) {
    for (int i = 0; i < g_n_macros; i++) {
        if (strcmp(g_macros[i].name, name) == 0) return &g_macros[i];
    }
    return NULL;
}

static int already_seen(const char *path) {
    for (int i = 0; i < g_n_pragma_once; i++) {
        if (strcmp(g_pragma_once_files[i], path) == 0) return 1;
    }
    return 0;
}

static void mark_seen(const char *path) {
    if (g_n_pragma_once < MAX_PRAGMA_ONCE) {
        safe_strcpy(g_pragma_once_files[g_n_pragma_once++], path, 512);
    }
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static void dirname_of(const char *path, char *out, size_t outsz) {
    const char *slash = strrchr(path, '/');
    if (!slash) { safe_strcpy(out, ".", outsz); return; }
    size_t n = (size_t)(slash - path);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}

static void join_path(char *dst, size_t dstsz, const char *dir, const char *name) {
    size_t i = 0;
    for (; i + 1 < dstsz && dir[i]; i++) dst[i] = dir[i];
    if (i + 1 < dstsz) dst[i++] = '/';
    for (size_t j = 0; i + 1 < dstsz && name[j]; i++, j++) dst[i] = name[j];
    dst[i] = '\0';
}

static int resolve_include(const char *cur_dir, const char *name, int is_quote, char *out, size_t outsz) {
    char cand[600];
    if (is_quote) {
        join_path(cand, sizeof(cand), cur_dir, name);
        FILE *f = fopen(cand, "rb");
        if (f) { fclose(f); safe_strcpy(out, cand, outsz); return 1; }
    }
    for (int i = 0; i < g_n_include_dirs; i++) {
        join_path(cand, sizeof(cand), g_include_dirs[i], name);
        FILE *f = fopen(cand, "rb");
        if (f) { fclose(f); safe_strcpy(out, cand, outsz); return 1; }
    }
    return 0;
}

static void emit(token_t *out, int *out_n, int cap, token_t t) {
    if (*out_n >= cap) { fprintf(stderr, "cc: token buffer full\n"); exit(1); }
    out[(*out_n)++] = t;
}

static void process_lexer(lexer_t *lx, const char *filename, token_t *out, int *out_n, int cap, int depth) {
    char cur_dir[512];
    dirname_of(filename, cur_dir, sizeof(cur_dir));

    for (;;) {
        token_t t = lexer_next(lx);
        if (t.kind == TK_EOF) return;

        if (t.kind == TK_HASH) {
            token_t dir = lexer_next(lx);
            if (dir.kind != TK_IDENT) { fprintf(stderr, "cc: %s:%d: expected preprocessor directive\n", filename, dir.line); exit(1); }

            if (strcmp(dir.text, "include") == 0) {
                size_t peek = lx->pos;
                while (peek < lx->len && (lx->src[peek] == ' ' || lx->src[peek] == '\t')) peek++;
                int is_quote = peek < lx->len && lx->src[peek] == '"';
                token_t path_tok = is_quote ? lexer_next(lx) : lexer_next_angle_include(lx);
                if (is_quote && path_tok.kind != TK_STR) { fprintf(stderr, "cc: %s:%d: expected \"file\" after #include\n", filename, dir.line); exit(1); }
                char resolved[600];
                if (!resolve_include(cur_dir, path_tok.text, is_quote, resolved, sizeof(resolved))) {
                    fprintf(stderr, "cc: %s:%d: cannot find include file '%s'\n", filename, dir.line, path_tok.text);
                    exit(1);
                }
                if (!already_seen(resolved)) {
                    if (depth >= MAX_INCLUDE_DEPTH) { fprintf(stderr, "cc: #include nested too deep\n"); exit(1); }
                    char *src = read_file(resolved);
                    if (!src) { fprintf(stderr, "cc: cannot open %s\n", resolved); exit(1); }
                    static char paths[MAX_INCLUDE_DEPTH][600];
                    safe_strcpy(paths[depth], resolved, sizeof(paths[depth]));
                    lexer_t sub;
                    lexer_init(&sub, src, paths[depth]);
                    process_lexer(&sub, paths[depth], out, out_n, cap, depth + 1);
                }
                continue;
            }
            if (strcmp(dir.text, "pragma") == 0) {
                token_t what = lexer_next(lx);
                if (what.kind == TK_IDENT && strcmp(what.text, "once") == 0) {
                    mark_seen(filename);
                } else {
                    fprintf(stderr, "cc: %s:%d: unsupported #pragma\n", filename, dir.line);
                    exit(1);
                }
                continue;
            }
            if (strcmp(dir.text, "define") == 0) {
                token_t name = lexer_next(lx);
                if (name.kind != TK_IDENT) { fprintf(stderr, "cc: %s:%d: expected macro name\n", filename, dir.line); exit(1); }
                if (g_n_macros >= MAX_MACROS) { fprintf(stderr, "cc: too many macros\n"); exit(1); }
                macro_t *m = &g_macros[g_n_macros++];
                safe_strcpy(m->name, name.text, MAX_NAME);
                m->n = 0;
                for (;;) {
                    token_t bt = lexer_next(lx);
                    if (bt.bol || bt.kind == TK_EOF) {
                        /* First token of the next line (or EOF) - this
                         * lexer has no newline tokens, so `bol` marks
                         * the boundary. It belongs to the main stream,
                         * not the macro body, so push it back rather
                         * than discarding it. */
                        lexer_push_back(lx, bt);
                        break;
                    }
                    if (m->n < MAX_MACRO_BODY) m->body[m->n++] = bt;
                }
                continue;
            }
            fprintf(stderr, "cc: %s:%d: unsupported preprocessor directive #%s\n", filename, dir.line, dir.text);
            exit(1);
        }

        if (t.kind == TK_IDENT) {
            macro_t *m = macro_find(t.text);
            if (m) {
                for (int i = 0; i < m->n; i++) {
                    token_t bt = m->body[i];
                    if (bt.kind == TK_IDENT) {
                        macro_t *nested = macro_find(bt.text);
                        if (nested && nested != m) {
                            for (int j = 0; j < nested->n; j++) emit(out, out_n, cap, nested->body[j]);
                            continue;
                        }
                    }
                    emit(out, out_n, cap, bt);
                }
                continue;
            }
        }

        emit(out, out_n, cap, t);
    }
}

void pp_process_file(const char *path, token_t *out, int *out_n, int cap) {
    char *src = read_file(path);
    if (!src) { fprintf(stderr, "cc: cannot open %s\n", path); exit(1); }
    lexer_t lx;
    lexer_init(&lx, src, path);
    mark_seen(path);
    process_lexer(&lx, path, out, out_n, cap, 0);
    token_t eof;
    memset(&eof, 0, sizeof(eof));
    eof.kind = TK_EOF;
    emit(out, out_n, cap, eof);
}
