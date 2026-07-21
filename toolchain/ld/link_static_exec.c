#include "ld.h"
#include "elf_format.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* Classic default base for a 32-bit, non-PIE, statically-linked Linux
 * executable. */
#define STATIC_EXEC_BASE 0x08048000u

static uint32_t align4(uint32_t x) { return (x + 3) & ~3u; }

void static_exec_assign_addresses(link_ctx_t *ctx) {
    uint32_t header_size = sizeof(Elf32_Ehdr) + sizeof(Elf32_Phdr);
    uint32_t addr = STATIC_EXEC_BASE + header_size;
    ctx->merged_addr[SEC_TEXT] = addr;
    addr = align4(addr + ctx->merged_size[SEC_TEXT]);
    ctx->merged_addr[SEC_RODATA] = addr;
    addr = align4(addr + ctx->merged_size[SEC_RODATA]);
    ctx->merged_addr[SEC_DATA] = addr;
    addr = align4(addr + ctx->merged_size[SEC_DATA]);
    ctx->merged_addr[SEC_BSS] = addr;
}

/* A single RWX PT_LOAD segment covering the whole file (header included -
 * p_offset=0, p_vaddr=STATIC_EXEC_BASE, both trivially page-aligned since
 * offset 0 always is). Real linkers split read-only/writable/executable
 * regions into separate page-aligned segments; one segment is a
 * deliberate simplification (this project's kernel.elf already accepts
 * an "RWX LOAD segment" warning from GNU ld for the same reason - see
 * docs/toolchain.md). No section header table: the Linux kernel's
 * execve() loader only ever reads program headers, sections are purely
 * a linker/debugger concern, so omitting them (e_shoff/e_shnum = 0) is a
 * valid, smaller, and simpler ELF executable. */
int write_static_exec_elf(link_ctx_t *ctx, const char *path) {
    uint32_t header_size = sizeof(Elf32_Ehdr) + sizeof(Elf32_Phdr);
    uint32_t filesz = (ctx->merged_addr[SEC_DATA] + ctx->merged_size[SEC_DATA]) - STATIC_EXEC_BASE;
    uint32_t memsz = (ctx->merged_addr[SEC_BSS] + ctx->merged_size[SEC_BSS]) - STATIC_EXEC_BASE;

    out_symbol_t *entry = find_out_sym(ctx, "_start");
    if (!entry) {
        fprintf(stderr, "ld: no _start symbol - static executables need one (CC's --target=host always emits it)\n");
        return -1;
    }

    FILE *f = fopen(path, "wb");
    if (!f) { perror("ld: fopen"); return -1; }

    Elf32_Ehdr eh = {0};
    eh.e_ident[0] = ELFMAG0; eh.e_ident[1] = ELFMAG1; eh.e_ident[2] = ELFMAG2; eh.e_ident[3] = ELFMAG3;
    eh.e_ident[4] = ELFCLASS32; eh.e_ident[5] = ELFDATA2LSB; eh.e_ident[6] = EV_CURRENT;
    eh.e_ident[7] = ELFOSABI_SYSV;
    eh.e_type = ET_EXEC;
    eh.e_machine = EM_386;
    eh.e_version = EV_CURRENT;
    eh.e_entry = entry->addr;
    eh.e_phoff = sizeof(Elf32_Ehdr);
    eh.e_ehsize = sizeof(Elf32_Ehdr);
    eh.e_phentsize = sizeof(Elf32_Phdr);
    eh.e_phnum = 1;
    fwrite(&eh, sizeof(eh), 1, f);

    Elf32_Phdr ph = {0};
    ph.p_type = PT_LOAD;
    ph.p_offset = 0;
    ph.p_vaddr = STATIC_EXEC_BASE;
    ph.p_paddr = STATIC_EXEC_BASE;
    ph.p_filesz = filesz;
    ph.p_memsz = memsz;
    ph.p_flags = PF_R | PF_W | PF_X;
    ph.p_align = 0x1000;
    fwrite(&ph, sizeof(ph), 1, f);

    uint32_t cur = header_size;
    fwrite(ctx->merged_data[SEC_TEXT], 1, ctx->merged_size[SEC_TEXT], f);
    cur += ctx->merged_size[SEC_TEXT];

    uint32_t rodata_off = ctx->merged_addr[SEC_RODATA] - STATIC_EXEC_BASE;
    while (cur < rodata_off) { fputc(0, f); cur++; }
    fwrite(ctx->merged_data[SEC_RODATA], 1, ctx->merged_size[SEC_RODATA], f);
    cur += ctx->merged_size[SEC_RODATA];

    uint32_t data_off = ctx->merged_addr[SEC_DATA] - STATIC_EXEC_BASE;
    while (cur < data_off) { fputc(0, f); cur++; }
    fwrite(ctx->merged_data[SEC_DATA], 1, ctx->merged_size[SEC_DATA], f);
    /* .bss: no file content - covered by p_memsz > p_filesz, zero-filled by the loader */

    fclose(f);
    /* 0755 octal == 493 decimal - written in decimal since CC's lexer
     * has no octal-literal support (see lexer.c's numeric-literal
     * reader): a leading-zero literal like `0755` is silently read as
     * plain decimal 755 (== octal 1363, a nonsense rwx pattern, mostly
     * missing owner-read - see docs/toolchain.md, Phase T8). */
    chmod(path, 493);
    return 0;
}
