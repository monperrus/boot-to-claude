#pragma once

/* Species index must match game.c's SPECIES[] array order: 0 Emberpup
 * (fire), 1 Aqualing (water), 2 Leafling (grass). Kept as a plain int
 * rather than sharing game.c's private mon_type_t/species_t so this
 * module has no dependency on the game's internal data layout. */

void sprite_init_palette(void);

/* Draws species_idx's ~40x36 pixel creature with (x, y) as its
 * top-left corner. */
void sprite_draw(int species_idx, int x, int y);
