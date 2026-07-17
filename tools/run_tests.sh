#!/usr/bin/env bash
# Boots the kernel headlessly in QEMU and checks the serial output for the
# self-test oracle. Later phases extend this with fixture-based checks
# (tests/*.tsh, tests/*.tc against tests/expected/*.txt).
set -euo pipefail

cd "$(dirname "$0")/.."

make >/dev/null

OUTPUT=$(timeout 5 qemu-system-i386 \
    -drive file=build/disk.img,format=raw \
    -display none -serial stdio -no-reboot 2>&1 || true)

echo "$OUTPUT"

if echo "$OUTPUT" | grep -q "SELFTEST PASS"; then
    echo "run_tests: PASS"
    exit 0
else
    echo "run_tests: FAIL"
    exit 1
fi
