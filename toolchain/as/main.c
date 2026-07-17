#include "as.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("as: fopen"); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { perror("as: fread"); exit(1); }
    buf[n] = '\0';
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    const char *fmt = NULL;
    const char *outpath = NULL;
    const char *inpath = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) { fmt = argv[++i]; }
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) { outpath = argv[++i]; }
        else { inpath = argv[i]; }
    }
    if (!fmt || !outpath || !inpath) {
        fprintf(stderr, "usage: as -f bin|elf32 -o <out> <in.S>\n");
        return 1;
    }

    char *src = read_file(inpath);

    static asm_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    if (as_parse_pass1(&ctx, src, inpath) != 0) {
        fprintf(stderr, "as: parse errors, not assembling %s\n", inpath);
        return 1;
    }
    if (as_encode_pass2(&ctx) != 0) {
        fprintf(stderr, "as: encode errors, not writing %s\n", outpath);
        return 1;
    }

    int rc;
    if (strcmp(fmt, "bin") == 0) {
        rc = write_bin(&ctx, outpath);
    } else if (strcmp(fmt, "elf32") == 0) {
        rc = write_elf32(&ctx, outpath);
    } else {
        fprintf(stderr, "as: unknown format '%s' (expected bin or elf32)\n", fmt);
        return 1;
    }
    return rc == 0 ? 0 : 1;
}
