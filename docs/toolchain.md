# Self-hosted toolchain

`toolchain/` replaces `nasm`/`gcc`/`ld`/`objcopy` with our own tools, so
the OS build depends on external infrastructure only once - to bootstrap
the toolchain itself from source (see [Bootstrap](#bootstrap) below).
`qemu-system-i386` stays external; it's the machine we run on, not part
of what builds for it.

## AS (assembler)

NASM-syntax-compatible, but deliberately scoped to exactly the
instruction/directive/operand forms the four `.S` files in this repo use
- not general x86 or general NASM compatibility. Extending this list is
  expected if a future `.S` file needs more; it is not meant to be
  exhaustive of x86.

No macro preprocessor (`%macro`/`%rep`/`%assign`/`%+`) - the one place
that used one (`kernel/idt_stubs.S`) was hand-expanded instead.

### Directives

`BITS 16` / `BITS 32` (a file may switch modes partway through, as
`stage2.S` does for the real-mode -> protected-mode transition), `ORG`,
`EQU` (constant expressions, including label arithmetic like
`gdt_end - gdt_start`), `section NAME`, `global NAME`, `extern NAME`,
`align N`, `times EXPR db|dw|dd|dq ...`, `db`/`dw`/`dd`/`dq` (each
accepting a comma-separated list of immediate/string/expression values),
`resb N`.

### Expressions

Decimal and `0x`-hex integer literals, binary literals with a `b` suffix
(`10011010b`), the current-position pseudo-symbols `$` (this address) and
`$$` (start of the current section/file), label references, and `+ - * /
>> <<` with parentheses. Must constant-fold at assemble time for `EQU`,
`times`, and data-directive values; may instead resolve to a relocation
when used as an instruction operand referencing a symbol whose final
address isn't known until link time (see [relocations](#relocations)).

### Labels

Global labels (`name:`) and NASM-style local labels (`.name:`, scoped to
the most recent preceding global label - `.hang`, `.loop`, etc. appear
identically named in multiple places across these files and must not
collide).

### Instructions

Every mnemonic/operand-form pair actually used, grouped by kind:

- **Data movement**: `mov` between 8/16/32-bit registers, segment
  registers (`ds`/`es`/`fs`/`gs`/`ss`), memory (`[label]`, `[label +
  disp]`, `[reg]`, `[reg+disp]`), and immediates/constant-expressions,
  with explicit `byte`/`word`/`dword` size overrides where the operand
  size isn't otherwise inferable; `movzx` (16->32 zero-extend);
  `push`/`pop` (16/32-bit register); `pusha`/`popa`; `lodsb`.
- **Arithmetic/logic**: `add`, `sub`, `cmp`, `or`, `xor`, `shl` - register
  or memory destination, register/immediate source.
- **Control flow**: `jmp` (near, to a label or a bare absolute address
  constant; far, `SEG:offset` with an immediate/EQU segment and a label
  offset), `call` (near, to a label), `ret`, `je`/`jz`/`jc`/`jbe`
  (near, to a label).
- **I/O and privileged**: `in`/`out` (`al`/`ax` with an 8-bit immediate
  port or `dx`), `int` (immediate vector), `cli`, `hlt`, `lgdt [label]`,
  `mov` to/from `cr0`.
- **String/prefix**: `cld`, `rep stosb`.

### Output formats

- `-f bin`: one flat byte stream at the file's `ORG` address (our `.S`
  files needing this mode never declare `section`, so there's no
  section-merging logic to speak of - just linear emission).
- `-f elf32`: a relocatable ELF32 object with real sections
  (`.text`/`.text.entry`/`.data`/`.bss`), a symbol table (globals from
  `global`, undefined externs from `extern`), and relocations for
  anything not resolvable within the same section of the same file at
  assemble time.

### Relocations

Same-section, same-file references (e.g. `jmp .hang`, `jmp isr_common`)
are resolved directly by `AS` as fixed relative offsets - no relocation
entry needed, same as any assembler does internally. A relocation is
emitted only when the target's final address isn't known until link
time: `R_386_PC32` for relative `call`/`jmp` to an `extern` symbol
(`call kmain`, `call isr_handler`), `R_386_32` for an absolute address
used as data or an immediate where the symbol lives in a different
section or is `extern` (`kernel/idt_stubs.S`'s `isr_stub_table`, which is
in `.data` and holds the absolute addresses of `.text` labels;
`kernel/entry.S`'s `mov edi, _bss_start`, where `_bss_start`/`_bss_end`
are defined by `LD`'s linker-script-equivalent layout logic, not by any
`.S`/`.c` file - see `LD`'s docs section once written).

### Verification

`AS` deliberately never chooses a shorter jump encoding the way NASM's
optimizer does (`jmp`/`jcc` always use the fixed-size near form, never
`EB`/short-`rel8`) - this keeps the encoder simple at the cost of
slightly larger code, so its output for `boot.S`/`stage2.S` is *not*
byte-identical to NASM's. Verification is therefore functional rather
than a byte diff: all four `.S` files (`boot.S`, `stage2.S` via `-f bin`;
`entry.S`, `idt_stubs.S` via `-f elf32`, linked with GNU `ld` as an
interim step until `LD` exists in Phase T5) were assembled with `AS` and
the resulting kernel boots to a working shell with `SELFTEST PASS` in
QEMU - the same bar `tools/run_tests.sh` holds the rest of the OS to.
The `-f elf32` relocations were additionally spot-checked directly:
`isr_stub_table`'s `R_386_32`-relocated entries resolve to exactly the
linked `isr_stub_N` symbols' addresses (verified via `nm`/`objdump`).

## CC, LD, OBJCOPY

Documented as each phase lands - see the plan
(`docs/toolchain.md` grows alongside `toolchain/cc`, `toolchain/ld`,
`toolchain/objcopy`).

## Bootstrap

_To be filled in once Phase T8 lands: the Stage0/1/2/3 procedure and how
to re-verify Stage2 == Stage3._
