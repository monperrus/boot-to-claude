#!/usr/bin/env python3
"""Types text into a running QEMU guest via its HMP monitor's `sendkey`
command, over a unix socket (-monitor unix:PATH,server,nowait). Used by
tools/run_tests.sh to drive the kernel shell headlessly - there is no
other way to feed the polled PS/2 keyboard driver without real hardware
or a display.
"""
import socket
import sys
import time

KEYMAP = {
    " ": "spc",
    ".": "dot",
    "\n": "ret",
    "-": "minus",
    "_": "shift-minus",
    ",": "comma",
    "/": "slash",
}


def key_name(c):
    if c in KEYMAP:
        return KEYMAP[c]
    if c.isalpha():
        return c.lower()
    if c.isdigit():
        return c
    raise ValueError(f"qemu_type: no key mapping for {c!r}")


def main():
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <monitor-socket> <text>", file=sys.stderr)
        sys.exit(1)

    sock_path, text = sys.argv[1], sys.argv[2]

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    time.sleep(0.2)
    s.recv(4096)  # monitor banner

    for c in text:
        s.sendall(f"sendkey {key_name(c)}\n".encode())
        time.sleep(0.05)
        s.recv(4096)

    s.close()


if __name__ == "__main__":
    main()
