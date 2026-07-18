#include "ld.h"
#include "elf_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

out_symbol_t *find_out_sym(link_ctx_t *ctx, const char *name) {
    for (int i = 0; i < ctx->n_out_syms; i++) {
        if (strcmp(ctx->out_syms[i].name, name) == 0) return &ctx->out_syms[i];
    }
    return NULL;
}

/* Concatenates every input file's sections of each kind, in file order
 * (the only "layout policy" this linker has - see docs/toolchain.md on
 * why that's enough to match kernel/link.ld's `*(.text.entry) *(.text*)`
 * behavior as long as entry.o is passed first, which the Makefile
 * already does). Fills in_section_t.merged_off and the merged byte
 * buffers; addresses are assigned afterward, per output mode. */
static void merge_sections(link_ctx_t *ctx) {
    uint32_t cursor[SEC_KIND_COUNT] = {0};
    for (int fi = 0; fi < ctx->n_files; fi++) {
        in_file_t *file = &ctx->files[fi];
        for (int si = 0; si < file->n_sections; si++) {
            in_section_t *s = &file->sections[si];
            if (s->kind == SEC_KIND_OTHER) continue;
            s->merged_off = cursor[s->kind];
            cursor[s->kind] += s->size;
        }
    }
    for (int k = 0; k < SEC_KIND_COUNT; k++) {
        ctx->merged_size[k] = cursor[k];
        if (k != SEC_BSS && cursor[k] > 0) {
            ctx->merged_data[k] = calloc(1, cursor[k]);
        }
    }
    for (int fi = 0; fi < ctx->n_files; fi++) {
        in_file_t *file = &ctx->files[fi];
        for (int si = 0; si < file->n_sections; si++) {
            in_section_t *s = &file->sections[si];
            if (s->kind == SEC_KIND_OTHER || s->kind == SEC_BSS) continue;
            memcpy(ctx->merged_data[s->kind] + s->merged_off, file->data + s->file_off, s->size);
        }
    }
}

static void build_global_symbol_table(link_ctx_t *ctx) {
    for (int fi = 0; fi < ctx->n_files; fi++) {
        in_file_t *file = &ctx->files[fi];
        for (int si = 0; si < file->n_syms; si++) {
            in_symbol_t *sym = &file->syms[si];
            if (!sym->defined || !sym->is_global) continue;
            in_section_t *sec = &file->sections[sym->section_idx];
            if (sec->kind == SEC_KIND_OTHER) continue;

            if (find_out_sym(ctx, sym->name)) {
                fprintf(stderr, "ld: duplicate symbol '%s' (from %s)\n", sym->name, file->filename);
                exit(1);
            }
            if (ctx->n_out_syms >= MAX_OUT_SYMS) { fprintf(stderr, "ld: too many symbols\n"); exit(1); }
            out_symbol_t *out = &ctx->out_syms[ctx->n_out_syms++];
            safe_strcpy(out->name, sym->name, MAX_NAME);
            out->addr = ctx->merged_addr[sec->kind] + sec->merged_off + sym->value;
            out->defined = 1;
        }
    }
}

static uint32_t sym_final_addr(link_ctx_t *ctx, in_file_t *file, int sym_idx) {
    in_symbol_t *sym = &file->syms[sym_idx];
    if (sym->defined) {
        in_section_t *sec = &file->sections[sym->section_idx];
        return ctx->merged_addr[sec->kind] + sec->merged_off + sym->value;
    }
    out_symbol_t *g = find_out_sym(ctx, sym->name);
    if (!g) {
        fprintf(stderr, "ld: undefined reference to '%s' (from %s)\n", sym->name, file->filename);
        exit(1);
    }
    return g->addr;
}

/* x86 ELF uses REL (not RELA) relocations: AS already baked the addend
 * into the bytes at each relocated field (see docs/toolchain.md), so
 * applying one is just: read the addend back out, add S (R_386_32) or
 * S - P (R_386_PC32, P = the relocated field's own final address), and
 * write the result back over the same bytes. */
static void apply_relocations(link_ctx_t *ctx) {
    for (int fi = 0; fi < ctx->n_files; fi++) {
        in_file_t *file = &ctx->files[fi];
        for (int ri = 0; ri < file->n_relocs; ri++) {
            in_reloc_t *r = &file->relocs[ri];
            in_section_t *target = &file->sections[r->section_idx];
            if (target->kind == SEC_KIND_OTHER) continue;

            uint32_t S = sym_final_addr(ctx, file, r->symbol_idx);
            uint32_t P = ctx->merged_addr[target->kind] + target->merged_off + r->offset;
            uint8_t *field = ctx->merged_data[target->kind] + target->merged_off + r->offset;

            uint32_t A;
            memcpy(&A, field, 4);
            uint32_t value = (r->type == R_386_PC32) ? (S + A - P) : (S + A);
            memcpy(field, &value, 4);
        }
    }
}

int main(int argc, char **argv) {
    const char *outpath = NULL;
    const char *inputs[MAX_INPUT_FILES];
    int n_inputs = 0;
    int flat_mode = 0;
    uint32_t flat_base = 0x10000;
    int flat_format_bin = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) { outpath = argv[++i]; }
        else if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
            /* Linker-script *presence* is the flat-mode switch; the
             * script's own contents are never read - link_flat.c
             * hardcodes the one layout this project's kernel needs
             * (see docs/toolchain.md). */
            flat_mode = 1;
            i++;
        }
        else if (strcmp(argv[i], "--flat-base") == 0 && i + 1 < argc) {
            flat_base = (uint32_t)strtoul(argv[++i], NULL, 0);
        }
        else if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
            /* flat mode only - write the loadable bytes directly instead
             * of wrapping them in an ELF (no separate OBJCOPY step, see
             * ld.h/docs/toolchain.md). Default stays ELF: still useful
             * to inspect with nm/objdump while debugging. */
            flat_format_bin = strcmp(argv[++i], "bin") == 0;
        }
        else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) { i++; /* -m elf_i386: accepted, always true */ }
        else if (strcmp(argv[i], "-nostdlib") == 0) { /* accepted, always true - no libc ever linked */ }
        else if (argv[i][0] == '-') { fprintf(stderr, "ld: unrecognized option '%s'\n", argv[i]); return 1; }
        else { if (n_inputs < MAX_INPUT_FILES) inputs[n_inputs++] = argv[i]; }
    }
    if (!outpath || n_inputs == 0) {
        fprintf(stderr, "usage: ld [-T <script>] [--flat-base 0xADDR] [--format elf|bin] -o <out> <in.o>...\n");
        return 1;
    }

    static link_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    for (int i = 0; i < n_inputs; i++) {
        if (read_object(inputs[i], &ctx.files[ctx.n_files]) != 0) return 1;
        ctx.n_files++;
    }

    merge_sections(&ctx);

    if (flat_mode) {
        flat_assign_addresses(&ctx, flat_base);
    } else {
        static_exec_assign_addresses(&ctx);
    }

    build_global_symbol_table(&ctx);
    apply_relocations(&ctx);

    int rc;
    if (flat_mode) {
        rc = flat_format_bin ? write_flat_bin(&ctx, outpath) : write_flat_elf(&ctx, outpath, flat_base);
    } else {
        rc = write_static_exec_elf(&ctx, outpath);
    }
    return rc == 0 ? 0 : 1;
}
