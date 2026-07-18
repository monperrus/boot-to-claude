#!/usr/bin/env bash
# Compiles each tests/toolchain/*.c fixture through CC (--target=host) +
# AS, links with GNU ld (interim - LD doesn't exist until Phase T5), runs
# it directly, and checks the exit code. Fast host-side iteration for the
# compiler; kernel-target builtins (port I/O etc) are verified separately
# via a QEMU boot, since they can't run on the host.
set -euo pipefail

cd "$(dirname "$0")/.."

CC=${CC_BIN:-build/toolchain/cc}
AS=${AS_BIN:-build/toolchain/as}
BUILD=build/toolchain_tests
mkdir -p "$BUILD" build/toolchain

# Not wired into the main Makefile yet (that's Phase T7) - bootstrap with
# the host compiler if the tools haven't been built already.
if [ ! -x "$CC" ]; then
    gcc -Wall -Wextra -O2 -o "$CC" \
        toolchain/cc/lexer.c toolchain/cc/preprocessor.c toolchain/cc/types.c \
        toolchain/cc/parser.c toolchain/cc/codegen.c toolchain/cc/builtins.c toolchain/cc/main.c
fi
if [ ! -x "$AS" ]; then
    gcc -Wall -Wextra -O2 -o "$AS" \
        toolchain/as/lexer.c toolchain/as/parser.c toolchain/as/encode.c \
        toolchain/as/bin_writer.c toolchain/as/elf_writer.c toolchain/as/main.c
fi

declare -A EXPECTED=(
    [t01_basic]=5
    [t02_control]=7
    [t02_types]=42
    [t03_recursion]=55
    [t04_pointers]=31
    [t05_structs]=17
)

fail=0
for src in tests/toolchain/*.c; do
    name=$(basename "$src" .c)
    exp=${EXPECTED[$name]:-}
    if [ -z "$exp" ]; then
        echo "SKIP $name (no expected value registered)"
        continue
    fi

    "$CC" -I toolchain/include --target=host -o "$BUILD/$name.S" "$src"
    "$AS" -f elf32 -o "$BUILD/$name.o" "$BUILD/$name.S"
    ld -m elf_i386 -o "$BUILD/$name" "$BUILD/$name.o"

    set +e
    "$BUILD/$name"
    got=$?
    set -e

    if [ "$got" -eq "$exp" ]; then
        echo "PASS $name ($got)"
    else
        echo "FAIL $name: expected $exp, got $got"
        fail=1
    fi
done

if [ "$fail" -eq 0 ]; then
    echo "run_toolchain_tests: PASS"
else
    echo "run_toolchain_tests: FAIL"
    exit 1
fi
