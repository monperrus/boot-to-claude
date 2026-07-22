#pragma once

/* A small turn-based, Pokemon-like battle game: pick a starter, fight a
 * run of wild encounters with a rock-paper-scissors type system, until
 * either your monster faints (game over) or you defeat enough wild
 * monsters to become champion. Runs its own input/output loop (dual
 * keyboard+serial, via input.h - see its comment) and returns to the
 * caller (the shell) when the run ends. */
void game_run(void);