#pragma once
/* LD: linker. See docs/toolchain.md for exactly what's supported and
 * why. Native host tool (build infrastructure), not OS code.
 *
 * Two output modes, both sharing the same merge/resolve/relocate core
 * (main.c): `link_flat` (kernel/link.ld's single-load-address layout -
 * .text/.rodata/.data/.bss concatenated at a fixed base; can write
 * either a flat binary the bootloader reads directly, or an ELF for
 * inspection - see write_flat_bin/write_flat_elf) and `link_static_exec`
 * (a minimal static ELF32 executable the Linux kernel can run directly,
 * for host tools - one PT_LOAD segment, no dynamic linking). Neither
 * mode implements a general linker-script language: `link_flat`'s layout
 * is hardcoded to match kernel/link.ld exactly (see link_flat.c), since
 * that is the only layout this project's kernel ever needs. There is no
 * separate OBJCOPY tool: link_flat already has the loadable bytes laid
 * out in memory before write_flat_elf wraps them in ELF headers, so
 * write_flat_bin just writes that same layout straight to a file. */

#include <stdint.h>
#include <stddef.h>

static inline void safe_strcpy(char *dst, const char *src, size_t dstsize) {
    size_t i = 0;
    for (; i + 1 < dstsize && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

#define MAX_INPUT_FILES 32
#define MAX_SECTIONS_PER_FILE 16
#define MAX_SYMS_PER_FILE 512
#define MAX_RELOCS_PER_FILE 512
#define MAX_NAME 128

typedef enum { SEC_TEXT, SEC_RODATA, SEC_DATA, SEC_BSS, SEC_KIND_COUNT, SEC_KIND_OTHER } sec_kind_t;

typedef struct {
    char name[MAX_NAME];
    sec_kind_t kind;    /* classified from the section name; SEC_KIND_OTHER is skipped (e.g. .symtab) */
    int is_nobits;         /* .bss: no file content, size still counts */
    uint32_t file_off;       /* offset of this section's bytes within in_file_t.data (PROGBITS only) */
    uint32_t size;
    uint32_t merged_off;      /* filled in during layout: offset within the OUTPUT section of this kind */
} in_section_t;

typedef struct {
    char name[MAX_NAME];
    int defined;          /* 0 = extern/undefined in this file */
    int is_global;
    int section_idx;         /* index into this file's sections[], -1 if undefined */
    uint32_t value;             /* offset within that section, as given by the object file */
} in_symbol_t;

typedef struct {
    int section_idx;    /* which of this file's sections the relocation patches */
    uint32_t offset;       /* byte offset within that section */
    int symbol_idx;          /* which of this file's symbols is referenced */
    int type;                  /* R_386_32 or R_386_PC32 */
} in_reloc_t;

typedef struct {
    char filename[256];
    uint8_t *data;
    uint32_t data_len;

    in_section_t sections[MAX_SECTIONS_PER_FILE];
    int n_sections;
    in_symbol_t syms[MAX_SYMS_PER_FILE];
    int n_syms;
    in_reloc_t relocs[MAX_RELOCS_PER_FILE];
    int n_relocs;
} in_file_t;

/* Global (whole-link) symbol table entry - one per unique defined name
 * across all input files, with its final linked address filled in once
 * layout is decided. */
typedef struct {
    char name[MAX_NAME];
    uint32_t addr;
    int defined;
} out_symbol_t;

#define MAX_OUT_SYMS 2048

typedef struct {
    in_file_t files[MAX_INPUT_FILES];
    int n_files;

    out_symbol_t out_syms[MAX_OUT_SYMS];
    int n_out_syms;

    /* Merged section byte buffers - PROGBITS content only; .bss has no
     * bytes here, just a size (merged_size[SEC_BSS]). */
    uint8_t *merged_data[SEC_KIND_COUNT];
    uint32_t merged_size[SEC_KIND_COUNT];
    uint32_t merged_addr[SEC_KIND_COUNT]; /* filled in during layout */
} link_ctx_t;

/* elf_reader.c */
int read_object(const char *path, in_file_t *out);

/* main.c: shared merge/resolve/relocate core, used by both output modes */
out_symbol_t *find_out_sym(link_ctx_t *ctx, const char *name);

/* link_flat.c / link_static_exec.c: each mode exposes an address-
 * assignment step (called before symbol resolution/relocation in
 * main.c, since both need final addresses) and a writer (called after). */
void flat_assign_addresses(link_ctx_t *ctx, uint32_t base);
int write_flat_elf(link_ctx_t *ctx, const char *path, uint32_t base);
int write_flat_bin(link_ctx_t *ctx, const char *path);

void static_exec_assign_addresses(link_ctx_t *ctx);
int write_static_exec_elf(link_ctx_t *ctx, const char *path);
