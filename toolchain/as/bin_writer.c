#include "as.h"

#include <stdio.h>

/* -f bin: our source files needing this mode never declare `section`, so
 * everything lands in the default SEC_TEXT bucket - just dump it. */
int write_bin(asm_ctx_t *ctx, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror("as: fopen"); return -1; }
    size_t n = fwrite(ctx->sec_bytes[SEC_TEXT], 1, ctx->sec_len[SEC_TEXT], f);
    fclose(f);
    return n == ctx->sec_len[SEC_TEXT] ? 0 : -1;
}
