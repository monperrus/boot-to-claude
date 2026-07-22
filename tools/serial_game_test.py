#!/usr/bin/env python3
"""Drives kernel/game/game.c entirely over the serial port - no PS/2
keyboard, no QEMU monitor `sendkey` injection (contrast tools/qemu_type.py,
used by tools/run_tests.sh for the keyboard path). This is the actual
scenario kernel/input.c's dual keyboard+serial polling exists for: an
external agent (an AI agent, or this script standing in for one) that
only has a serial connection to the guest, sending commands and reading
responses as plain text.

Connects to the QEMU `-serial unix:PATH,server,nowait` socket tools/
run_serial_tests.sh boots the guest with. Battle outcomes are
RNG-dependent (see game.c's rng_next), so this plays real turns to a
real end state (a faint or a win) rather than asserting on a specific
outcome - it accepts either.
"""
import socket
import sys
import time

TIMEOUT_S = 20.0
POLL_S = 0.1


def connect(sock_path):
    for _ in range(50):  # 50 * 0.2s = 10s
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(sock_path)
            s.settimeout(POLL_S)
            return s
        except (FileNotFoundError, ConnectionRefusedError):
            time.sleep(0.2)
    raise SystemExit(f"serial_game_test: socket {sock_path} never appeared")


class Session:
    def __init__(self, sock):
        self.sock = sock
        self.buf = ""

    def send(self, line):
        self.sock.sendall((line + "\n").encode())

    def wait_for(self, pattern, timeout=TIMEOUT_S):
        deadline = time.time() + timeout
        while pattern not in self.buf:
            if time.time() > deadline:
                raise SystemExit(
                    f"serial_game_test: timed out waiting for {pattern!r}\n"
                    f"--- received so far ---\n{self.buf}"
                )
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                raise SystemExit("serial_game_test: connection closed unexpectedly")
            self.buf += chunk.decode(errors="replace")
        # Trim everything up to and including the match, so the next
        # wait_for() only sees genuinely new output.
        self.buf = self.buf[self.buf.index(pattern) + len(pattern):]

    def wait_for_any(self, patterns, timeout=TIMEOUT_S):
        deadline = time.time() + timeout
        while True:
            for p in patterns:
                if p in self.buf:
                    self.buf = self.buf[self.buf.index(p) + len(p):]
                    return p
            if time.time() > deadline:
                raise SystemExit(
                    f"serial_game_test: timed out waiting for any of {patterns!r}\n"
                    f"--- received so far ---\n{self.buf}"
                )
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                raise SystemExit("serial_game_test: connection closed unexpectedly")
            self.buf += chunk.decode(errors="replace")


def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <serial-unix-socket>", file=sys.stderr)
        sys.exit(1)

    sess = Session(connect(sys.argv[1]))

    sess.wait_for("SELFTEST PASS")
    sess.wait_for("type 'help'")

    sess.send("help")
    sess.wait_for("commands:")
    print("ok: shell reachable over serial (no keyboard involved)")

    sess.send("play")
    sess.wait_for("Choose your starter:")
    sess.send("1")
    sess.wait_for("4) Run")
    print("ok: game reachable, starter menu -> battle screen")

    # Play real turns (move 1 each time) to a real end state - either
    # outcome is fine, this is just proving serial-only input drives a
    # full game to completion, not asserting a specific RNG result.
    outcome = "4) Run"  # sentinel: "no end state reached yet"
    for _ in range(40):
        sess.send("1")
        outcome = sess.wait_for_any(["Game over!", "you are the Champion!", "4) Run"])
        if outcome != "4) Run":
            break
    if outcome == "4) Run":
        raise SystemExit("serial_game_test: battle never reached an end state")
    print(f"ok: battle played to completion over serial ({outcome.strip()})")

    sess.send("help")
    sess.wait_for("commands:")
    print("ok: shell still responsive after the game returned")

    print("serial_game_test: PASS")


if __name__ == "__main__":
    main()