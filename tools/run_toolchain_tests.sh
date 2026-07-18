#!/usr/bin/env bash
# Compiles each tests/toolchain/*.c fixture through CC (--target=host) +
# AS + LD (all our own tools as of Phase T5/T7), runs it directly, and
# checks the exit code. Fast host-side iteration for the compiler;
# kernel-target builtins (port I/O etc) are verified separately via a
# QEMU boot, since they can't run on the host.
set -euo pipefail

cd "$(dirname "$0")/.."

CC=${CC_BIN:-build/toolchain/cc}
AS=${AS_BIN:-build/toolchain/as}
LD=${LD_BIN:-build/toolchain/ld}
BUILD=build/toolchain_tests
mkdir -p "$BUILD" build/toolchain

# Not wired into the main Makefile yet - bootstrap with the host compiler
# if the tools haven't been built already (see docs/toolchain.md on the
# one-time host-gcc bootstrap dependency).
if [ ! -x "$CC" ]; then
    gcc -Wall -Wextra -O2 -o "$CC" toolchain/cc/*.c
fi
if [ ! -x "$AS" ]; then
    gcc -Wall -Wextra -O2 -o "$AS" toolchain/as/*.c
fi
if [ ! -x "$LD" ]; then
    gcc -Wall -Wextra -O2 -o "$LD" toolchain/ld/*.c
fi

declare -A EXPECTED=(
    [t01_basic]=5
    [t02_control]=7
    [t02_types]=42
    [t03_recursion]=55
    [t04_pointers]=31
    [t05_structs]=17
    [t06_switch]=139
    [t07_comma]=53
    [t08_forward_struct]=6
    [t09_local_init]=81
    [t10_short]=6
    [t11_array_size]=155
    [t12_local_array_size]=4
    [t13_struct_copy]=212
    [t14_struct_return]=44
    [t15_escapes]=158
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
    "$LD" -o "$BUILD/$name" "$BUILD/$name.o"

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
