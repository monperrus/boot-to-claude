#include "as.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int regnum(reg_t r) {
    switch (r) {
        case R_AL: case R_AX: case R_EAX: return 0;
        case R_CL: case R_CX: case R_ECX: return 1;
        case R_DL: case R_DX: case R_EDX: return 2;
        case R_BL: case R_BX: case R_EBX: return 3;
        case R_AH: case R_SP: case R_ESP: return 4;
        case R_CH: case R_BP: case R_EBP: return 5;
        case R_DH: case R_SI: case R_ESI: return 6;
        case R_BH: case R_DI: case R_EDI: return 7;
        case R_ES: return 0;
        case R_CS: return 1;
        case R_SS: return 2;
        case R_DS: return 3;
        case R_FS: return 4;
        case R_GS: return 5;
        case R_CR0: return 0;
        default: return 0;
    }
}

static int is_reg32(reg_t r) { return r >= R_EAX && r <= R_EDI; }
static int is_reg16(reg_t r) { return r >= R_AX && r <= R_DI; }
static int is_reg8(reg_t r) { return r >= R_AL && r <= R_BH; }
static int is_sreg(reg_t r) { return r >= R_ES && r <= R_GS; }

/* --- growable emission into `out` (NULL in dry-run/length-only mode) --- */

typedef struct {
    uint8_t *out;
    int len;
} emitter_t;

static void e8(emitter_t *e, uint8_t b) {
    if (e->out) e->out[e->len] = b;
    e->len++;
}
static void e16(emitter_t *e, uint16_t v) { e8(e, v & 0xFF); e8(e, (v >> 8) & 0xFF); }
static void e32(emitter_t *e, uint32_t v) {
    e8(e, v & 0xFF); e8(e, (v >> 8) & 0xFF); e8(e, (v >> 16) & 0xFF); e8(e, (v >> 24) & 0xFF);
}

/* --- symbol resolution ---
 * `strict` distinguishes pass 1 (length-only: never errors, never emits
 * relocations, unresolved symbols read as 0) from pass 2 (real: local
 * symbols resolve directly or, in ELF mode, a relocation is emitted for
 * anything not resolvable within the same section at assemble time). */

static void add_reloc(asm_ctx_t *ctx, section_kind_t sec, uint32_t off, const char *sym,
                       int32_t addend, reloc_kind_t kind) {
    if (ctx->nrelocs >= MAX_RELOCS) { fprintf(stderr, "as: too many relocations\n"); exit(1); }
    reloc_t *r = &ctx->relocs[ctx->nrelocs++];
    r->section = sec;
    r->offset = off;
    safe_strcpy(r->sym, sym, MAX_NAME);
    r->addend = addend;
    r->kind = kind;
}

/* Resolves an absolute-address reference (symbol + disp) used as an
 * immediate/memory-displacement value. is_bin selects -f bin semantics
 * (single flat address space, always resolvable, no relocations) vs.
 * -f elf32 (may need a relocation). Writes the resolved/placeholder
 * value via e32/e16 at the call site - this just computes it. */
static int64_t resolve_abs(asm_ctx_t *ctx, int is_bin, const char *sym, int64_t disp,
                             section_kind_t patch_sec, uint32_t patch_off, int strict) {
    if (!sym[0]) return disp;
    symbol_t *s = sym_find(ctx, sym);
    if (s && s->is_equ) return s->equ_value + disp; /* plain number, never org-adjusted */
    if (is_bin) {
        if (!s || !s->defined) {
            if (strict) { fprintf(stderr, "as: undefined symbol '%s'\n", sym); exit(1); }
            return disp;
        }
        return (int64_t)ctx->org + (int64_t)s->offset + disp;
    }
    /* ELF: absolute references always need a relocation - the section's
     * final base address isn't known until link time. x86 ELF uses REL
     * (not RELA) relocations: the addend has no dedicated field, it must
     * be baked into the bytes at the relocated location, which is why
     * this returns `disp` (the addend) rather than 0 - LD will add the
     * resolved symbol value on top of whatever's already there. */
    if (strict) {
        add_reloc(ctx, patch_sec, patch_off, sym, (int32_t)disp, RELOC_ABS32);
    }
    return disp;
}

/* Resolves a PC-relative call/jmp target. Same-section same-file targets
 * fold to a fixed relative displacement directly; anything else (extern,
 * or -f bin's "always resolvable" absolute model) is handled by the
 * caller via this return value or a relocation. `next_ip_rel` is the
 * SECTION-RELATIVE position right after the relative field (i.e. NOT
 * org-adjusted - this function adds org itself when is_bin, consistently
 * with how it computes the target, since bin-mode "addresses" are
 * `org + offset` throughout). */
static int64_t resolve_pcrel(asm_ctx_t *ctx, int is_bin, const char *sym, int64_t disp,
                               section_kind_t cur_sec, int64_t next_ip_rel,
                               section_kind_t patch_sec, uint32_t patch_off, int strict,
                               int field_bytes) {
    if (!sym[0]) {
        /* Bare constant target address, e.g. `jmp KERNEL_LOAD_ADDR`
         * where KERNEL_LOAD_ADDR is an EQU - disp is already the
         * absolute target, no symbol lookup needed. */
        int64_t next_ip = is_bin ? (int64_t)ctx->org + next_ip_rel : next_ip_rel;
        return disp - next_ip;
    }
    symbol_t *s = sym_find(ctx, sym);
    if (is_bin) {
        if (!s || !s->defined) {
            if (strict) { fprintf(stderr, "as: undefined symbol '%s'\n", sym); exit(1); }
            return 0;
        }
        int64_t target = (int64_t)ctx->org + (int64_t)s->offset + disp;
        int64_t next_ip = (int64_t)ctx->org + next_ip_rel;
        return target - next_ip;
    }
    if (s && s->defined && s->section == cur_sec) {
        return ((int64_t)s->offset + disp) - next_ip_rel;
    }
    /* Same REL-vs-RELA reasoning as resolve_abs: the standard x86 addend
     * for a rel32 call/jmp is -field_bytes (the CPU measures the
     * displacement from the end of the instruction), and that addend
     * must be baked into the bytes, not left as 0. */
    int32_t addend = (int32_t)(disp - field_bytes);
    if (strict) {
        add_reloc(ctx, patch_sec, patch_off, sym, addend, RELOC_PC32);
    }
    return addend;
}

/* --- generic 32-bit ModRM(+SIB+disp) encoder ---
 * Used by CC-generated code (always 32-bit: kernel and host targets are
 * both x86-32). Handles register-direct, [esp]/[esp+disp8] (SIB - ESP
 * can't be a plain ModRM base), [ebp+disp32] (always the disp32 form,
 * mod=10 - mod=00 with rm=101 means "disp32 direct address" instead of
 * "ebp with no displacement", so EBP-relative never uses mod=00), and
 * [label(+disp)] (mod=00, rm=101, disp32 direct address - 32-bit's other
 * ModRM special case). `reg_field` is either another register or a group
 * opcode extension number, per the caller. */
static void encode_rm32(emitter_t *e, asm_ctx_t *ctx, int is_bin, operand_t *rm, int reg_field,
                          section_kind_t patch_sec, uint32_t stmt_offset, int strict) {
    if (rm->kind == OPND_REG) {
        e8(e, (uint8_t)(0xC0 | (reg_field << 3) | regnum(rm->reg)));
        return;
    }
    if (rm->reg == R_ESP) {
        if (rm->disp == 0) {
            e8(e, (uint8_t)(0x04 | (reg_field << 3)));
            e8(e, 0x24);
        } else {
            e8(e, (uint8_t)(0x44 | (reg_field << 3)));
            e8(e, 0x24);
            e8(e, (uint8_t)rm->disp);
        }
        return;
    }
    if (rm->reg == R_EBP) {
        e8(e, (uint8_t)(0x85 | (reg_field << 3)));
        e32(e, (uint32_t)rm->disp);
        return;
    }
    if (rm->reg != R_NONE) {
        /* [reg] / [reg+disp] for any other GP register (EAX/ECX/EDX/EBX/
         * ESI/EDI) - no SIB needed, and mod=00 with no displacement is
         * safe here (rm=101/EBP is the only base register where mod=00
         * means something else - see the EBP case above). Always uses
         * the disp32 form when non-zero, same "no size-optimization"
         * philosophy as everywhere else in this encoder. */
        if (rm->disp == 0) {
            e8(e, (uint8_t)(0x00 | (reg_field << 3) | regnum(rm->reg)));
        } else {
            e8(e, (uint8_t)(0x80 | (reg_field << 3) | regnum(rm->reg)));
            e32(e, (uint32_t)rm->disp);
        }
        return;
    }
    /* bare label, no base register: 32-bit direct address */
    e8(e, (uint8_t)(0x05 | (reg_field << 3)));
    uint32_t patch_off = stmt_offset + (uint32_t)e->len;
    int64_t addr = resolve_abs(ctx, is_bin, rm->sym, rm->disp, patch_sec, patch_off, strict);
    e32(e, (uint32_t)addr);
}

/* ADD/OR/AND/SUB/XOR/CMP share one opcode block layout:
 * base+0 = r/m8,r8 ; base+1 = r/m32,r32 ; base+2 = r8,r/m8 ; base+3 = r32,r/m32
 * base+4 = AL,imm8 ; base+5 = eAX,imm32 ; group1 (0x81 /ext) = r/m,imm32 */
static int alu_lookup(const char *m, int *base, int *ext) {
    static const struct { const char *name; int base, ext; } table[] = {
        { "add", 0x00, 0 }, { "or", 0x08, 1 }, { "and", 0x20, 4 },
        { "sub", 0x28, 5 }, { "xor", 0x30, 6 }, { "cmp", 0x38, 7 },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(m, table[i].name) == 0) { *base = table[i].base; *ext = table[i].ext; return 1; }
    }
    return 0;
}

/* --- the encoder --- */

int stmt_encode(asm_ctx_t *ctx, stmt_t *st, uint8_t *out, int strict) {
    emitter_t e = { out, 0 };
    const char *m = st->mnemonic;
    operand_t *a = &st->ops[0];
    operand_t *b = &st->ops[1];
    operand_t *c = &st->ops[2];
    int is_bin = ctx->have_org;

    /* --- pseudo-ops for data directives: one scalar value per stmt --- */
    if (strcmp(m, ".db") == 0 || strcmp(m, ".dw") == 0 || strcmp(m, ".dd") == 0 || strcmp(m, ".dq") == 0) {
        int width = strcmp(m, ".db") == 0 ? 1 : strcmp(m, ".dw") == 0 ? 2 : strcmp(m, ".dd") == 0 ? 4 : 8;
        int64_t v = a->has_sym
            ? resolve_abs(ctx, is_bin, a->sym, a->disp, st->section, st->offset, strict)
            : a->disp;
        if (width == 1) e8(&e, (uint8_t)v);
        else if (width == 2) e16(&e, (uint16_t)v);
        else if (width == 4) e32(&e, (uint32_t)v);
        else { e32(&e, (uint32_t)v); e32(&e, (uint32_t)(v >> 32)); }
        return e.len;
    }
    if (strcmp(m, ".resb") == 0) {
        for (int64_t i = 0; i < a->disp; i++) e8(&e, 0);
        return e.len;
    }
    if (strcmp(m, ".pad") == 0) {
        /* `times N db V` folds to one .pad stmt: a->disp = N, b->disp = V */
        for (int64_t i = 0; i < a->disp; i++) e8(&e, (uint8_t)b->disp);
        return e.len;
    }

    /* --- generic 32-bit ALU ops (add/or/and/sub/xor/cmp): reg<->reg,
     * reg<->mem(ebp/esp/label), reg<->imm32 - what CC's codegen emits.
     * Gated on bits==32 so this can never shadow the 16-bit real-mode
     * forms below (which have their own, narrower matches). */
    {
        int base, ext;
        if (st->bits == 32 && alu_lookup(m, &base, &ext)) {
            if (a->kind == OPND_REG && is_reg32(a->reg) && (b->kind == OPND_REG || b->kind == OPND_MEM)) {
                e8(&e, (uint8_t)(base + 3)); /* r32, r/m32 */
                encode_rm32(&e, ctx, is_bin, b, regnum(a->reg), st->section, st->offset, strict);
                return e.len;
            }
            if ((a->kind == OPND_REG || a->kind == OPND_MEM) && b->kind == OPND_REG && is_reg32(b->reg)) {
                e8(&e, (uint8_t)(base + 1)); /* r/m32, r32 */
                encode_rm32(&e, ctx, is_bin, a, regnum(b->reg), st->section, st->offset, strict);
                return e.len;
            }
            if ((a->kind == OPND_REG || a->kind == OPND_MEM) && b->kind == OPND_IMM) {
                e8(&e, 0x81); /* r/m32, imm32 (always full-size - no imm8 sign-extend optimization) */
                encode_rm32(&e, ctx, is_bin, a, ext, st->section, st->offset, strict);
                int64_t v = b->has_sym
                    ? resolve_abs(ctx, is_bin, b->sym, b->disp, st->section, st->offset + (uint32_t)e.len, strict)
                    : b->disp;
                e32(&e, (uint32_t)v);
                return e.len;
            }
        }
    }

    if (strcmp(m, "xor") == 0 || strcmp(m, "or") == 0 || strcmp(m, "sub") == 0 || strcmp(m, "add") == 0) {
        int ext = strcmp(m, "add") == 0 ? 0 : strcmp(m, "or") == 0 ? 1 : strcmp(m, "sub") == 0 ? 5 : 6; /* xor */
        if (a->kind == OPND_REG && b->kind == OPND_REG && is_reg32(a->reg) && is_reg32(b->reg)) {
            e8(&e, (uint8_t)(0x01 + ext * 8));
            e8(&e, (uint8_t)(0xC0 | (regnum(b->reg) << 3) | regnum(a->reg)));
            return e.len;
        }
        if (a->kind == OPND_REG && b->kind == OPND_REG && is_reg16(a->reg) && is_reg16(b->reg)) {
            e8(&e, (uint8_t)(0x01 + ext * 8));
            e8(&e, (uint8_t)(0xC0 | (regnum(b->reg) << 3) | regnum(a->reg)));
            return e.len;
        }
        if (a->kind == OPND_REG && b->kind == OPND_REG && is_reg8(a->reg) && is_reg8(b->reg)) {
            e8(&e, (uint8_t)(0x00 + ext * 8));
            e8(&e, (uint8_t)(0xC0 | (regnum(b->reg) << 3) | regnum(a->reg)));
            return e.len;
        }
        if (a->kind == OPND_REG && a->reg == R_AL && b->kind == OPND_IMM) {
            e8(&e, (uint8_t)(0x04 + ext * 8));
            e8(&e, (uint8_t)b->disp);
            return e.len;
        }
        if (a->kind == OPND_REG && a->reg == R_EAX && b->kind == OPND_IMM && st->bits == 16) {
            e8(&e, 0x66);
            e8(&e, (uint8_t)(0x05 + ext * 8));
            e32(&e, (uint32_t)b->disp);
            return e.len;
        }
        if (st->bits == 16 && a->kind == OPND_MEM && !a->reg && b->kind == OPND_REG && is_reg32(b->reg)) {
            e8(&e, 0x66);
            e8(&e, (uint8_t)(0x01 + ext * 8));
            e8(&e, (uint8_t)(0x06 | (regnum(b->reg) << 3)));
            e16(&e, (uint16_t)resolve_abs(ctx, is_bin, a->sym, a->disp, st->section, st->offset + (uint32_t)e.len, strict));
            return e.len;
        }
        if (st->bits == 16 && a->kind == OPND_MEM && !a->reg && b->kind == OPND_REG && is_reg16(b->reg)) {
            e8(&e, (uint8_t)(0x01 + ext * 8));
            e8(&e, (uint8_t)(0x06 | (regnum(b->reg) << 3)));
            e16(&e, (uint16_t)resolve_abs(ctx, is_bin, a->sym, a->disp, st->section, st->offset + (uint32_t)e.len, strict));
            return e.len;
        }
    }

    if (strcmp(m, "cmp") == 0) {
        if (a->kind == OPND_REG && is_reg16(a->reg) && b->kind == OPND_IMM) {
            e8(&e, 0x81);
            e8(&e, (uint8_t)(0xF8 | regnum(a->reg)));
            e16(&e, (uint16_t)b->disp);
            return e.len;
        }
    }

    if (strcmp(m, "mov") == 0) {
        if (is_sreg(a->reg) || is_sreg(b->reg)) {
            reg_t sreg = is_sreg(a->reg) ? a->reg : b->reg;
            e8(&e, 0x8E);
            e8(&e, (uint8_t)(0xC0 | (regnum(sreg) << 3) | regnum(b->reg == sreg ? a->reg : b->reg)));
            return e.len;
        }
        if (a->reg == R_CR0 && b->kind == OPND_REG && is_reg32(b->reg)) {
            e8(&e, 0x0F); e8(&e, 0x22);
            e8(&e, (uint8_t)(0xC0 | regnum(b->reg)));
            return e.len;
        }
        if (b->reg == R_CR0 && a->kind == OPND_REG && is_reg32(a->reg)) {
            /* MOV r32, CR0: modrm reg field = CR0 (0), rm field = dest reg */
            e8(&e, 0x0F); e8(&e, 0x20);
            e8(&e, (uint8_t)(0xC0 | regnum(a->reg)));
            return e.len;
        }
        if (a->kind == OPND_REG && b->kind == OPND_IMM && is_reg8(a->reg)) {
            e8(&e, (uint8_t)(0xB0 + regnum(a->reg)));
            int64_t v = b->has_sym ? resolve_abs(ctx, is_bin, b->sym, b->disp, st->section, st->offset + 1, strict) : b->disp;
            e8(&e, (uint8_t)v);
            return e.len;
        }
        if (a->kind == OPND_REG && b->kind == OPND_IMM && is_reg16(a->reg) && st->bits == 16) {
            e8(&e, (uint8_t)(0xB8 + regnum(a->reg)));
            int64_t v = b->has_sym ? resolve_abs(ctx, is_bin, b->sym, b->disp, st->section, st->offset + 1, strict) : b->disp;
            e16(&e, (uint16_t)v);
            return e.len;
        }
        if (a->kind == OPND_REG && b->kind == OPND_IMM && is_reg16(a->reg) && st->bits == 32) {
            e8(&e, 0x66);
            e8(&e, (uint8_t)(0xB8 + regnum(a->reg)));
            int64_t v = b->has_sym ? resolve_abs(ctx, is_bin, b->sym, b->disp, st->section, st->offset + 2, strict) : b->disp;
            e16(&e, (uint16_t)v);
            return e.len;
        }
        if (a->kind == OPND_REG && b->kind == OPND_IMM && is_reg32(a->reg)) {
            e8(&e, (uint8_t)(0xB8 + regnum(a->reg)));
            int64_t v = b->has_sym
                ? resolve_abs(ctx, is_bin, b->sym, b->disp, st->section, st->offset + 1, strict)
                : b->disp;
            e32(&e, (uint32_t)v);
            return e.len;
        }
        if (a->kind == OPND_REG && b->kind == OPND_REG && is_reg16(a->reg) && is_reg16(b->reg)) {
            e8(&e, 0x8B);
            e8(&e, (uint8_t)(0xC0 | (regnum(a->reg) << 3) | regnum(b->reg)));
            return e.len;
        }
        /* mov r16, r/m16 and mov r/m16, r16 in 32-bit code - e.g.
         * `mov ax, [eax]` / `mov [ecx], ax`, the pair CC's codegen uses
         * for 16-bit (uint16_t/short) loads/stores via the same general
         * register-based addressing forms encode_rm32 already handles
         * for 32-bit operands. Only the operand-size prefix (0x66)
         * differs from the 32-bit mov r32,r/m32 case below - the
         * addressing byte(s) are identical either way. Distinct from the
         * st->bits==16 bare-label forms right below, which are for
         * 16-bit *code* (real mode), not 32-bit code touching a 16-bit
         * operand. */
        if (st->bits == 32 && a->kind == OPND_REG && is_reg16(a->reg) && b->kind == OPND_MEM) {
            e8(&e, 0x66); e8(&e, 0x8B);
            encode_rm32(&e, ctx, is_bin, b, regnum(a->reg), st->section, st->offset, strict);
            return e.len;
        }
        if (st->bits == 32 && a->kind == OPND_MEM && b->kind == OPND_REG && is_reg16(b->reg)) {
            e8(&e, 0x66); e8(&e, 0x89);
            encode_rm32(&e, ctx, is_bin, a, regnum(b->reg), st->section, st->offset, strict);
            return e.len;
        }
        if (st->bits == 16 && a->kind == OPND_REG && is_reg16(a->reg) && b->kind == OPND_MEM && !b->reg) {
            e8(&e, 0x8B);
            e8(&e, (uint8_t)(0x06 | (regnum(a->reg) << 3)));
            e16(&e, (uint16_t)resolve_abs(ctx, is_bin, b->sym, b->disp, st->section, st->offset + (uint32_t)e.len, strict));
            return e.len;
        }
        if (st->bits == 16 && a->kind == OPND_MEM && !a->reg && b->kind == OPND_REG && is_reg16(b->reg)) {
            e8(&e, 0x89);
            e8(&e, (uint8_t)(0x06 | (regnum(b->reg) << 3)));
            e16(&e, (uint16_t)resolve_abs(ctx, is_bin, a->sym, a->disp, st->section, st->offset + (uint32_t)e.len, strict));
            return e.len;
        }
        if (st->bits == 16 && a->kind == OPND_REG && is_reg32(a->reg) && b->kind == OPND_MEM && !b->reg) {
            e8(&e, 0x66); e8(&e, 0x8B);
            e8(&e, (uint8_t)(0x06 | (regnum(a->reg) << 3)));
            e16(&e, (uint16_t)resolve_abs(ctx, is_bin, b->sym, b->disp, st->section, st->offset + (uint32_t)e.len, strict));
            return e.len;
        }
        if (st->bits == 16 && a->kind == OPND_MEM && !a->reg && b->kind == OPND_REG && is_reg32(b->reg)) {
            e8(&e, 0x66); e8(&e, 0x89);
            e8(&e, (uint8_t)(0x06 | (regnum(b->reg) << 3)));
            e16(&e, (uint16_t)resolve_abs(ctx, is_bin, a->sym, a->disp, st->section, st->offset + (uint32_t)e.len, strict));
            return e.len;
        }
        if (st->bits == 16 && a->kind == OPND_MEM && !a->reg && b->kind == OPND_IMM && a->size_override == 2) {
            e8(&e, 0xC7);
            e8(&e, (uint8_t)0x06);
            e16(&e, (uint16_t)resolve_abs(ctx, is_bin, a->sym, a->disp, st->section, st->offset + (uint32_t)e.len, strict));
            int64_t v = b->has_sym ? resolve_abs(ctx, is_bin, b->sym, b->disp, st->section, st->offset + (uint32_t)e.len + 2, strict) : b->disp;
            e16(&e, (uint16_t)v);
            return e.len;
        }
        if (st->bits == 16 && a->kind == OPND_MEM && !a->reg && b->kind == OPND_IMM && a->size_override == 4) {
            e8(&e, 0x66); e8(&e, 0xC7);
            e8(&e, (uint8_t)0x06);
            e16(&e, (uint16_t)resolve_abs(ctx, is_bin, a->sym, a->disp, st->section, st->offset + (uint32_t)e.len, strict));
            int64_t v = b->has_sym ? resolve_abs(ctx, is_bin, b->sym, b->disp, st->section, st->offset + (uint32_t)e.len + 4, strict) : b->disp;
            e32(&e, (uint32_t)v);
            return e.len;
        }
        if (a->kind == OPND_REG && is_reg32(a->reg) && b->kind == OPND_MEM && b->reg == R_ESP) {
            e8(&e, 0x8B);
            if (b->disp == 0) {
                e8(&e, (uint8_t)(0x04 | (regnum(a->reg) << 3)));
                e8(&e, 0x24);
            } else {
                e8(&e, (uint8_t)(0x44 | (regnum(a->reg) << 3)));
                e8(&e, 0x24);
                e8(&e, (uint8_t)b->disp);
            }
            return e.len;
        }

        /* --- generic 32-bit forms: reg<->reg, reg<->mem(ebp/label),
         * mem<->imm32, 8-bit reg<->mem(ebp/label) - what CC's codegen
         * emits for locals/globals/struct fields. */
        if (st->bits == 32) {
            if (a->kind == OPND_REG && is_reg32(a->reg) && (b->kind == OPND_REG || b->kind == OPND_MEM)) {
                e8(&e, 0x8B);
                encode_rm32(&e, ctx, is_bin, b, regnum(a->reg), st->section, st->offset, strict);
                return e.len;
            }
            if (a->kind == OPND_MEM && b->kind == OPND_REG && is_reg32(b->reg)) {
                e8(&e, 0x89);
                encode_rm32(&e, ctx, is_bin, a, regnum(b->reg), st->section, st->offset, strict);
                return e.len;
            }
            if (a->kind == OPND_REG && is_reg8(a->reg) && (b->kind == OPND_REG || b->kind == OPND_MEM)) {
                e8(&e, 0x8A);
                encode_rm32(&e, ctx, is_bin, b, regnum(a->reg), st->section, st->offset, strict);
                return e.len;
            }
            if (a->kind == OPND_MEM && b->kind == OPND_REG && is_reg8(b->reg)) {
                e8(&e, 0x88);
                encode_rm32(&e, ctx, is_bin, a, regnum(b->reg), st->section, st->offset, strict);
                return e.len;
            }
            if (a->kind == OPND_MEM && b->kind == OPND_IMM && a->size_override != 1) {
                e8(&e, 0xC7);
                encode_rm32(&e, ctx, is_bin, a, 0, st->section, st->offset, strict);
                int64_t v = b->has_sym
                    ? resolve_abs(ctx, is_bin, b->sym, b->disp, st->section, st->offset + (uint32_t)e.len, strict)
                    : b->disp;
                e32(&e, (uint32_t)v);
                return e.len;
            }
            if (a->kind == OPND_MEM && b->kind == OPND_IMM && a->size_override == 1) {
                e8(&e, 0xC6);
                encode_rm32(&e, ctx, is_bin, a, 0, st->section, st->offset, strict);
                e8(&e, (uint8_t)b->disp);
                return e.len;
            }
        }
    }

    if (strcmp(m, "lea") == 0 && st->bits == 32 && a->kind == OPND_REG && is_reg32(a->reg) && b->kind == OPND_MEM) {
        e8(&e, 0x8D);
        encode_rm32(&e, ctx, is_bin, b, regnum(a->reg), st->section, st->offset, strict);
        return e.len;
    }

    if (strcmp(m, "imul") == 0 && st->nops == 3 && st->bits == 32 && a->kind == OPND_REG && is_reg32(a->reg) && c->kind == OPND_IMM) {
        /* imul r32, r/m32, imm32 - always full-size, same "no imm8
         * sign-extend optimization" convention as the group1 ALU ops
         * above (see the .db/.pad branch's imm32 comment). */
        e8(&e, 0x69);
        encode_rm32(&e, ctx, is_bin, b, regnum(a->reg), st->section, st->offset, strict);
        int64_t v = c->has_sym
            ? resolve_abs(ctx, is_bin, c->sym, c->disp, st->section, st->offset, strict)
            : c->disp;
        e32(&e, (uint32_t)v);
        return e.len;
    }
    if (strcmp(m, "imul") == 0 && st->bits == 32 && a->kind == OPND_REG && is_reg32(a->reg)) {
        e8(&e, 0x0F); e8(&e, 0xAF);
        encode_rm32(&e, ctx, is_bin, b, regnum(a->reg), st->section, st->offset, strict);
        return e.len;
    }
    if (strcmp(m, "idiv") == 0 && (a->kind == OPND_REG || a->kind == OPND_MEM)) {
        e8(&e, 0xF7);
        encode_rm32(&e, ctx, is_bin, a, 7, st->section, st->offset, strict);
        return e.len;
    }
    if (strcmp(m, "not") == 0 && (a->kind == OPND_REG || a->kind == OPND_MEM)) {
        e8(&e, 0xF7);
        encode_rm32(&e, ctx, is_bin, a, 2, st->section, st->offset, strict);
        return e.len;
    }
    if (strcmp(m, "neg") == 0 && (a->kind == OPND_REG || a->kind == OPND_MEM)) {
        e8(&e, 0xF7);
        encode_rm32(&e, ctx, is_bin, a, 3, st->section, st->offset, strict);
        return e.len;
    }
    if (strcmp(m, "cdq") == 0) { e8(&e, 0x99); return e.len; }

    if (strncmp(m, "set", 3) == 0 && a->kind == OPND_REG && is_reg8(a->reg)) {
        static const struct { const char *suf; uint8_t op; } cc[] = {
            { "e", 0x94 }, { "ne", 0x95 }, { "l", 0x9C }, { "le", 0x9E }, { "g", 0x9F }, { "ge", 0x9D },
        };
        for (size_t i = 0; i < sizeof(cc) / sizeof(cc[0]); i++) {
            char full[16];
            snprintf(full, sizeof(full), "set%s", cc[i].suf);
            if (strcmp(m, full) == 0) {
                e8(&e, 0x0F); e8(&e, cc[i].op);
                e8(&e, (uint8_t)(0xC0 | regnum(a->reg)));
                return e.len;
            }
        }
    }

    if (strcmp(m, "movzx") == 0 && is_reg32(a->reg) && is_reg16(b->reg)) {
        /* MOVZX r32, r/m16 = 0F B7 /r, no 0x66 prefix - the opcode
         * itself already means "32-bit destination, 16-bit source"; a
         * leading 0x66 operand-size override reinterprets *both*
         * operands as 16-bit, silently downgrading this to r16,r16 (a
         * near no-op) instead of actually zero-extending into the full
         * 32-bit register. Never previously exercised: every earlier
         * movzx use was the byte-from-memory form (0F B6) below, not
         * this register-to-register 16-to-32 form - only CC's new
         * TY_SHORT load path (`mov ax,[eax]` then `movzx eax,ax`) hit
         * it, see docs/toolchain.md. */
        e8(&e, 0x0F); e8(&e, 0xB7);
        e8(&e, (uint8_t)(0xC0 | (regnum(a->reg) << 3) | regnum(b->reg)));
        return e.len;
    }
    if (strcmp(m, "movzx") == 0 && st->bits == 32 && is_reg32(a->reg) &&
        (b->kind == OPND_REG ? is_reg8(b->reg) : 1)) {
        e8(&e, 0x0F); e8(&e, 0xB6);
        encode_rm32(&e, ctx, is_bin, b, regnum(a->reg), st->section, st->offset, strict);
        return e.len;
    }

    if (strcmp(m, "shl") == 0 && is_reg16(a->reg) && b->kind == OPND_IMM) {
        e8(&e, 0xC1);
        e8(&e, (uint8_t)(0xE0 | regnum(a->reg)));
        e8(&e, (uint8_t)b->disp);
        return e.len;
    }
    if ((strcmp(m, "shl") == 0 || strcmp(m, "shr") == 0) && st->bits == 32 && is_reg32(a->reg) && b->kind == OPND_IMM) {
        int ext = strcmp(m, "shl") == 0 ? 4 : 5;
        e8(&e, 0xC1);
        e8(&e, (uint8_t)(0xC0 | (ext << 3) | regnum(a->reg)));
        e8(&e, (uint8_t)b->disp);
        return e.len;
    }
    if ((strcmp(m, "shl") == 0 || strcmp(m, "shr") == 0) && st->bits == 32 && is_reg32(a->reg) && b->reg == R_CL) {
        int ext = strcmp(m, "shl") == 0 ? 4 : 5;
        e8(&e, 0xD3);
        e8(&e, (uint8_t)(0xC0 | (ext << 3) | regnum(a->reg)));
        return e.len;
    }

    if ((strcmp(m, "jc") == 0 || strcmp(m, "jz") == 0 || strcmp(m, "je") == 0 || strcmp(m, "jbe") == 0)) {
        uint8_t cc = strcmp(m, "jc") == 0 ? 0x82 : strcmp(m, "jbe") == 0 ? 0x86 : 0x84; /* jz/je both == */
        if (st->bits == 16) {
            e8(&e, 0x0F); e8(&e, cc);
            int64_t rel = resolve_pcrel(ctx, is_bin, a->sym, a->disp, st->section,
                                          (int64_t)st->offset + 4, st->section, st->offset + 2, strict, 2);
            e16(&e, (uint16_t)rel);
        } else {
            e8(&e, 0x0F); e8(&e, cc);
            int64_t rel = resolve_pcrel(ctx, is_bin, a->sym, a->disp, st->section,
                                          (int64_t)st->offset + 6, st->section, st->offset + 2, strict, 4);
            e32(&e, (uint32_t)rel);
        }
        return e.len;
    }
    if (strcmp(m, "jne") == 0 || strcmp(m, "jl") == 0 || strcmp(m, "jle") == 0 || strcmp(m, "jg") == 0 || strcmp(m, "jge") == 0) {
        uint8_t cc = strcmp(m, "jne") == 0 ? 0x85 : strcmp(m, "jl") == 0 ? 0x8C : strcmp(m, "jle") == 0 ? 0x8E
                     : strcmp(m, "jg") == 0 ? 0x8F : 0x8D; /* jge */
        e8(&e, 0x0F); e8(&e, cc);
        int64_t rel = resolve_pcrel(ctx, is_bin, a->sym, a->disp, st->section,
                                      (int64_t)st->offset + 6, st->section, st->offset + 2, strict, 4);
        e32(&e, (uint32_t)rel);
        return e.len;
    }

    if (strcmp(m, "jmp") == 0) {
        if (a->kind == OPND_FAR) {
            /* EA ptr16:16 (16-bit operand size) or EA ptr16:32 (32-bit) -
             * the segment selector is always 16 bits, but the offset
             * width follows the current bit mode. Using the 16-bit form
             * unconditionally left the far jump 2 bytes short in 32-bit
             * code (e.g. __builtin_gdt_flush's `jmp 0x08:label`, used to
             * reload CS after loading the GDT) - the "segment" half
             * ended up reading the offset's missing high 16 bits
             * instead, and the real segment bytes spilled into the next
             * instruction, corrupting everything after it. */
            e8(&e, 0xEA);
            uint32_t off_patch_off = st->offset + 1;
            int width = st->bits == 16 ? 2 : 4;
            int64_t off = resolve_abs(ctx, is_bin, a->sym, a->disp, st->section, off_patch_off, strict);
            if (width == 2) e16(&e, (uint16_t)off); else e32(&e, (uint32_t)off);
            int64_t seg = a->far_seg_has_sym
                ? resolve_abs(ctx, is_bin, a->far_seg_sym, a->far_seg, st->section, off_patch_off + (uint32_t)width, strict)
                : a->far_seg;
            e16(&e, (uint16_t)seg);
            return e.len;
        }
        if (st->bits == 16) {
            e8(&e, 0xE9);
            int64_t rel = resolve_pcrel(ctx, is_bin, a->sym, a->disp, st->section,
                                          (int64_t)st->offset + 3, st->section, st->offset + 1, strict, 2);
            e16(&e, (uint16_t)rel);
        } else {
            e8(&e, 0xE9);
            int64_t rel = resolve_pcrel(ctx, is_bin, a->sym, a->disp, st->section,
                                          (int64_t)st->offset + 5, st->section, st->offset + 1, strict, 4);
            e32(&e, (uint32_t)rel);
        }
        return e.len;
    }

    if (strcmp(m, "call") == 0 && a->kind == OPND_REG && is_reg32(a->reg)) {
        /* call r/m32 (FF /2) - indirect call through a register, e.g. a
         * function-pointer variable's loaded value (see cc.h's
         * is_indirect_call / docs/toolchain.md). */
        e8(&e, 0xFF);
        e8(&e, (uint8_t)(0xC0 | (2 << 3) | regnum(a->reg)));
        return e.len;
    }
    if (strcmp(m, "call") == 0) {
        if (st->bits == 16) {
            e8(&e, 0xE8);
            int64_t rel = resolve_pcrel(ctx, is_bin, a->sym, a->disp, st->section,
                                          (int64_t)st->offset + 3, st->section, st->offset + 1, strict, 2);
            e16(&e, (uint16_t)rel);
        } else {
            e8(&e, 0xE8);
            int64_t rel = resolve_pcrel(ctx, is_bin, a->sym, a->disp, st->section,
                                          (int64_t)st->offset + 5, st->section, st->offset + 1, strict, 4);
            e32(&e, (uint32_t)rel);
        }
        return e.len;
    }

    if (strcmp(m, "push") == 0) {
        if (a->kind == OPND_IMM) {
            e8(&e, 0x68);
            int64_t v = a->has_sym ? resolve_abs(ctx, is_bin, a->sym, a->disp, st->section, st->offset + 1, strict) : a->disp;
            e32(&e, (uint32_t)v);
            return e.len;
        }
        if (is_reg32(a->reg)) { e8(&e, (uint8_t)(0x50 + regnum(a->reg))); return e.len; }
        if (is_reg16(a->reg)) { e8(&e, (uint8_t)(0x50 + regnum(a->reg))); return e.len; }
    }
    if (strcmp(m, "pop") == 0) {
        if (is_reg32(a->reg) || is_reg16(a->reg)) { e8(&e, (uint8_t)(0x58 + regnum(a->reg))); return e.len; }
    }

    if (strcmp(m, "in") == 0) {
        if (b->kind == OPND_IMM) { e8(&e, 0xE4); e8(&e, (uint8_t)b->disp); return e.len; }
        if (b->reg == R_DX && a->reg == R_AL) { e8(&e, 0xEC); return e.len; }
        /* IN AX,DX needs the 0x66 operand-size override - 0xED alone,
         * in 32-bit code, means IN EAX,DX (the default operand size).
         * Without the prefix this silently read two 16-bit words per
         * call (packed into EAX, high half discarded by the caller's
         * own movzx), draining twice as many words off the ATA data
         * port as intended and dropping every other one - see
         * kernel/ata.c / docs/toolchain.md. */
        if (b->reg == R_DX && a->reg == R_AX) { e8(&e, 0x66); e8(&e, 0xED); return e.len; }
        if (b->reg == R_DX && a->reg == R_EAX) { e8(&e, 0xED); return e.len; }
    }
    if (strcmp(m, "out") == 0) {
        if (a->kind == OPND_IMM) { e8(&e, 0xE6); e8(&e, (uint8_t)a->disp); return e.len; }
        if (a->reg == R_DX && b->reg == R_AL) { e8(&e, 0xEE); return e.len; }
        if (a->reg == R_DX && b->reg == R_AX) { e8(&e, 0x66); e8(&e, 0xEF); return e.len; }
        if (a->reg == R_DX && b->reg == R_EAX) { e8(&e, 0xEF); return e.len; }
    }

    if (strcmp(m, "int") == 0) { e8(&e, 0xCD); e8(&e, (uint8_t)a->disp); return e.len; }
    if (strcmp(m, "lgdt") == 0 && st->bits == 16) {
        /* boot/stage2.S's real-mode form: a direct 16-bit absolute
         * address, hardcoded to that one ModRM byte (mod=00, reg=2,
         * rm=110 "disp16 direct address") - distinct from the 32-bit
         * register-indirect form below, which CC's __builtin_gdt_flush
         * uses instead (`lgdt [ecx]`, loading from a C variable's
         * address held in a register). */
        e8(&e, 0x0F); e8(&e, 0x01); e8(&e, 0x16);
        e16(&e, (uint16_t)resolve_abs(ctx, is_bin, a->sym, a->disp, st->section, st->offset + (uint32_t)e.len, strict));
        return e.len;
    }
    if ((strcmp(m, "lgdt") == 0 || strcmp(m, "lidt") == 0) && st->bits == 32 &&
        (a->kind == OPND_MEM || a->kind == OPND_REG)) {
        /* 0F 01 /2 (lgdt) or /3 (lidt), r/m32 - general memory/register
         * operand via the same encode_rm32 helper every other
         * instruction here uses, e.g. `lgdt [ecx]` / `lidt [ecx]`. */
        e8(&e, 0x0F); e8(&e, 0x01);
        encode_rm32(&e, ctx, is_bin, a, strcmp(m, "lgdt") == 0 ? 2 : 3, st->section, st->offset, strict);
        return e.len;
    }

    if (strcmp(m, "pusha") == 0) { e8(&e, 0x60); return e.len; }
    if (strcmp(m, "popa") == 0) { e8(&e, 0x61); return e.len; }
    if (strcmp(m, "lodsb") == 0) { e8(&e, 0xAC); return e.len; }
    if (strcmp(m, "ret") == 0) { e8(&e, 0xC3); return e.len; }
    if (strcmp(m, "hlt") == 0) { e8(&e, 0xF4); return e.len; }
    if (strcmp(m, "cli") == 0) { e8(&e, 0xFA); return e.len; }
    if (strcmp(m, "cld") == 0) { e8(&e, 0xFC); return e.len; }
    if (strcmp(m, "rep") == 0 && strcmp(a->sym, "stosb") == 0) { e8(&e, 0xF3); e8(&e, 0xAA); return e.len; }

    fprintf(stderr, "as: line %d: unsupported instruction form: %s\n", st->line_no, m);
    exit(1);
}

int as_encode_pass2(asm_ctx_t *ctx) {
    for (int i = 0; i < ctx->nstmts; i++) {
        stmt_t *st = &ctx->stmts[i];
        section_kind_t sec = st->section;
        uint8_t *dst = ctx->sec_bytes[sec] + st->offset;
        int len = stmt_encode(ctx, st, dst, 1);
        if ((uint32_t)len != st->length) {
            fprintf(stderr, "as: internal error: length mismatch for '%s' at line %d (pass1=%u pass2=%d)\n",
                    st->mnemonic, st->line_no, st->length, len);
            return -1;
        }
        if (st->offset + (uint32_t)len > ctx->sec_len[sec]) {
            ctx->sec_len[sec] = st->offset + (uint32_t)len;
        }
    }
    return 0;
}
