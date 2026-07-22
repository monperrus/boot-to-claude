#!/usr/bin/env bash
# Boots the kernel headlessly and drives kernel/game/game.c entirely over
# the serial port via tools/serial_game_test.py - no PS/2 keyboard, no
# QEMU monitor `sendkey` (contrast tools/run_tests.sh, which uses the
# keyboard path). This is the actual scenario kernel/input.c's dual
# keyboard+serial polling exists for: an external agent connected only
# over serial. See docs/game.md.
set -euo pipefail

cd "$(dirname "$0")/.."
make >/dev/null

BUILD=build
SOCK="$BUILD/qemu-serial.sock"
rm -f "$SOCK"

qemu-system-i386 \
    -drive file="$BUILD/disk.img",format=raw \
    -display none \
    -serial unix:"$SOCK",server,nowait \
    -no-reboot &
QEMU_PID=$!

cleanup() {
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
}
trap cleanup EXIT

python3 tools/serial_game_test.py "$SOCK"
