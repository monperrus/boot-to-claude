#pragma once

/* Blocks until a character arrives from either the PS/2 keyboard or the
 * serial port (COM1), whichever comes first - lets a human at the
 * keyboard and e.g. an AI agent connected over serial both drive the
 * shell/game interchangeably, matching the kernel's existing
 * dual-output (console+serial) convention for output. */
char input_getchar(void);

/* Free-running counter, incremented once per poll iteration spent
 * waiting for input in input_getchar(). Not wall-clock time (there's no
 * timer interrupt in this kernel - see README), but it advances at a
 * rate that depends on real elapsed wall-clock waiting, so it's a
 * reasonable low-stakes entropy source for e.g. game.c's RNG seed. */
unsigned int input_ticks(void);