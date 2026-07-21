#include "as.h"
#include "elf_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *SEC_NAMES[MAX_SECTIONS] = { ".text", ".data", ".bss", ".rodata" };
static const int SEC_IS_BSS[MAX_SECTIONS] = { 0, 0, 1, 0 };
static const int SEC_IS_EXEC[MAX_SECTIONS] = { 1, 0, 0, 0 };

typedef struct {
    char buf[1 << 20];
    uint32_t len;
} strtab_builder_t;

static uint32_t strtab_add(strtab_builder_t *st, const char *s) {
    if (st->len == 0) { st->buf[0] = '\0'; st->len = 1; } /* index 0 = empty string, ELF convention */
    uint32_t off = st->len;
    size_t n = strlen(s) + 1;
    memcpy(st->buf + st->len, s, n);
    st->len += (uint32_t)n;
    return off;
}

int write_elf32(asm_ctx_t *ctx, const char *path) {
    /* --- decide which content sections exist --- */
    int content_secs[MAX_SECTIONS];
    int n_content = 0;
    for (int s = 0; s < MAX_SECTIONS; s++) {
        if (ctx->sec_used[s]) content_secs[n_content++] = s;
    }

    int reloc_for[MAX_SECTIONS]; /* does section s have any relocations? */
    for (int s = 0; s < MAX_SECTIONS; s++) reloc_for[s] = 0;
    for (int i = 0; i < ctx->nrelocs; i++) reloc_for[ctx->relocs[i].section] = 1;

    /* --- section header string table + index plan ---
     * 0=NULL, 1..n_content=content, then one .rel.X per content section
     * with relocs, then .symtab, .strtab, .shstrtab. Computed before the
     * symbol table below, which needs shidx_content[] to translate our
     * internal section_kind_t into the real ELF section header index -
     * they're NOT the same number, since only sections actually used
     * appear in the file (e.g. a .bss-only object has no .data, so .bss
     * isn't at its "usual" index). */
    static strtab_builder_t shstrtab;
    memset(&shstrtab, 0, sizeof(shstrtab));

    int shidx_content[MAX_SECTIONS];
    for (int i = 0; i < MAX_SECTIONS; i++) { shidx_content[i] = -1; }

    int nsh = 1;
    uint32_t name_content[MAX_SECTIONS], name_rel[MAX_SECTIONS];
    for (int i = 0; i < n_content; i++) {
        int s = content_secs[i];
        shidx_content[s] = nsh++;
        name_content[s] = strtab_add(&shstrtab, SEC_NAMES[s]);
    }
    for (int i = 0; i < n_content; i++) {
        int s = content_secs[i];
        if (reloc_for[s]) {
            char nm[MAX_NAME];
            snprintf(nm, sizeof(nm), ".rel%s", SEC_NAMES[s]);
            nsh++;
            name_rel[s] = strtab_add(&shstrtab, nm);
        }
    }
    int shidx_symtab = nsh++;
    uint32_t name_symtab = strtab_add(&shstrtab, ".symtab");
    int shidx_strtab = nsh++;
    uint32_t name_strtab = strtab_add(&shstrtab, ".strtab");
    int shidx_shstrtab = nsh++;
    uint32_t name_shstrtab = strtab_add(&shstrtab, ".shstrtab");

    /* --- symbol table: locals first (ELF requirement), then globals --- */
    static Elf32_Sym symtab[MAX_SYMS + 1];
    int nsym = 0;
    static strtab_builder_t strtab;
    memset(&strtab, 0, sizeof(strtab));
    static int sym_elf_index[MAX_SYMS];

    /* Not a compound literal (CC doesn't support them - see
     * docs/toolchain.md, Phase T3/T8): symtab is `static`, but a prior
     * call in the same process could have left index 0 non-zero, so
     * this can't just rely on static zero-init either. */
    memset(&symtab[nsym], 0, sizeof(symtab[nsym]));
    nsym++; /* index 0: null symbol */

    for (int pass = 0; pass < 2; pass++) { /* pass 0 = locals, pass 1 = globals */
        for (int i = 0; i < ctx->nsyms; i++) {
            symbol_t *s = &ctx->syms[i];
            if (s->is_equ) continue;
            if (!s->defined && !s->is_extern) continue;
            int is_glob = s->is_global || s->is_extern;
            if ((pass == 0 && is_glob) || (pass == 1 && !is_glob)) continue;

            Elf32_Sym sym = {0};
            sym.st_name = strtab_add(&strtab, s->name);
            sym.st_info = (unsigned char)elf32_st_info(is_glob ? STB_GLOBAL : STB_LOCAL, STT_NOTYPE);
            if (s->defined) {
                sym.st_shndx = (Elf32_Half)shidx_content[s->section];
                sym.st_value = s->offset;
            } else {
                sym.st_shndx = SHN_UNDEF;
            }
            sym_elf_index[i] = nsym;
            symtab[nsym++] = sym;
        }
    }
    int first_global = 1;
    for (int i = 1; i < nsym; i++) {
        if ((symtab[i].st_info >> 4) == STB_LOCAL) first_global = i + 1;
    }

    /* --- relocation table entries, grouped by target section --- */
    static Elf32_Rel rels[MAX_SECTIONS][MAX_RELOCS];
    int nrels[MAX_SECTIONS] = {0};
    for (int i = 0; i < ctx->nrelocs; i++) {
        reloc_t *r = &ctx->relocs[i];
        symbol_t *s = sym_find(ctx, r->sym);
        if (!s) { fprintf(stderr, "as: relocation against unknown symbol '%s'\n", r->sym); return -1; }
        int symidx = sym_elf_index[s - ctx->syms];
        int type = r->kind == RELOC_PC32 ? R_386_PC32 : R_386_32;
        int sec = r->section;
        rels[sec][nrels[sec]].r_offset = r->offset;
        rels[sec][nrels[sec]].r_info = elf32_r_info(symidx, type);
        nrels[sec]++;
    }

    /* --- lay out the file: Ehdr, content sections' bytes, rel tables,
     * symtab, strtab, shstrtab, then the section header table --- */
    uint32_t off = sizeof(Elf32_Ehdr);
    uint32_t off_content[MAX_SECTIONS];
    for (int i = 0; i < n_content; i++) {
        int s = content_secs[i];
        off_content[s] = off;
        if (!SEC_IS_BSS[s]) off += ctx->sec_len[s];
    }
    uint32_t off_rel[MAX_SECTIONS];
    for (int i = 0; i < n_content; i++) {
        int s = content_secs[i];
        if (reloc_for[s]) { off_rel[s] = off; off += (uint32_t)nrels[s] * sizeof(Elf32_Rel); }
    }
    uint32_t off_symtab = off; off += (uint32_t)nsym * sizeof(Elf32_Sym);
    uint32_t off_strtab = off; off += strtab.len;
    uint32_t off_shstrtab = off; off += shstrtab.len;
    uint32_t off_shdrs = (off + 3) & ~3u;

    FILE *f = fopen(path, "wb");
    if (!f) { perror("as: fopen"); return -1; }

    Elf32_Ehdr eh = {0};
    eh.e_ident[0] = ELFMAG0; eh.e_ident[1] = ELFMAG1; eh.e_ident[2] = ELFMAG2; eh.e_ident[3] = ELFMAG3;
    eh.e_ident[4] = ELFCLASS32; eh.e_ident[5] = ELFDATA2LSB; eh.e_ident[6] = EV_CURRENT;
    eh.e_ident[7] = ELFOSABI_SYSV;
    eh.e_type = ET_REL;
    eh.e_machine = EM_386;
    eh.e_version = EV_CURRENT;
    eh.e_shoff = off_shdrs;
    eh.e_ehsize = sizeof(Elf32_Ehdr);
    eh.e_shentsize = sizeof(Elf32_Shdr);
    eh.e_shnum = (Elf32_Half)nsh;
    eh.e_shstrndx = (Elf32_Half)shidx_shstrtab;
    fwrite(&eh, sizeof(eh), 1, f);

    for (int i = 0; i < n_content; i++) {
        int s = content_secs[i];
        if (!SEC_IS_BSS[s]) fwrite(ctx->sec_bytes[s], 1, ctx->sec_len[s], f);
    }
    for (int i = 0; i < n_content; i++) {
        int s = content_secs[i];
        if (reloc_for[s]) fwrite(rels[s], sizeof(Elf32_Rel), (size_t)nrels[s], f);
    }
    fwrite(symtab, sizeof(Elf32_Sym), (size_t)nsym, f);
    fwrite(strtab.buf, 1, strtab.len, f);
    fwrite(shstrtab.buf, 1, shstrtab.len, f);

    while (ftell(f) < (long)off_shdrs) fputc(0, f);

    Elf32_Shdr sh;
    memset(&sh, 0, sizeof(sh));
    fwrite(&sh, sizeof(sh), 1, f); /* NULL section */

    for (int i = 0; i < n_content; i++) {
        int s = content_secs[i];
        memset(&sh, 0, sizeof(sh));
        sh.sh_name = name_content[s];
        sh.sh_type = SEC_IS_BSS[s] ? SHT_NOBITS : SHT_PROGBITS;
        sh.sh_flags = SHF_ALLOC | (SEC_IS_EXEC[s] ? SHF_EXECINSTR : 0) | (s == SEC_TEXT ? 0 : SHF_WRITE);
        sh.sh_offset = off_content[s];
        sh.sh_size = ctx->sec_len[s];
        sh.sh_addralign = 4;
        fwrite(&sh, sizeof(sh), 1, f);
    }
    for (int i = 0; i < n_content; i++) {
        int s = content_secs[i];
        if (!reloc_for[s]) continue;
        memset(&sh, 0, sizeof(sh));
        sh.sh_name = name_rel[s];
        sh.sh_type = SHT_REL;
        sh.sh_offset = off_rel[s];
        sh.sh_size = (uint32_t)nrels[s] * sizeof(Elf32_Rel);
        sh.sh_link = (Elf32_Word)shidx_symtab;
        sh.sh_info = (Elf32_Word)shidx_content[s];
        sh.sh_addralign = 4;
        sh.sh_entsize = sizeof(Elf32_Rel);
        fwrite(&sh, sizeof(sh), 1, f);
    }
    memset(&sh, 0, sizeof(sh));
    sh.sh_name = name_symtab;
    sh.sh_type = SHT_SYMTAB;
    sh.sh_offset = off_symtab;
    sh.sh_size = (uint32_t)nsym * sizeof(Elf32_Sym);
    sh.sh_link = (Elf32_Word)shidx_strtab;
    sh.sh_info = (Elf32_Word)first_global;
    sh.sh_addralign = 4;
    sh.sh_entsize = sizeof(Elf32_Sym);
    fwrite(&sh, sizeof(sh), 1, f);

    memset(&sh, 0, sizeof(sh));
    sh.sh_name = name_strtab;
    sh.sh_type = SHT_STRTAB;
    sh.sh_offset = off_strtab;
    sh.sh_size = strtab.len;
    sh.sh_addralign = 1;
    fwrite(&sh, sizeof(sh), 1, f);

    memset(&sh, 0, sizeof(sh));
    sh.sh_name = name_shstrtab;
    sh.sh_type = SHT_STRTAB;
    sh.sh_offset = off_shstrtab;
    sh.sh_size = shstrtab.len;
    sh.sh_addralign = 1;
    fwrite(&sh, sizeof(sh), 1, f);

    fclose(f);
    return 0;
}
