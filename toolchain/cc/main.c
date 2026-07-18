#include "cc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *inpath = NULL;
    const char *outpath = NULL;
    cc_target_t target = TARGET_KERNEL;
    int parse_only = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) { outpath = argv[++i]; }
        else if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) {
            if (g_n_include_dirs < MAX_INCLUDE_DIRS) g_include_dirs[g_n_include_dirs++] = argv[++i];
        }
        else if (strncmp(argv[i], "-I", 2) == 0 && argv[i][2] != '\0') {
            /* gcc-compatible combined form (-Ikernel, no space) - the
             * real Makefile's CFLAGS uses exactly this form. */
            if (g_n_include_dirs < MAX_INCLUDE_DIRS) g_include_dirs[g_n_include_dirs++] = argv[i] + 2;
        }
        else if (strncmp(argv[i], "--target=", 9) == 0) {
            target = strcmp(argv[i] + 9, "host") == 0 ? TARGET_HOST : TARGET_KERNEL;
        }
        else if (strcmp(argv[i], "--parse-only") == 0) { parse_only = 1; }
        else if (strcmp(argv[i], "-c") == 0) { /* implied: one file in, one .S out - accepted, no-op */ }
        else if (argv[i][0] == '-') {
            /* Tolerate the rest of a real compiler invocation's flags
             * (-Wall, -O2, -std=c11, -m32, -ffreestanding, ...) rather
             * than erroring or (worse) mistaking one for the input
             * file - the Makefile passes these unchanged either way,
             * since CC is meant to be a drop-in gcc replacement for
             * this project's exact build commands (see docs/toolchain.md). */
        }
        else { inpath = argv[i]; }
    }
    if (!inpath) {
        fprintf(stderr, "usage: cc [-I dir]... [--target=kernel|host] -o <out.S> <in.c>\n");
        return 1;
    }

    static token_t toks[MAX_TOKENS];
    int n_toks = 0;
    pp_process_file(inpath, toks, &n_toks, MAX_TOKENS);

    node_t *prog = cc_parse(toks, n_toks);

    if (parse_only) {
        printf("cc: parsed %s OK\n", inpath);
        return 0;
    }

    if (!outpath) {
        fprintf(stderr, "cc: -o <out.S> required\n");
        return 1;
    }
    FILE *out = fopen(outpath, "w");
    if (!out) { perror("cc: fopen"); return 1; }
    codegen_emit(prog, out, target);
    fclose(out);
    return 0;
}
