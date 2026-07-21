#include "ld.h"
#include "elf_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Prefix match (matches real linker scripts' `*(.text*)` wildcard
 * convention), not exact - AS's own object files always name a section
 * exactly ".text"/".data"/etc, but object files from other assemblers
 * (e.g. this project's own kernel/entry.S, still assembled by nasm
 * until Phase T7's switchover, names its section ".text.entry") use the
 * variant names this is meant to catch too. Caught by testing LD against
 * the *current* nasm-built objects, not just AS's own output - see
 * docs/toolchain.md. */
static sec_kind_t classify_section(const char *name) {
    if (strncmp(name, ".text", 5) == 0) return SEC_TEXT;
    if (strncmp(name, ".rodata", 7) == 0) return SEC_RODATA;
    if (strncmp(name, ".data", 5) == 0) return SEC_DATA;
    if (strncmp(name, ".bss", 4) == 0) return SEC_BSS;
    return SEC_KIND_OTHER;
}

static uint8_t *read_whole_file(const char *path, uint32_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "ld: cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "ld: short read on %s\n", path); exit(1); }
    fclose(f);
    *len_out = (uint32_t)n;
    return buf;
}

int read_object(const char *path, in_file_t *out) {
    memset(out, 0, sizeof(*out));
    safe_strcpy(out->filename, path, sizeof(out->filename));
    out->data = read_whole_file(path, &out->data_len);

    Elf32_Ehdr eh;
    memcpy(&eh, out->data, sizeof(eh));
    if (eh.e_ident[0] != ELFMAG0 || eh.e_ident[1] != ELFMAG1 ||
        eh.e_ident[2] != ELFMAG2 || eh.e_ident[3] != ELFMAG3) {
        fprintf(stderr, "ld: %s: not an ELF file\n", path);
        return -1;
    }
    if (eh.e_type != ET_REL || eh.e_machine != EM_386) {
        fprintf(stderr, "ld: %s: not an ELF32 x86 relocatable object\n", path);
        return -1;
    }
    if (eh.e_shnum > MAX_SECTIONS_PER_FILE) {
        fprintf(stderr, "ld: %s: too many sections\n", path);
        return -1;
    }

    Elf32_Shdr shdrs[MAX_SECTIONS_PER_FILE];
    for (int i = 0; i < eh.e_shnum; i++) {
        memcpy(&shdrs[i], out->data + eh.e_shoff + (uint32_t)i * sizeof(Elf32_Shdr), sizeof(Elf32_Shdr));
    }
    const char *shstrtab = (const char *)(out->data + shdrs[eh.e_shstrndx].sh_offset);

    out->n_sections = eh.e_shnum;
    int symtab_idx = -1;
    for (int i = 0; i < eh.e_shnum; i++) {
        in_section_t *s = &out->sections[i];
        safe_strcpy(s->name, shstrtab + shdrs[i].sh_name, sizeof(s->name));
        s->kind = classify_section(s->name);
        s->is_nobits = shdrs[i].sh_type == SHT_NOBITS;
        s->file_off = shdrs[i].sh_offset;
        s->size = shdrs[i].sh_size;
        if (shdrs[i].sh_type == SHT_SYMTAB) symtab_idx = i;
    }

    if (symtab_idx >= 0) {
        const char *strtab = (const char *)(out->data + shdrs[shdrs[symtab_idx].sh_link].sh_offset);
        uint32_t n = shdrs[symtab_idx].sh_size / sizeof(Elf32_Sym);
        if (n > MAX_SYMS_PER_FILE) { fprintf(stderr, "ld: %s: too many symbols\n", path); return -1; }
        for (uint32_t i = 0; i < n; i++) {
            Elf32_Sym sym;
            memcpy(&sym, out->data + shdrs[symtab_idx].sh_offset + i * sizeof(Elf32_Sym), sizeof(sym));
            in_symbol_t *is = &out->syms[out->n_syms++];
            safe_strcpy(is->name, strtab + sym.st_name, sizeof(is->name));
            is->is_global = (sym.st_info >> 4) == STB_GLOBAL;
            is->defined = sym.st_shndx != SHN_UNDEF;
            is->section_idx = is->defined ? sym.st_shndx : -1;
            is->value = sym.st_value;
        }

        for (int i = 0; i < eh.e_shnum; i++) {
            if (shdrs[i].sh_type != SHT_REL) continue;
            int target_sec = (int)shdrs[i].sh_info;
            uint32_t n_rel = shdrs[i].sh_size / sizeof(Elf32_Rel);
            for (uint32_t j = 0; j < n_rel; j++) {
                if (out->n_relocs >= MAX_RELOCS_PER_FILE) { fprintf(stderr, "ld: %s: too many relocations\n", path); return -1; }
                Elf32_Rel rel;
                memcpy(&rel, out->data + shdrs[i].sh_offset + j * sizeof(Elf32_Rel), sizeof(rel));
                in_reloc_t *ir = &out->relocs[out->n_relocs++];
                ir->section_idx = target_sec;
                ir->offset = rel.r_offset;
                ir->symbol_idx = (int)elf32_r_sym(rel.r_info);
                ir->type = (int)elf32_r_type(rel.r_info);
            }
        }
    }

    return 0;
}
