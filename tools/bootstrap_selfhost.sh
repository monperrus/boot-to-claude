#!/usr/bin/env bash
# Phase T8: the self-hosting bootstrap. Builds AS/CC/LD from source three
# times - Stage1 via the host's real compiler (BOOTSTRAP_CC, only ever
# used here), Stage2 by compiling the exact same source with Stage1, and
# Stage3 by compiling it again with Stage2 - and requires Stage2 and
# Stage3 to be byte-identical. That fixed point is the standard GCC/LLVM
# proof that a compiler genuinely compiles itself, rather than having
# merely been *compiled once* by something else that happened to tolerate
# its source. See docs/toolchain.md.
#
# On success, installs Stage2 (== Stage3) at $BUILD/toolchain/{as,cc,ld} -
# the same path the main Makefile's AS/CC/LD variables already point at,
# so every OS build after this one is self-hosted, not just bootstrapped.
set -euo pipefail

cd "$(dirname "$0")/.."

BUILD=${1:-build}
BOOTSTRAP_CC=${BOOTSTRAP_CC:-gcc}

AS_SRCS=(toolchain/as/*.c)
CC_SRCS=(toolchain/cc/*.c)
LD_SRCS=(toolchain/ld/*.c)
HOSTLIB_SRCS=(toolchain/hostlib/*.c)

echo "bootstrap: stage1 (host $BOOTSTRAP_CC)"
mkdir -p "$BUILD/toolchain-stage1"
"$BOOTSTRAP_CC" -Wall -Wextra -O2 -o "$BUILD/toolchain-stage1/as" "${AS_SRCS[@]}"
"$BOOTSTRAP_CC" -Wall -Wextra -O2 -o "$BUILD/toolchain-stage1/cc" "${CC_SRCS[@]}"
"$BOOTSTRAP_CC" -Wall -Wextra -O2 -o "$BUILD/toolchain-stage1/ld" "${LD_SRCS[@]}"

# build_stage NAME: compiles toolchain/{as,cc,ld}+hostlib's source with
# $BUILD/toolchain-$PREV_STAGE's tools (set by the caller), producing
# $BUILD/toolchain-$NAME/{as,cc,ld}.
build_stage() {
    local name=$1
    local prev_cc=$2
    local prev_as=$3
    local prev_ld=$4
    local outdir="$BUILD/toolchain-$name"
    local objdir="$BUILD/toolchain-$name-obj"
    mkdir -p "$outdir" "$objdir"

    local as_objs=()
    local cc_objs=()
    local ld_objs=()
    local hostlib_objs=()

    local src tag base s o
    for src in "${AS_SRCS[@]}" "${CC_SRCS[@]}" "${LD_SRCS[@]}" "${HOSTLIB_SRCS[@]}"; do
        case "$src" in
            toolchain/as/*) tag=as ;;
            toolchain/cc/*) tag=cc ;;
            toolchain/ld/*) tag=ld ;;
            toolchain/hostlib/*) tag=hostlib ;;
        esac
        base=$(basename "$src" .c)
        s="$objdir/${tag}_${base}.S"
        o="$objdir/${tag}_${base}.o"
        "$prev_cc" -Itoolchain/include --target=host -o "$s" "$src"
        "$prev_as" -f elf32 -o "$o" "$s"
        case "$tag" in
            as) as_objs+=("$o") ;;
            cc) cc_objs+=("$o") ;;
            ld) ld_objs+=("$o") ;;
            hostlib) hostlib_objs+=("$o") ;;
        esac
    done

    "$prev_ld" -o "$outdir/as" "${as_objs[@]}" "${hostlib_objs[@]}"
    "$prev_ld" -o "$outdir/cc" "${cc_objs[@]}" "${hostlib_objs[@]}"
    "$prev_ld" -o "$outdir/ld" "${ld_objs[@]}" "${hostlib_objs[@]}"
    chmod +x "$outdir/as" "$outdir/cc" "$outdir/ld"
}

echo "bootstrap: stage2 (via stage1)"
build_stage stage2 \
    "$BUILD/toolchain-stage1/cc" "$BUILD/toolchain-stage1/as" "$BUILD/toolchain-stage1/ld"

echo "bootstrap: stage3 (via stage2)"
build_stage stage3 \
    "$BUILD/toolchain-stage2/cc" "$BUILD/toolchain-stage2/as" "$BUILD/toolchain-stage2/ld"

echo "bootstrap: verifying stage2 == stage3"
fail=0
for t in as cc ld; do
    if cmp -s "$BUILD/toolchain-stage2/$t" "$BUILD/toolchain-stage3/$t"; then
        echo "  $t: identical"
    else
        echo "  $t: DIFFERS" >&2
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "bootstrap: FAIL - stage2 and stage3 are not byte-identical" >&2
    exit 1
fi

mkdir -p "$BUILD/toolchain"
cp "$BUILD/toolchain-stage2/as" "$BUILD/toolchain-stage2/cc" "$BUILD/toolchain-stage2/ld" "$BUILD/toolchain/"
chmod +x "$BUILD/toolchain/as" "$BUILD/toolchain/cc" "$BUILD/toolchain/ld"

echo "bootstrap: PASS - self-hosted toolchain installed at $BUILD/toolchain"
