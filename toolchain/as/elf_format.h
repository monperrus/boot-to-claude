#pragma once
/* Minimal ELF32 structure/constant definitions - only what AS's elf_writer
 * and (later) LD's elf_reader need. Defined locally rather than pulled
 * from the system <elf.h> so this code doesn't quietly depend on a glibc
 * header CC won't have access to once the toolchain self-hosts. */

#include <stdint.h>

typedef uint32_t Elf32_Addr;
typedef uint32_t Elf32_Off;
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Word;
typedef int32_t  Elf32_Sword;

#define EI_NIDENT 16

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf32_Half e_type;
    Elf32_Half e_machine;
    Elf32_Word e_version;
    Elf32_Addr e_entry;
    Elf32_Off  e_phoff;
    Elf32_Off  e_shoff;
    Elf32_Word e_flags;
    Elf32_Half e_ehsize;
    Elf32_Half e_phentsize;
    Elf32_Half e_phnum;
    Elf32_Half e_shentsize;
    Elf32_Half e_shnum;
    Elf32_Half e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    Elf32_Word sh_name;
    Elf32_Word sh_type;
    Elf32_Word sh_flags;
    Elf32_Addr sh_addr;
    Elf32_Off  sh_offset;
    Elf32_Word sh_size;
    Elf32_Word sh_link;
    Elf32_Word sh_info;
    Elf32_Word sh_addralign;
    Elf32_Word sh_entsize;
} Elf32_Shdr;

typedef struct {
    Elf32_Word    st_name;
    Elf32_Addr    st_value;
    Elf32_Word    st_size;
    unsigned char st_info;
    unsigned char st_other;
    Elf32_Half    st_shndx;
} Elf32_Sym;

typedef struct {
    Elf32_Addr r_offset;
    Elf32_Word r_info;
} Elf32_Rel;

typedef struct {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
} Elf32_Phdr;

#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'
#define ELFCLASS32 1
#define ELFDATA2LSB 1
#define EV_CURRENT 1
#define ELFOSABI_SYSV 0

#define ET_REL 1
#define ET_EXEC 2
#define EM_386 3

#define SHN_UNDEF 0
#define SHT_NULL 0
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_REL 9
#define SHT_NOBITS 8

#define SHF_WRITE 0x1
#define SHF_ALLOC 0x2
#define SHF_EXECINSTR 0x4

#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STT_NOTYPE 0
#define STT_FUNC 2
#define STT_SECTION 3
/* Plain functions, not function-like macros: CC's preprocessor only
 * supports object-like #define (see docs/toolchain.md, Phase T2) - these
 * four need real parameters. */
static inline unsigned char elf32_st_info(int bind, int type) { return (unsigned char)((bind << 4) + (type & 0xf)); }

#define R_386_32 1
#define R_386_PC32 2
static inline unsigned int elf32_r_sym(unsigned int info) { return info >> 8; }
static inline unsigned char elf32_r_type(unsigned int info) { return (unsigned char)info; }
static inline unsigned int elf32_r_info(unsigned int sym, unsigned int type) { return (sym << 8) + (unsigned char)type; }

#define PT_LOAD 1
#define PF_X 1
#define PF_W 2
#define PF_R 4
