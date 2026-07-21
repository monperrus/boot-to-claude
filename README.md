# boot-to-claude

A from-scratch, single-tasking x86 OS for QEMU/KVM: a custom two-stage
boot loader, a monolithic kernel with its own tiny filesystem (tinyfs), an
interactive shell, a dynamic scripting language, and a small C-like
compiler with its own bytecode VM.

Every line of OS/filesystem/language source in this repo is hand-written
for this project - including the toolchain that builds it. `toolchain/`
is a self-hosted assembler, C compiler, and linker (see
[`docs/toolchain.md`](docs/toolchain.md)) that replaces `nasm`/`gcc`/
`ld`/`objcopy` entirely: the host's real `gcc` is used exactly once, to
bootstrap the toolchain from source on a fresh checkout, and never
touches OS code. `qemu-system-i386` stays external - it's the machine we
run on, not part of what builds for it.

## Quickstart

```
make        # builds build/disk.img
make run    # boots it in QEMU (headless, serial on stdio)
```

`make run` boots into the shell:

```
tiny-os shell. type 'help' for commands.
> ls
hello.txt  15
hello.tsh  281
hello.tc   272
> cat hello.txt
Hello, tinyfs!
> run hello.tsh
hello from tinyfs script
x = 14
...
> build hello.tc hello.tbc
build: ok
> exec hello.tbc
14
7
55
10
```

That one session exercises every layer: tinyfs (`ls`/`cat`), the
scripting language (`run`, including its own `fread`/`fwrite` calls into
tinyfs), and the compiler + bytecode VM (`build` + `exec`, including a
recursive function call). `tools/run_tests.sh` automates exactly this
against a headless QEMU instance - see [Testing](#testing).

Requires `dd` and `qemu-system-i386` on the host, plus (one-time only,
to bootstrap `toolchain/` from source - see below) a host C compiler,
`gcc` by default. That bootstrap step never runs again after the first
`build/toolchain/{as,cc,ld}` exist: `nasm`/`ld`/`objcopy` aren't needed
at all, and `gcc` never touches OS code, only `toolchain/`'s own source.
No cross-compiler needed.

## Architecture

```
BIOS -> boot/boot.S (MBR, 512B)         real mode
     -> boot/stage2.S                    real mode -> protected mode
     -> kernel/entry.S -> kmain.c        32-bit protected mode, flat, no paging
          gdt.c / idt.c                  kernel-owned GDT; IDT covers CPU
                                          exceptions only - no PIC/IRQs, ever
          console.c / serial.c           VGA text + COM1, dual-output throughout
          memory.c                       free-list kmalloc/kfree over a 1MB heap
          ata.c -> fs/tinyfs.c           polled ATA PIO -> tinyfs filesystem
          keyboard.c -> shell/shell.c    polled PS/2 -> command REPL
          lang/lexer.c (shared)
            -> lang/script/{parser,ast,eval}.c   scripting language (tree-walking)
            -> lang/compiler/{parser,codegen}.c  compiler -> lang/vm.c (bytecode VM)
```

Deliberately single-tasking: no scheduler, no preemption, no interrupts
enabled after boot, no process/syscall boundary. The shell and both
language runtimes execute directly in kernel context. This keeps the OS
layer small so the interesting work is in the filesystem and language
implementations - see [`docs/`](docs/) for their designs:

- [`docs/tinyfs.md`](docs/tinyfs.md) - on-disk filesystem format
- [`docs/scripting-language.md`](docs/scripting-language.md) - the `tsh` language
- [`docs/bytecode-isa.md`](docs/bytecode-isa.md) - the compiled language + VM ISA
- [`docs/toolchain.md`](docs/toolchain.md) - the self-hosted assembler/compiler/linker

## Repo layout

```
boot/            stage1 (MBR) + stage2 boot loaders
kernel/          the kernel itself (see architecture diagram above)
kernel/fs/       tinyfs (shared: freestanding in the kernel, native in mkfs_tinyfs)
kernel/lang/     shared lexer + the two language implementations
kernel/shell/    interactive command REPL
toolchain/       self-hosted AS/CC/LD (see docs/toolchain.md) - host-side
                   build infrastructure, not OS code, but self-authored
                   like everything else in this repo
tools/           host-side build/test infrastructure (not OS code):
                   mkfs_tinyfs.c        formats + seeds build/disk.img at build time
                   bootstrap_selfhost.sh builds+verifies toolchain/ (Stage1/2/3)
                   run_tests.sh         headless QEMU test harness
                   run_toolchain_tests.sh  host-side AS/CC/LD test harness
                   qemu_type.py         scripts keystrokes into QEMU via its HMP monitor
tests/           fixtures (*.tsh, *.tc) + tests/expected/ serial-output fixtures
tests/toolchain/ AS/CC/LD test fixtures (see run_toolchain_tests.sh)
docs/            design docs, listed above
```

## Testing

There's no hardware and no display in CI, so the serial port is the test
oracle throughout: `kernel/selftest.c` runs a boot-time self-test (string
ops, the allocator including coalescing, and a tinyfs create/write/read/
delete roundtrip) and prints `SELFTEST PASS`/`FAIL`.

```
tools/run_tests.sh
```

boots `build/disk.img` headless (`-display none -serial file:...`), waits
for `SELFTEST PASS`, then drives the shell by scripting keystrokes into
QEMU's HMP monitor over a unix socket (`tools/qemu_type.py sendkey ...`)
- the only way to feed the kernel's polled PS/2 driver without real
hardware - and asserts on the resulting serial output for every shell
command, `run hello.tsh`, and `build`+`exec` on `hello.tc`.

## Boot budget

The kernel image is capped at a fixed number of disk sectors (`boot/
stage2.S`'s `KERNEL_SECTORS`, currently 512 = 256KB), read via BIOS
INT13h in bounded chunks (SeaBIOS silently misbehaves on very large
single extended-read requests). `kernel/fs/tinyfs.h`'s
`TINYFS_FS_START_LBA` reserves exactly that much room before the
filesystem region begins. The Makefile fails the build loudly if the
kernel image ever grows past its budget - if you hit that, bump
`KERNEL_SECTORS`, the Makefile's `KERNEL_MAX_SECTORS`, and
`TINYFS_FS_START_LBA` together.
