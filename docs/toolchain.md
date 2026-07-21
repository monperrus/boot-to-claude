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

## LD (linker, Phase T5)

`toolchain/ld/` parses ELF32 relocatable objects (`elf_reader.c`, sections
indexed directly by their raw ELF section-header index so symbol
`st_shndx` / relocation `sh_info` map straight in), merges same-kind
sections across all input files **in command-line order** (`main.c`'s
`merge_sections`), resolves symbols into one flat global table, and
applies the two relocation types this project's object code actually
produces: `R_386_32` (`value = S+A`) and `R_386_PC32` (`value = S+A-P`) -
x86 ELF uses REL not RELA, so the addend `A` is read back out of the
bytes AS already baked in, not stored separately.

File-order concatenation replaces a real linker script's section-pattern
matching (`*(.text.entry) *(.text*)` etc.) - the one simplification this
phase relies on, and it only works because the Makefile already puts
`entry.o` first on the command line. `classify_section()` matches
section names by **prefix** (`.text`, `.rodata`, `.data`, `.bss`), not
exact string - needed because nasm names `entry.S`'s section
`.text.entry`, not `.text`; an exact match silently dropped `_start`
from the link (caught by testing against the current *nasm*-built
objects, not just AS's own output).

Two output modes, sharing the merge/resolve/relocate core above:

- `link_flat` (`-T <script>`, script contents never read - its mere
  presence is the mode switch): hardcodes `kernel/link.ld`'s exact
  layout, base `0x10000`, `.text`/`.rodata`/`.data`/`.bss` 4-byte-aligned
  in sequence, injects `_bss_start`/`_bss_end`. Two writers share this
  layout: `write_flat_elf` produces an `ET_EXEC` with real section
  headers (useful for `nm`/`objdump` while debugging), and
  `write_flat_bin` (`--format bin`) writes the same loadable bytes
  straight to a file with no ELF wrapper at all - what the bootloader
  actually reads off disk. There is no separate OBJCOPY tool: LD already
  has the bytes laid out in memory before write_flat_elf wraps them in
  headers, so write_flat_bin is a direct writer, not an ELF-parsing
  extractor. Verified byte-identical to `write_flat_elf`'s output piped
  through GNU `objcopy -O binary`, and separately boot-tested to
  `SELFTEST PASS`.
- `link_static_exec` (default): a minimal real Linux ET_EXEC for host
  tools - base `0x08048000`, one RWX `PT_LOAD` segment covering the
  whole file, `.bss` covered only by `p_memsz > p_filesz`, **no section
  header table** (`e_shoff=0` - `execve()`'s loader never reads them).
  Entry point is the linked `_start` symbol; errors if none is found
  (only CC `--target=host` output ever defines one).

Verified functionally, not by byte-diff (ELF metadata/padding legitimately
differs from GNU ld's output): the full `tests/toolchain/*.c` battery
linked by LD and run directly (matching Phase T4's expected exit codes);
the kernel linked by LD from a mix of nasm- and AS-assembled objects,
booted in QEMU to `SELFTEST PASS` and a working interactive shell
(scripting language, bytecode compiler+VM, tinyfs - via
`tools/qemu_type.py`); and `tools/mkfs_tinyfs` itself - `kernel/fs/tinyfs.c`,
`kernel/string.c`, and `tools/mkfs_tinyfs.c` compiled by `CC
--target=host`, assembled by AS, linked by LD in `link_static_exec` mode,
then actually run to format a real disk image, which then boots and
passes the full `tools/run_tests.sh` suite.

That last check surfaced seven real, previously-latent bugs - none of them
in LD itself, but in CC/AS paths LD's static_exec mode was the first
thing to actually exercise end-to-end:

- **CC preprocessor**: `#include`'s per-depth path buffer
  (`static char paths[MAX_INCLUDE_DEPTH][600]`) was reused by every
  sibling include at the same nesting depth, and `token_t.file` stores
  that pointer directly (no copy) - a later sibling `#include`
  overwriting the buffer retroactively "changed" the filename recorded
  on every earlier token from the previous sibling. Fixed by giving each
  include its own permanently-allocated path. Also, macro-in-macro
  expansion only substituted one level deep (`TINYFS_DATA_BLOCK` ->
  `(TINYFS_INODE_BLOCK + ...)` left `TINYFS_INODE_BLOCK` unexpanded) -
  replaced with a proper recursive expander guarded against
  self-reference.
- **CC codegen - `_start`**: emitted unconditionally in every
  `--target=host` translation unit, so a multi-file host program
  (`mkfs_tinyfs.c` + `tinyfs.c` + `string.c`) got a `call main` baked into
  files that don't define `main`, which AS then couldn't resolve
  (there's no separate crt0.o in this toolchain). Now only emitted by the
  one unit that actually defines `main`. Relatedly, `_start` originally
  did a bare `call main` - `call` itself pushes a return address, so
  `argc` landed in the right cdecl slot by coincidence but `argv`
  received `argv[0]`'s string-pointer value instead of `&argv[0]`; fixed
  by loading argc/argv explicitly from `[esp]`/`[esp+4]` before the call.
- **CC codegen - top-level string/extern collection**: `walk_collect`
  recurses into `->next` (needed for statement sibling chains reached via
  `->list`), but the top-level driver in `codegen_emit` *also* iterates
  `n = n->next` while calling `walk_collect(n, prog)` on each node - so
  each call re-walked the entire remaining top-level chain, duplicating
  every string literal and extern once per remaining prefix.
- **CC codegen - static locals never allocated**: the `.data`/`.bss`
  emission loops only scanned `prog`'s top-level chain, but a `static`
  local variable is declared inside a function body's statement list -
  so `lea eax, [__static_N]` was emitted at every use site with no
  matching `global __static_N` / storage anywhere, an unresolvable
  symbol. Fixed by collecting static locals during the same recursive
  walk used for strings/externs.
- **CC codegen - function values didn't decay to a pointer**: a bare
  function name used as a value (`tinyfs_init(disk_block_read, ...)`,
  passing a callback) went through the same `ND_VAR` path as an ordinary
  global - compute its address, then *dereference* it - which read the
  function's own machine code bytes as if they were a stored pointer.
  Fixed by skipping the dereference for `TY_FUNC`-typed values.
- **CC codegen - no indirect calls**: calling *through* a function-pointer
  variable (`block_read(...)` where `block_read` is a `static
  tinyfs_block_read_fn`, assigned via `tinyfs_init`) always emitted a
  direct `call block_read` - a jump straight into the `.bss` bytes
  holding the pointer value, executed as instructions. The parser now
  detects when a call's callee resolves to a variable rather than a
  function symbol and marks the call indirect; codegen loads the stored
  pointer and emits `call eax`. AS gained the corresponding `call r/m32`
  (`FF /2`) encoding - previously only the direct relative form
  (`E8 rel32`) existed.
- **AS - 3-operand `imul` silently dropped its immediate**: the operand
  parser had a hard 2-operand ceiling (`ops[2]`, no comma check after the
  second operand), so `imul ecx, ecx, 4` (CC's array-indexing scale, e.g.
  `argv[1]`) parsed as `ecx, ecx` with `, 4` silently discarded, then
  encoded as the 2-operand register-multiply form (`ecx = ecx*ecx`
  instead of `ecx*4`). This is a general, silent miscompilation of any
  runtime-indexed array/pointer access with a non-constant-folded scale -
  it went undetected by `tests/toolchain/t04_pointers.c` because that
  test's array *writes* and *reads* both use the identical broken
  formula (self-cancelling: `nums[i] = i*2` stores at `base+i*i`, and
  `a[i]` in the summing loop reads back from `base+i*i` too), so the
  final sum still came out right despite the underlying addresses being
  wrong. It only surfaced when reading data written through a *different*
  mechanism using real offsets - the kernel-provided `argv[]` array.
  Fixed by extending `stmt_t.ops[]` to 3 slots and adding the proper
  `imul r32, r/m32, imm32` encoding (`0x69 /r id`, always full imm32 -
  same "no imm8 sign-extend optimization" convention the group1 ALU ops
  already use).

## Full switch-over (Phase T7)

The Makefile now builds `toolchain/{as,cc,ld}` from source with the
host's real `gcc` (`BOOTSTRAP_CC`, invoked exactly once, only on that
source - see `docs/toolchain.md`'s Bootstrap section below), then uses
those binaries for every kernel and host-tool source file. `AS`/`CC`/`LD`
point at `$(BUILD)/toolchain/{as,cc,ld}`; `HOSTCC` is `CC --target=host`.
Since CC never emits object files directly (only AS-syntax assembly
text), every C source now goes through two steps where gcc's `-c` used
to be one: `$(BUILD)/%.S: kernel/%.c` (CC) then `$(BUILD)/%.o:
$(BUILD)/%.S` (AS) - GNU Make's implicit-rule search resolves this
alongside the unrelated `$(BUILD)/%.o: kernel/%.S` rule for the two
hand-written assembly files (`entry.S`/`idt_stubs.S`) without conflict,
since the two rules' prerequisite directories never contain the same
stem. `kernel/builtins_shim.c` and `tools/host_syscalls_shim.c` are
gone - no longer referenced anywhere - since CC intercepts every
`__builtin_*`/`__sys_*` call site directly (see the CC section above).

Verified with the full existing test suite: `tools/run_tests.sh` (boot,
tinyfs mount, every shell command, the scripting language, the bytecode
compiler+VM) and `tools/run_toolchain_tests.sh`, both green, with zero
invocations of gcc/nasm/ld/objcopy anywhere in the OS build path.

Getting there surfaced a long tail of real bugs - unlike Phase T5's
handful, most of these were latent because nothing had ever exercised
the exact code pattern before (a kernel-only builtin, a language feature
only the scripting interpreter happened to use, an instruction only one
driver emitted). Each was found by an actual QEMU boot/GDB session
against real, previously-untested code, not by inspection:

- **AS - `lgdt`/`lidt` only supported one operand form**: hardcoded to
  the 16-bit absolute-address form `boot/stage2.S` uses; `__builtin_gdt_flush`'s
  `lgdt [ecx]` (register-indirect, 32-bit) silently mis-assembled.
  Added the general `encode_rm32`-based `0F 01 /2` (`lgdt`) and `/3`
  (`lidt`) form alongside the existing one.
- **AS - far jump (`jmp SEG:OFFSET`) always used a 16-bit offset**: in
  32-bit code (`__builtin_gdt_flush`'s CS-reload jump) this is 2 bytes
  short of the real `ptr16:32` encoding, corrupting every byte after it.
  Fixed by sizing the offset from `st->bits`, matching every other
  16-vs-32 branch in the encoder.
- **CC codegen - `walk_collect`'s extern-duplication fix had a second
  bug**: temporarily NULLing a top-level node's `->next` to stop
  re-walking the whole remaining chain (Phase T5's fix) also corrupted
  `is_local`'s scan of `prog` *while that scan was still in progress*,
  whenever it reached the currently-truncated node - any same-file
  function called from code defined *earlier* in the file looked
  "not local" and got a spurious `extern`, later colliding with its own
  real definition. Replaced the mutation with `walk_collect_ex(...,
  skip_next)`, which never touches the shared list at all.
- **CC codegen - indirect-call detection didn't exclude the extern-
  function-name pass**: `block_read(...)` (call through a function-
  pointer variable) still ran through the "not a locally-defined
  function, so `extern` it" check, `extern`-declaring the *variable*
  name - which then also got a real local `global` definition in the
  same file, leaving AS unable to tell which one a `lea eax,
  [block_read]` reference meant. Skipped for `is_indirect_call` calls.
- **CC codegen - file-scope `static` variables/functions were always
  global**: `parse_global_var` never looked at the `static` keyword
  (only `parse_func` did), so two files' own file-private `static int
  foo;` would collide as duplicate global symbols. Added
  `is_file_static`, mirroring the function case.
- **CC - `switch`/`case`/`default` weren't implemented at all**: not a
  bug so much as a real gap Phase T2's dialect survey missed (used by
  `kernel/lang/lexer.c`'s tokenizer switch, among others) - `switch`
  parsed as an unknown identifier followed by a call, producing garbage.
  Added as a flat list of case/default label markers inside the switch's
  block (parsed the same as any other block), with a linear
  compare-and-jump chain and a hidden per-switch frame slot holding the
  switch value across the chain - naturally gives C's fallthrough
  semantics for free, since falling off the end of one case's code just
  runs into the next label in emission order.
- **CC - the comma operator was declared (`ND_COMMA` existed) but never
  actually implemented** - `parse_expr` was just `parse_assign`, so a
  for-loop's multi-clause increment (`i++, j--`) silently dropped
  everything after the first comma, desyncing the parser from that point
  on. Added real comma-operator parsing (distinct from comma-as-
  separator in call args/declarators/initializers, which already call
  `parse_assign` directly per element) and trivial codegen (evaluate
  and discard the left side, keep the right).
- **CC - self-referential and forward-declared struct tags didn't
  work**: a struct's own tag was only registered *after* its body was
  parsed, so `struct node { struct node *next; }` failed to resolve its
  own self-reference; and `typedef struct node node_t;` with no body
  (forward declaration before the real definition, `kernel/lang/script/ast.h`'s
  exact pattern) had no path but "error" at all. Fixed by registering
  the tag before parsing the body (reusing the same `type_t` if one
  already exists, so an earlier forward declaration's typedef still sees
  the real member layout once the full definition arrives) and by
  creating an incomplete tag on first forward-reference instead of
  erroring.
- **CC - function-pointer *parameters* weren't parsed**:
  `parse_declarator` already handled `T (*name)(params)` for ordinary
  declarations, but `parse_param_list` had its own simpler inline
  parameter parser that never checked for the parenthesized form (used
  by `kernel/lang/compiler/parser.c`'s `parse_binop_level(parser_t *p,
  ast_node_t *(*next)(parser_t *), ...)`). Added the same form directly
  to the parameter-parsing loop.
- **AS - `movzx r32, r16` (register-to-register) had a spurious `0x66`
  prefix**: legal-looking but wrong - the prefix reinterprets *both*
  operands as 16-bit, silently downgrading `movzx eax, ax` to a
  near-no-op `movzx ax, ax` instead of really zero-extending into the
  full register. Never exercised before: every earlier `movzx` use was
  the byte-from-memory form (`0F B6`); only CC's new `uint16_t` load path
  (`mov ax, [eax]` then `movzx eax, ax`, added this phase - see below)
  hit the register-to-register form (`0F B7`) at all.
- **CC codegen - no 16-bit (`short`/`uint16_t`) load/store support**:
  `load_value`/`store_value` only distinguished byte (1) from
  "everything else, 4 bytes" - a `uint16_t` array element write emitted
  a plain 4-byte `mov`, silently overwriting the next element's first
  two bytes on every store. This is exactly `kernel/ata.c`'s PIO loop
  (`uint16_t *p = buf; p[i] = inw(ATA_DATA);`), which had never been
  compiled by CC before. Added real 16-bit forms to both, plus the
  matching `mov r16, r/m16` / `mov r/m16, r16` encodings in AS (only a
  16-bit-code, bare-label form existed before, for `boot/stage2.S`).
- **AS - `in`/`out` with a 16-bit accumulator (`in ax, dx` / `out dx,
  ax`) also needed the `0x66` prefix and didn't have it** - `0xED`/`0xEF`
  alone, in 32-bit code, means the *32-bit* accumulator form. Every
  `inw()`/`outw()` call (ATA sector transfers, 256 words per sector) was
  silently reading/writing **two** 16-bit port words per instruction
  instead of one, packed into EAX with the low half kept and the high
  half discarded by the caller's own `movzx` - draining twice as many
  words off the device as intended and dropping every other one. This
  produced the session's most confusing bug: correct-looking code,
  correct-looking disassembly, and a superblock read that silently came
  back with every other 16-bit word missing.
- **CC - `T name[] = { ... }` (array size inferred from its
  initializer) left `array_len` at -1** in both the global
  (`parse_global_var`) and local (`parse_one_local_decl`, needed for
  function-local `static const T ops[] = {...}` tables) declaration
  paths - `type_size` treats a non-positive `array_len` as size 0, so
  `sizeof(name)` silently came out as 0. `kernel/keyboard.c`'s scancode
  table used exactly this pattern for its `TABLE_LEN` bounds check
  (`sizeof(lower_table)/sizeof(lower_table[0])`); with the check always
  seeing `TABLE_LEN == 0`, `code >= TABLE_LEN` was vacuously true for
  every scancode, so `keyboard_getchar` detected every keypress
  correctly but silently discarded all of them instead of ever
  returning one. Fixed by counting the initializer list's elements once
  parsing reaches it, in both places.
- **CC - struct/union-by-value wasn't supported at all**: assignment
  (`a = b;`), a non-brace initializer (`T save = *p;` -
  `kernel/lang/script/parser.c`'s assignment-lookahead backtracking:
  `parser_t save = *p; ... *p = save;`), function return
  (`kernel/lang/script/eval.c`'s `mkint`/`mkstr` constructors), and
  by-value parameters (`print_value(value_t v)`) all silently used the
  scalar 4-byte `mov` path, corrupting or truncating anything bigger
  than a pointer. This was the session's deepest fix: `load_value` now
  leaves a struct/union as its own address instead of dereferencing it
  (the same "decay" `TY_ARRAY` already got), `store_value` gained a
  byte-copy loop (`codegen_struct_copy` - AS has no `rep movsb`, so it's
  a plain indexed compare-and-loop like every other loop this codegen
  emits) for struct/union targets, function returns use the classic
  hidden-pointer convention (caller allocates a temp slot, passes its
  address as an implicit first argument at `[ebp+8]`, shifting every
  real parameter's offset out by 4 - `parse_func`/`ND_RETURN`/
  `codegen_call_or_builtin` all coordinate on this), and by-value
  parameters are copied onto the stack at the call site instead of
  having their address pushed, with `parse_func`'s parameter-offset
  loop sized per-parameter instead of assuming a flat 4 bytes. (AS's
  `MAX_STMTS`/`MAX_SYMS`/`MAacross_RELOCS` also had to grow - the
  struct-copy loop's handful of instructions per copy added up fast in
  `eval.c`, which does little else.)
- **CC lexer - `read_escape` had no case for `\b` (or `\f`/`\v`/`\a`)**:
  fell through to the `default: return c;` branch, so `'\b'` silently
  lexed to the *letter* `'b'` (0x62) instead of backspace (0x08). This
  broke two unrelated things built from the same character constant:
  `kernel/keyboard.c`'s scancode table entry for the real Backspace key
  (now mapped to the letter b), and - far more visibly -
  `kernel/shell/shell.c`'s `read_line`'s `if (c == '\b')` check, which
  became `if (c == 'b')`: typing the *letter* b anywhere in a shell
  command made `read_line` treat it as a backspace keypress and erase
  the previous character instead of appending 'b'. Found via the exact
  symptom: every shell command in the test suite worked except the one
  containing a 'b' (`build hello.tc hello.tbc`), consistently, regardless
  of keystroke timing - which ruled out a timing race and pointed at the
  character itself.

## Bootstrap (Phase T8 - self-hosting)

`tools/bootstrap_selfhost.sh` is the whole procedure, and it's what the
Makefile's `$(BUILD)/toolchain/{as,cc,ld}` rule actually runs:

1. **Stage1**: the host's real compiler (`BOOTSTRAP_CC`, `gcc` by
   default - its only invocation anywhere in the build) compiles
   `toolchain/{as,cc,ld}`'s source directly, the same one-shot build
   Phase T7 already did.
2. **Stage2**: Stage1's own `cc`/`as`/`ld` compile that *exact same
   source* - including `toolchain/hostlib/`, see below - into a second
   set of binaries.
3. **Stage3**: Stage2 compiles the same source a third time.
4. **Verify**: Stage2 and Stage3 must be byte-identical (`cmp`) for all
   three tools. This is the standard GCC/LLVM fixed-point proof that a
   compiler genuinely compiles itself, not just "happened to survive
   being compiled once by something that tolerated its source."

On success, Stage2 (== Stage3) is installed at `$(BUILD)/toolchain/
{as,cc,ld}` - the exact path `AS`/`CC`/`LD` already pointed at since
Phase T7 - so every kernel, boot-loader, and host-tool build after this
one is genuinely self-hosted, not merely bootstrapped by an external
compiler once and then reused.

### hostlib: a minimal libc for AS/CC/LD's own source

Phase T7's builtins (`__sys_open`/`read`/`write`/`lseek`/`close`/`exit`)
were enough for `mkfs_tinyfs`, which was already deliberately written
libc-free. `toolchain/{as,cc,ld}`'s own source was never written that
way - it uses ordinary `fopen`/`fprintf`/`malloc`/`strcmp`/`isdigit`
freely, same as any normal C program compiled by a normal compiler. Two
new builtins (`__sys_brk`, `__sys_chmod`) plus `toolchain/hostlib/`
(compiled into every `--target=host` binary alongside the program's own
object files) implement just enough of `stdio.h`/`stdlib.h`/`string.h`/
`ctype.h`/`sys/stat.h` to cover what that source actually calls:
unbuffered `FILE*` I/O over the raw syscalls, a bump allocator over
`brk(2)` (no `free()` - these are short-lived, single-pass build tools,
same spirit as the rest of this codegen's "simple over optimal"
philosophy), and a `printf`-format subset (`%s %d %u %c`, the `+` flag,
`l`/`ll` length modifiers parsed and ignored - seen below, that's not a
shortcut, it's the only self-consistent choice once `int64_t` is a
32-bit alias).

### New CC features, needed only because CC now compiles *itself*

Two real language features, both invisible to every kernel/host-tool
build target until AS/CC/LD's own source went through CC for the first
time:

- **Variadic functions** (`fprintf(FILE*, const char*, ...)`, and
  codegen.c's own `out(const char *fmt, ...)`): `...` in a parameter
  list is now accepted (parser.c's `parse_param_list`) with no further
  bookkeeping needed - call sites already push each argument
  independently with no prototype-arity check (see
  `codegen_call_or_builtin`), so a variadic call "just works" once
  parsing tolerates the extra parameter marker. `va_start`/`va_arg`/
  `va_end` aren't real functions (CC's preprocessor has no function-like
  macros, and `va_arg`'s second argument is a type name, which no
  ordinary call could accept) - they're parsed directly, like `sizeof`,
  and desugar into ordinary pointer arithmetic on a `char*` (`va_list`):
  `va_start(ap, last)` becomes `ap = (char*)&last + K`; `va_arg(ap, T)`
  becomes `*(T*)((ap += K) - K)` (the compound-assign-then-subtract is
  the only way to get "read the old value, then advance" as one
  expression without a temporary), where `K` is `last`'s (or `T`'s)
  4-byte-rounded size.
- **Multi-dimensional arrays** (`uint8_t sec_bytes[MAX_SECTIONS]
  [65536]` in AS's own `as.h`): `parse_declarator`, `parse_type_name`,
  and `parse_struct_members` all only handled one `[N]` group before,
  silently either erroring or (worse, see below) nesting extra
  dimensions in the wrong order. A shared `parse_array_dims` helper now
  collects every `[N]` group and nests them correctly - the *first*
  bracket is the outermost dimension, so it has to wrap a type built
  from the trailing brackets inward, not the other way around.

### Real bugs found

This phase's verification bar was explicitly two-tier: the toolchain
test battery (`tools/run_toolchain_tests.sh`, host-only, fast) had to
pass under full self-hosting, *and* the full OS boot/shell suite
(`tools/run_tests.sh`) had to keep passing against a disk image built
entirely by the self-hosted result. Both were needed - one bug below
was invisible to the first and only caught by the second. As with every
previous phase, each was found by something concrete actually failing
(a crash, a wrong exit code, a corrupted disk image), not by inspection:

- **`elf_format.h`'s `ELF32_ST_INFO`/`ELF32_R_SYM`/`ELF32_R_TYPE`/
  `ELF32_R_INFO` were function-like macros**: CC's preprocessor only
  ever supported object-like `#define` (a Phase T2 scoping decision,
  never previously an issue since no *kernel* source used one) - the
  first time AS's or LD's own `elf_format.h` went through CC, these
  four failed to expand as calls at all. Converted to plain `static
  inline` functions (already proven to parse fine elsewhere, e.g.
  `cc.h`'s own `safe_strcpy`).
- **Three compound literals in AS's own source** (`(token_t){ .kind =
  TK_EOF }` in `parser.c`, `(Elf32_Sym){0}` twice in `elf_writer.c`/
  `link_flat.c`): compound literals were already known-unsupported
  (Phase T3's compound-literal refactor covered `kernel/` and `tools/`,
  but not `toolchain/` itself, since nothing had compiled `toolchain/`
  through CC before). Replaced with `memset` + explicit field
  assignment, the same pattern T3 established.
- **`parse_param_list`'s own array-parameter decay was wrong for 2+
  dimensions** - and its own declaration, right there in `parser.c`, is
  `char names[][MAX_NAME]`: the old code wrapped every `[N]` bracket
  group in another `ptr_to()`, so `T x[][M]` decayed to `T**` instead of
  the correct `T (*x)[M]` (only the *first*, unsized dimension decays to
  a pointer - trailing dimensions stay real array dimensions of the
  pointee). Every `names[n]` inside `parse_param_list` itself then
  indexed by 4-byte pointer strides instead of `MAX_NAME`-byte array
  strides, corrupting parser state on the very first call with a real
  parameter - self-hosting is what first parsed this function's own
  signature through CC instead of gcc.
- **`codegen_addr` had no case for a struct/union-returning `ND_CALL`**:
  `line_peek(ln).kind` (member access directly on a struct-by-value
  call's result, no intermediate variable) is a common, ordinary C
  idiom that Phase T7's struct-return test coverage never happened to
  exercise. Fixed by treating it like any other "address of a
  struct-typed rvalue" (`codegen_call_or_builtin` already leaves that
  address in `eax` on return, per its own established convention).
- **AS's `.bss` handling overflowed its own 64KB-per-section buffer**:
  `.resb`'s pass-2 loop materialized real (zero) bytes into
  `ctx->sec_bytes[SEC_BSS]` even though nothing ever reads them back
  (both writers skip emitting `.bss` content, matching `SHT_NOBITS`
  semantics) - fine for kernel-sized `.bss`, but AS's own `ctx` global
  alone reserves ~20MB of `.bss`. Fixed by passing a `NULL` destination
  for `.bss` statements in pass 2 (the same "dry-run, length-only" mode
  pass 1 already uses), so only the *count* is ever tracked.
- **LD's `MAX_SYMS_PER_FILE`/`MAX_RELOCS_PER_FILE` (512) and
  `MAX_OUT_SYMS` (2048) were too small**: `toolchain/as/encode.c`'s own
  object file alone has 800+ symbols (mostly its many small static
  encoding tables and helpers). Bumped generously (4096 / 4096 / 16384),
  the same "just raise the fixed limit" fix already used repeatedly in
  Phase T7.
- **A short/partial local brace-init left the rest of the aggregate as
  stack garbage, not zero**: `line_t ln = {0};` for a 3-member struct
  (one member a 32-element array) only initialized the *first* member;
  the fix `parse_one_local_decl` already had for globals (a trailing
  `resb` in `emit_data_bytes`) had no local-variable equivalent. This
  one was the session's most confusing bug to trace: `ln.n` (an
  uninitialized bounds counter) and the outer parse loop's "did we
  reach real EOF" check both silently read garbage, sending AS's own
  line-tokenizing loop into a multi-million-iteration spin that looked
  like a hang, not a crash. Fixed with `codegen_zero_range`, a byte-loop
  that zeroes the whole aggregate up front (redundant but harmless on
  nested recursive calls) before the existing per-element/member
  overlay runs.
- **A string-literal initializer for a *pointer-typed* struct field
  emitted a null pointer, not a relocation**: `static const regdef_t
  REGS[] = {{"al",R_AL,1}, ...}` (AS's own register table) - `emit_
  scalar_bytes` used `v->num` for every non-`TY_CHAR` scalar, but an
  `ND_STR` node never sets `->num` at all. `REGS[0].name` came out NULL,
  and `strcmp(NULL, "ebp")` crashed on the very first register lookup.
  Fixed by checking for `ND_STR` first and emitting `dd <label>` (a real
  relocation to the string's own bytes) instead.
- **Pointer-minus-pointer (`s - ctx->syms`, elf_writer.c's own
  array-position computation for a relocation's symbol index) was typed
  and compiled as if it were pointer arithmetic, not an integer element
  count**: `make_binop` labeled *any* `TK_MINUS` between two pointers as
  itself pointer-typed, so `codegen_binop`'s pointer-arithmetic scaling
  fired and **multiplied** one operand by the element size instead of
  **dividing** the raw byte difference by it - `sym_elf_index[garbage]`
  then read wildly out of bounds. Fixed with real `ptrdiff_t` support:
  `make_binop` now types `ptr - ptr` as plain `int`, and `codegen_binop`
  computes the byte difference and divides it by the element size (via
  `idiv`) as its own explicit case, ahead of the general pointer-
  arithmetic branch.
- **`chmod(path, 0755)` in `link_static_exec.c` silently used decimal
  755, not octal 0755**: CC's numeric-literal lexer only ever recognized
  a `0x` hex prefix, never a bare leading-zero octal one (never an issue
  before - nothing in the kernel used octal literals). Every self-hosted-
  built executable ended up with permission bits `1363` octal instead of
  `755` (missing owner-read, among other things) - it happened to still
  be *executable* by the OS (execute doesn't require read), so this was
  a real, silent correctness bug that never actually broke a test.
  Rewritten in decimal (493) with a comment explaining why.
- **AS's own `.dq` (8-byte data directive) computed its high 32 bits as
  `v >> 32`, which silently becomes `v >> 0` under self-hosting**: not a
  parsing bug at all - `int64_t` is CC's 32-bit alias (see stdint.h),
  and x86's `SHR`/`SAR` mask a 32-bit operand's shift count to 5 bits,
  so a *compiled* `v >> 32` is a no-op, not zero. `boot/boot.S`'s `dq 1`
  (the disk-address-packet's starting-LBA field) came out as `0x0000000
  100000001` instead of `1` - the boot sector still assembled and
  linked without error, and every toolchain test still passed, since
  none of them touch `dq` at all (only hand-written boot/stage2 assembly
  does). Only running the *full* `tools/run_tests.sh` against a disk
  image built end-to-end by the self-hosted toolchain caught this - the
  kernel never got to print anything, because the boot sector's own INT
  13h disk read (extended by that corrupted LBA) never loaded stage2 in
  the first place. Fixed by writing the high word directly as a sign
  extension (`v < 0 ? 0xFFFFFFFF : 0`) instead of a shift no self-hosted
  build can trust.
