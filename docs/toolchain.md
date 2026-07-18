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

Every mnemonic/operand-form pair actually used, grouped by kind. The
original set (needed by the four hand-written `.S` files, all 16-bit
real-mode or the small fixed 32-bit sequences in `entry.S`/`idt_stubs.S`)
and the set CC's codegen added in Phase T4 (general 32-bit reg/mem forms)
are both listed together here since they share one encoder
(`toolchain/as/encode.c`), gated by addressing mode (`[esp+disp]` via
SIB, `[ebp+disp]` always as the disp32 form since `mod=00,rm=101` means
"disp32-direct, no base" rather than "EBP with no displacement", `[reg]`
for any other general register, `[label(+disp)]` as 32-bit disp32-direct
- see [relocations](#relocations)) rather than by which file originally
needed them:

- **Data movement**: `mov` between 8/16/32-bit registers, segment
  registers (`ds`/`es`/`fs`/`gs`/`ss`), memory, and immediates/constant-
  expressions, with explicit `byte`/`word`/`dword` size overrides where
  the operand size isn't otherwise inferable; `movzx` (8/16 -> 32
  zero-extend, register or memory source); `lea`; `push`/`pop` (16/32-bit
  register, or an imm32); `pusha`/`popa`; `lodsb`.
- **Arithmetic/logic**: `add`, `sub`, `and`, `or`, `xor`, `cmp` (reg/mem
  destination, reg/mem/imm source), `imul` (two-operand, `r32, r/m32`),
  `idiv`/`cdq`, `not`/`neg`, `shl`/`shr` (by an imm8 or by `cl`).
- **Comparison results**: `sete`/`setne`/`setl`/`setle`/`setg`/`setge`
  (r/m8 destination - CC always follows one with `movzx` back to 32-bit).
- **Control flow**: `jmp` (near, to a label or a bare absolute address
  constant; far, `SEG:offset` with an immediate/EQU segment and a label
  offset), `call` (near, to a label), `ret`, the full signed/unsigned-
  agnostic conditional jump set CC needs (`je`/`jz`/`jne`/`jc`/`jbe`/
  `jl`/`jle`/`jg`/`jge`), all near-to-a-label.
- **I/O and privileged**: `in`/`out` (`al`/`ax` with an 8-bit immediate
  port or `dx`), `int` (immediate vector), `cli`, `hlt`, `lgdt`/`lidt`
  (`[label]` or `[reg]`), `mov` to/from `cr0`.
- **String/prefix**: `cld`, `rep stosb`.

No jump-size optimization anywhere (near/far forms are always the fixed-
size encoding - see [Verification](#verification)), and no `xchg` - CC's
codegen always evaluates operands in an order that already puts values in
the right registers, so a register swap is never needed.

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

## CC (compiler)

`toolchain/cc`: preprocessor + lexer + parser + naive x86-32 codegen for
"tinyC" - a scoped C dialect, not general C89/C99. Emits `AS`-syntax
assembly text (mirrors `gcc -S | as`); two targets selected by
`--target=kernel` (freestanding, no runtime) or `--target=host` (adds a
`_start` that calls `main` then exits via a raw syscall). Native host
tool, not OS code.

### Preprocessor

`#include` (quote form searches the including file's directory first,
then falls back to `-I` search paths, exactly reproducing the trick
`kernel/fs/tinyfs.c` relies on to pick kernel vs. host `string.h`; angle
form searches only `-I` paths, using dedicated header-name lexing since
`<foo.h>` isn't a normal token), `#pragma once` (dedup by resolved path),
and simple **object-like** `#define` with recursive constant
substitution (one level of nested macro expansion, e.g.
`TINYFS_INODE_BLOCK` expanding to `(TINYFS_BITMAP_BLOCK + 1)` whose own
pieces also expand) - no function-like macros, no `#ifdef`/`#if`, since
nothing in this codebase uses them.

### Types

`void`, `char`, `short`, `int` (no `long`/`long long` - nothing in this
codebase needs a type wider than 32 bits, which keeps the type-specifier
grammar smaller), `signed`/`unsigned`, pointers, arrays, `struct`/
`union`/`enum`, function types (including function-pointer typedefs -
`typedef void (*fn_t)(int);` - the one parenthesized declarator form
supported; arbitrary nested declarators like pointer-to-array aren't,
since nothing here needs them), `typedef`. `const`/`volatile`/`inline`
are parsed and ignored; `static`/`extern` affect linkage
(`global`/no-`global` in the emitted asm) and, for locals, storage
(stack frame vs. a dedicated `.data`/`.bss` label). `char` always behaves
as unsigned (zero-extending loads) - simpler than tracking C's
implementation-defined signedness, and nothing here relies on a
negative `char`. `__attribute__((packed))` on a struct/union removes all
inter-member padding.

Struct/union layout is computed *eagerly*, the moment a definition's `}`
(and any trailing `__attribute__`) is parsed, not lazily on first
`sizeof`/use - `.`/`->` member lookups elsewhere in the file need real
offsets immediately, and nothing else was guaranteed to trigger layout
first (a real bug caught by `tests/toolchain/t05_structs.c`: struct
member access silently used offset 0 for every member until whatever
first called `sizeof` on that type - see the fix in `parser.c`).

### Expressions/statements

The full standard precedence chain (assignment/compound-assignment,
ternary, `||`/`&&` short-circuit, bitwise, equality, relational, shift,
additive, multiplicative, unary, postfix), `sizeof` (type or expression),
casts, pointer/array indexing and address-of/dereference (array indexing
is lowered directly to `*(arr + idx)` in the parser, so codegen never
needs a separate indexing case), struct member access (`.`/`->`),
function calls. `if`/`else`, `while`, `do`/`while`, `for`, `return`,
`break`/`continue`, blocks. No `switch`, no `goto`/labels, no comma
operator, no variadic functions, no K&R-style declarations - unused
anywhere in this codebase.

Brace initializer lists (`{1, 2, 3}`) are only supported for globals/
`static` locals (compile-time-constant `.data` bytes) - this codebase
never brace-initializes a plain stack local, only scalars, so codegen
never needs to lower a brace list into runtime store instructions.
Compound literals (`(type){...}`) and designated initializers
(`[i] = x`, `.field = x`) aren't supported at all - the two call sites
that used them (`kernel/lang/script/eval.c`'s `mkint`/`mkstr`
constructors, `kernel/lang/compiler/codegen.c`'s `binop_opcode`
function) were refactored to avoid them in Phase T3, a deliberate
scope-narrowing decision rather than an oversight.

### Codegen

Naive and non-optimizing on purpose (see the plan's Phase T4 entry):
every expression's result lives in `EAX`; a binary operator evaluates
its right operand first and `push`es it, then its left operand into
`EAX`, then `pop`s the right operand into `ECX` and combines - no
register allocation, values spill to the real machine stack between
every sub-expression. cdecl calling convention (caller pushes args
right-to-left, callee's `EBP+8`/`+12`/... for params, return value in
`EAX`, caller cleans up the pushed args). Locals live at fixed `EBP`-
relative offsets assigned as their declarations are parsed; every slot
is a 4-byte-aligned minimum of 4 bytes regardless of the C type's actual
size (simplicity over density). `static` locals get their own unique
`.data`/`.bss` label instead of a stack slot, exactly like globals.

### Builtins (`builtins.c`)

Fixed instruction sequences in place of a general GCC-extended-inline-
asm parser - the kernel's actual inline-asm usage reduces to exactly
these operations (see the plan's research notes). Recognized by name at
a call site, in place of an ordinary `call`; arguments are evaluated
through the normal expression codegen first, then moved into whatever
fixed registers the underlying instruction needs:

| builtin | x86 |
|---|---|
| `__builtin_inb(port)` / `__builtin_outb(port, val)` | `in al, dx` / `out dx, al` |
| `__builtin_inw(port)` / `__builtin_outw(port, val)` | `in ax, dx` / `out dx, ax` |
| `__builtin_hlt()` | `hlt` |
| `__builtin_lidt(ptr)` | `lidt [ptr]` |
| `__builtin_gdt_flush(ptr)` | `lgdt` + reload every data/stack segment to selector `0x10` + far-jump to selector `0x08` to reload `CS` - the one sequence that inherently can't be a single instruction, needs a fresh label per call site (own counter, `.Lbi` prefix so it never collides with codegen's own `.L` control-flow labels) |
| `__sys_open/read/write/lseek/close/exit` | raw `int 0x80` Linux x86-32 syscalls, host target only - see [Bootstrap](#bootstrap) for why host tools are libc-free |

### Verification

`tests/toolchain/*.c` + `tools/run_toolchain_tests.sh`: each fixture
compiles through `CC --target=host` + `AS`, links with GNU `ld` (interim,
until `LD` exists in Phase T5), runs directly, and checks its exit code
- fast host-side iteration covering arithmetic, control flow, recursion,
pointers/arrays, structs/unions/packed layout, and function pointers.
Three real bugs were caught this way (not just compile-clean, actually
wrong-answer or crashing): the eager-struct-layout bug above; a missing
`[reg]`/`[reg+disp]` addressing case in `AS`'s generic 32-bit encoder
(fell through to the "bare label" case, corrupting the address - caught
by a standalone `AS` extension smoke test before CC even ran through
it); and an ELF symbol table bug where a defined symbol's section index
was computed as a fixed offset from its internal `section_kind_t`
instead of the section's *actual* assigned ELF header index - wrong the
moment a file didn't use all four section kinds in order (e.g. `.bss`
with no `.data`), silently pointing every reference at the wrong
section and segfaulting at run time.

Kernel-target builtins (port I/O, `hlt`) can't run on the host, so
they're verified separately: a small kernel-target program that polls
COM1's line-status register and writes bytes via `__builtin_inb`/
`__builtin_outb`, linked with the *real* `kernel/entry.S` and booted
through the *real* `boot.S`/`stage2.S` in QEMU, printed its expected
output over serial.

## Kernel source adaptation (Phase T3)

The kernel source now expresses every low-level operation the way CC
expects it, ahead of Phase T7's actual switchover - so this phase's
changes are visible today even though the build still uses host
gcc/nasm/ld/objcopy:

- All 12 inline-asm sites (`kernel/{ata,serial,keyboard,idt,gdt}.c`,
  `kernel/shell/shell.c`) now call named builtins declared in
  `kernel/builtins.h` (`__builtin_outb`/`inb`/`outw`/`inw`/`hlt`/
  `lidt`/`gdt_flush`) instead of raw `__asm__` blocks.
- `tools/mkfs_tinyfs.c` is libc-free: raw `__sys_open`/`read`/`write`/
  `lseek`/`close`/`exit` (declared in `tools/host_syscalls.h`) instead
  of `fopen`/`fread`/`fwrite`/`fseek`/`fclose`/`exit`/`perror`, and
  `kernel/string.h` instead of the system `<string.h>` (which needed
  adding `strchr` there, used to split `host-path:fs-name` seed specs).

Since GCC doesn't recognize any of these names as its own builtins, real
GCC-inline-asm bodies live in two small shim files -
`kernel/builtins_shim.c` and `tools/host_syscalls_shim.c` - that exist
*only* so the current interim build still links. Both get dropped from
the Makefile in Phase T7: CC intercepts every call to these names
directly in codegen and never needs (or could parse - CC has no
inline-asm support at all) a linkable definition. The "Builtins" section
above documents the actual instruction sequences CC emits for each one;
the shims mirror them exactly (e.g. `__sys_open`'s hardcoded mode `420`)
so behavior is identical either way.

Verified with the *existing* toolchain, deliberately - this phase proves
the refactor is behavior-preserving before the new toolchain is even in
the loop: `tools/run_tests.sh` (full OS boot + shell + both languages)
and `tools/run_toolchain_tests.sh` (CC's own test battery) both stay
green throughout, built entirely with gcc/nasm/ld/objcopy as before.

## LD, OBJCOPY

Not yet built - Phases T5/T6. Everything so far links with GNU `ld` and
extracts with GNU `objcopy` as an interim step.

## Bootstrap

_To be filled in once Phase T8 lands: the Stage0/1/2/3 procedure and how
to re-verify Stage2 == Stage3._
