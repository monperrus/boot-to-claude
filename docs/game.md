# Tiny Monsters (`kernel/game/`)

A small turn-based, Pokemon-like battle game built into the kernel,
reachable from the shell's `play` command. It exists specifically to be
playable by an external agent (an AI agent, or any other program) that
only has a serial connection to the machine - no keyboard, no display.

## Dual input: keyboard *or* serial

Every other input path in this kernel (the shell) used to read
exclusively from the polled PS/2 keyboard (`kernel/keyboard.c`). Serial
(`kernel/serial.c`, COM1) was write-only - used for the existing
dual-output logging (`console_write` + `serial_write` side by side
everywhere), never for input.

`kernel/input.c` adds that: `serial_has_char()`/`serial_getc()` (serial
RX) and `keyboard_has_char()` (a non-blocking peek at the PS/2
controller) let `input_getchar()` poll both peripherals and return
whichever produces a character first. Both the shell (`kernel/shell/
shell.c`) and the game (`kernel/game/game.c`) read all their input
through `input_getchar()`, so a human at the keyboard and an agent
connected over serial can drive either one interchangeably, in the same
session, with no configuration - matching the existing dual-*output*
convention rather than adding a new, separate input mode.

## Graphical battle screen

When there's a real display attached (`make run-gui`, or any QEMU
invocation with a display backend), the game also renders a GBA-Pokemon-
style battle screen: sky/ground background, both combatants' sprites and
platforms, HP bars, and a dialogue/menu box - drawn via direct VGA mode
0x13 register programming (`kernel/vga.c`, `docs/toolchain.md`-adjacent
bare-metal register I/O, no BIOS/VBE calls). Purely additive: every
`console_write`/`serial_write` call and every `input_getchar()` read
this file already made are unchanged, so an agent playing over serial
sees exactly the same text stream whether or not anyone is watching the
graphical side. `kernel/game/sprites.c` draws the three starters
procedurally (circles/ellipses/triangles from `vga.c`, the same
hand-authored-primitives approach as `kernel/font.c`'s stroke-based
glyphs) rather than from a stored image asset.

Returning to the shell restores 80x25 text mode, including the real
BIOS-loaded font glyphs (mode 0x13 drawing and text-mode character data
share the same underlying VGA RAM - see `vga.c`'s comments on plane 2
save/restore).

Since there's no timer interrupt in this kernel, each battle message
holds on screen for a fixed busy-wait spin (`battle_gui.c`'s
`gui_pause`) rather than a calibrated real-time delay - long enough to
read, uncalibrated by design (nothing here depends on its exact
duration; an AI agent isn't reading pixels).

## Playing over serial

Boot with a bidirectional serial backend instead of (or in addition to)
a display - e.g.:

```
qemu-system-i386 -drive file=build/disk.img,format=raw \
    -display none -serial unix:/tmp/tinyos-serial.sock,server,nowait
```

Then connect to that socket and treat it as a plain line-oriented text
stream: read until `SELFTEST PASS` and the shell's `> ` prompt, send
`play\n`, read the starter menu, send a digit, read the battle screen,
and so on - exactly what `tools/serial_game_test.py` does (see
Testing below). No special protocol; it's the same text a human would
see and type at a terminal.

## Rules

- **Starters**: three species, one of each type - `Emberpup` (Fire),
  `Aqualing` (Water), `Leafling` (Grass). The wild encounter pool draws
  from the same three.
- **Types**: a simple rock-paper-scissors cycle, Fire beats Grass beats
  Water beats Fire. A type-advantaged move does 1.5x damage ("It's super
  effective!"); a disadvantaged one does 0.5x ("It's not very
  effective..."); same/no relationship is 1x.
- **Battle**: the player always attacks first each turn; if the wild
  monster survives, it counterattacks with a random one of its own three
  moves. Damage is `move power`, type-adjusted, `+/-2` random variance,
  minimum 1.
- **Progression**: HP does *not* regenerate between encounters - only
  fainting ends a run. Each wild monster defeated increases `stage`
  (their HP scales up slightly with it); reach stage 6 (5 wins) to become
  Champion. Your own monster fainting ends the run immediately.
- **Commands** at any prompt: a digit picks a starter or a move; `run`
  flees the current encounter (a fresh one follows, same stage); `quit`
  leaves the game and returns to the shell immediately.

## Testing

Two complementary automated tests, both driving a full session against
a headless QEMU instance and asserting on the resulting output - see
each script's own header comment for the exact mechanism:

- `tools/run_tests.sh` - the shell's existing keyboard-driven suite,
  extended with a `play` pass that checks the deterministic parts of the
  flow (menu structure, `run`/`quit`) via QEMU monitor `sendkey`
  injection (`tools/qemu_type.py`), the same way every other shell
  command is tested.
- `tools/run_serial_tests.sh` (+ `tools/serial_game_test.py`) - the
  scenario this feature actually exists for: no keyboard input at all,
  driven entirely over a `-serial unix:...` socket. Plays a real battle
  to a real end state (a faint or a win - battle outcomes are
  RNG-dependent, so this accepts either) to prove the whole shell+game
  loop works end to end over serial alone, then confirms the shell is
  still responsive afterward.
