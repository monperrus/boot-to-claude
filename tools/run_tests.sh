#!/usr/bin/env bash
# Boots the kernel headlessly in QEMU, checks the kernel self-test result,
# then drives the interactive shell via QEMU's monitor `sendkey` (see
# tools/qemu_type.py - there's no other way to feed the polled PS/2
# keyboard driver without real hardware) and asserts on serial output.
set -euo pipefail

cd "$(dirname "$0")/.."
make >/dev/null

BUILD=build
LOG="$BUILD/serial.log"
MON="$BUILD/qemu-monitor.sock"
rm -f "$LOG" "$MON"

qemu-system-i386 \
    -drive file="$BUILD/disk.img",format=raw \
    -display none \
    -serial file:"$LOG" \
    -monitor unix:"$MON",server,nowait \
    -no-reboot &
QEMU_PID=$!

cleanup() {
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
}
trap cleanup EXIT

fail() {
    echo "run_tests: FAIL - $1"
    echo "--- serial log ---"
    cat "$LOG" 2>/dev/null || true
    exit 1
}

wait_for() {
    local pattern="$1"
    local max_tries="${2:-50}" # 50 * 0.2s = 10s
    local i=0
    while ! grep -qF "$pattern" "$LOG" 2>/dev/null; do
        i=$((i + 1))
        if [ "$i" -ge "$max_tries" ]; then
            fail "timed out waiting for '$pattern'"
        fi
        sleep 0.2
    done
}

type_line() {
    python3 tools/qemu_type.py "$MON" "$1"$'\n'
}

# Checks that every non-blank line in tests/expected/<fixture> appears
# (in order) somewhere in the serial log - used for .tsh/.tc fixtures run
# through the shell's `run`/`build`+`exec` commands.
check_fixture_output() {
    local expected_file="$1"
    while IFS= read -r want; do
        [ -z "$want" ] && continue
        wait_for "$want" 30
    done < "$expected_file"
}

for i in $(seq 1 25); do
    [ -S "$MON" ] && break
    sleep 0.2
done
[ -S "$MON" ] || fail "qemu monitor socket never appeared"

wait_for "SELFTEST PASS"
wait_for "type 'help'"

type_line "ls"
wait_for "hello.txt"

type_line "cat hello.txt"
wait_for "Hello, tinyfs!"

type_line "write greet.txt hi there"
wait_for "write: ok"

type_line "cat greet.txt"
wait_for "hi there"

type_line "rm greet.txt"
wait_for "rm: ok"

type_line "cat greet.txt"
wait_for "cat: no such file"

type_line "help"
wait_for "commands:"

type_line "run hello.tsh"
check_fixture_output tests/expected/hello.tsh.txt

echo "run_tests: PASS"
