#include "ld.h"
#include "elf_format.h"

#include <stdio.h>
#include <string.h>

static uint32_t align4(uint32_t x) { return (x + 3) & ~3u; }

/* Hardcodes exactly kernel/link.ld's layout - base address, section
 * order, and the _bss_start/_bss_end symbols entry.S relies on to zero
 * .bss (see docs/toolchain.md on why a general linker-script language
 * isn't implemented). */
void flat_assign_addresses(link_ctx_t *ctx, uint32_t base) {
    uint32_t addr = base;
    ctx->merged_addr[SEC_TEXT] = addr;
    addr = align4(addr + ctx->merged_size[SEC_TEXT]);
    ctx->merged_addr[SEC_RODATA] = addr;
    addr = align4(addr + ctx->merged_size[SEC_RODATA]);
    ctx->merged_addr[SEC_DATA] = addr;
    addr = align4(addr + ctx->merged_size[SEC_DATA]);
    ctx->merged_addr[SEC_BSS] = addr;
    uint32_t bss_end = addr + ctx->merged_size[SEC_BSS];

    out_symbol_t *s1 = &ctx->out_syms[ctx->n_out_syms++];
    safe_strcpy(s1->name, "_bss_start", MAX_NAME);
    s1->addr = ctx->merged_addr[SEC_BSS];
    s1->defined = 1;

    out_symbol_t *s2 = &ctx->out_syms[ctx->n_out_syms++];
    safe_strcpy(s2->name, "_bss_end", MAX_NAME);
    s2->addr = bss_end;
    s2->defined = 1;
}

typedef struct {
    char buf[1 << 16];
    uint32_t len;
} strtab_t;

static uint32_t strtab_add(strtab_t *st, const char *s) {
    if (st->len == 0) { st->buf[0] = '\0'; st->len = 1; }
    uint32_t off = st->len;
    size_t n = strlen(s) + 1;
    memcpy(st->buf + st->len, s, n);
    st->len += (uint32_t)n;
    return off;
}

static const char *SEC_NAMES[SEC_KIND_COUNT] = { ".text", ".rodata", ".data", ".bss" };

int write_flat_elf(link_ctx_t *ctx, const char *path, uint32_t base) {
    (void)base;

    static strtab_t strtab, shstrtab;
    memset(&strtab, 0, sizeof(strtab));
    memset(&shstrtab, 0, sizeof(shstrtab));

    /* section index plan: 0=NULL, 1..4=.text/.rodata/.data/.bss (only
     * those with nonzero size), then .symtab, .strtab, .shstrtab */
    int shidx[SEC_KIND_COUNT];
    int nsh = 1;
    uint32_t name_sec[SEC_KIND_COUNT];
    for (int k = 0; k < SEC_KIND_COUNT; k++) {
        shidx[k] = -1;
        if (ctx->merged_size[k] == 0) continue;
        shidx[k] = nsh++;
        name_sec[k] = strtab_add(&shstrtab, SEC_NAMES[k]);
    }
    nsh++; /* .symtab's own index isn't referenced by anything else here */
    uint32_t name_symtab = strtab_add(&shstrtab, ".symtab");
    int shidx_strtab = nsh++;
    uint32_t name_strtab = strtab_add(&shstrtab, ".strtab");
    int shidx_shstrtab = nsh++;
    uint32_t name_shstrtab = strtab_add(&shstrtab, ".shstrtab");

    static Elf32_Sym symtab[MAX_OUT_SYMS + 1];
    int nsym = 0;
    symtab[nsym++] = (Elf32_Sym){0};
    for (int i = 0; i < ctx->n_out_syms; i++) {
        out_symbol_t *s = &ctx->out_syms[i];
        Elf32_Sym sym = {0};
        sym.st_name = strtab_add(&strtab, s->name);
        sym.st_info = (unsigned char)ELF32_ST_INFO(STB_GLOBAL, STT_NOTYPE);
        sym.st_value = s->addr;
        sym.st_shndx = 1; /* not meaningful for our purposes (address-based, not section-relative) once linked; any nonzero value marks it defined */
        symtab[nsym++] = sym;
    }

    uint32_t off = sizeof(Elf32_Ehdr);
    uint32_t off_sec[SEC_KIND_COUNT];
    for (int k = 0; k < SEC_KIND_COUNT; k++) {
        if (shidx[k] < 0) continue;
        off_sec[k] = off;
        if (k != SEC_BSS) off += ctx->merged_size[k];
    }
    uint32_t off_symtab = off; off += (uint32_t)nsym * sizeof(Elf32_Sym);
    uint32_t off_strtab = off; off += strtab.len;
    uint32_t off_shstrtab = off; off += shstrtab.len;
    uint32_t off_shdrs = (off + 3) & ~3u;

    FILE *f = fopen(path, "wb");
    if (!f) { perror("ld: fopen"); return -1; }

    Elf32_Ehdr eh = {0};
    eh.e_ident[0] = ELFMAG0; eh.e_ident[1] = ELFMAG1; eh.e_ident[2] = ELFMAG2; eh.e_ident[3] = ELFMAG3;
    eh.e_ident[4] = ELFCLASS32; eh.e_ident[5] = ELFDATA2LSB; eh.e_ident[6] = EV_CURRENT;
    eh.e_ident[7] = ELFOSABI_SYSV;
    eh.e_type = ET_EXEC;
    eh.e_machine = EM_386;
    eh.e_version = EV_CURRENT;
    out_symbol_t *entry = find_out_sym(ctx, "_start");
    eh.e_entry = entry ? entry->addr : ctx->merged_addr[SEC_TEXT];
    eh.e_shoff = off_shdrs;
    eh.e_ehsize = sizeof(Elf32_Ehdr);
    eh.e_shentsize = sizeof(Elf32_Shdr);
    eh.e_shnum = (Elf32_Half)nsh;
    eh.e_shstrndx = (Elf32_Half)shidx_shstrtab;
    fwrite(&eh, sizeof(eh), 1, f);

    for (int k = 0; k < SEC_KIND_COUNT; k++) {
        if (shidx[k] < 0 || k == SEC_BSS) continue;
        fwrite(ctx->merged_data[k], 1, ctx->merged_size[k], f);
    }
    fwrite(symtab, sizeof(Elf32_Sym), (size_t)nsym, f);
    fwrite(strtab.buf, 1, strtab.len, f);
    fwrite(shstrtab.buf, 1, shstrtab.len, f);
    while ((uint32_t)ftell(f) < off_shdrs) fputc(0, f);

    Elf32_Shdr sh;
    memset(&sh, 0, sizeof(sh));
    fwrite(&sh, sizeof(sh), 1, f); /* NULL section */

    for (int k = 0; k < SEC_KIND_COUNT; k++) {
        if (shidx[k] < 0) continue;
        memset(&sh, 0, sizeof(sh));
        sh.sh_name = name_sec[k];
        sh.sh_type = (k == SEC_BSS) ? SHT_NOBITS : SHT_PROGBITS;
        sh.sh_flags = SHF_ALLOC | (k == SEC_TEXT ? SHF_EXECINSTR : 0) | ((k == SEC_DATA || k == SEC_BSS) ? SHF_WRITE : 0);
        sh.sh_addr = ctx->merged_addr[k];
        sh.sh_offset = off_sec[k];
        sh.sh_size = ctx->merged_size[k];
        sh.sh_addralign = 4;
        fwrite(&sh, sizeof(sh), 1, f);
    }
    memset(&sh, 0, sizeof(sh));
    sh.sh_name = name_symtab;
    sh.sh_type = SHT_SYMTAB;
    sh.sh_offset = off_symtab;
    sh.sh_size = (uint32_t)nsym * sizeof(Elf32_Sym);
    sh.sh_link = (Elf32_Word)shidx_strtab;
    sh.sh_info = 1; /* one local (null) symbol, rest are global - see AS's elf_writer.c for the full convention; not load-bearing here */
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
