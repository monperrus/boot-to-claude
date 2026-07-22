#pragma once

/* Graphical (VGA mode 13h) rendering of the battle game, styled after
 * the GBA Pokemon games' battle screens - drawn *alongside* game.c's
 * existing console/serial text narration, never replacing it: an AI
 * agent playing over serial sees exactly the same lines as before this
 * existed. species_idx follows sprites.h's convention (0 Emberpup,
 * 1 Aqualing, 2 Leafling), matching game.c's SPECIES[] array order. */

void battle_gui_enter(void);
void battle_gui_exit(void);

/* One static screen shown while the text starter-selection prompt is
 * on screen; names[i] labels species i. */
void battle_gui_starter_select(const char *names[3]);

void battle_gui_scene(int player_species, int player_hp, int player_max_hp, const char *player_name,
                       int wild_species, int wild_hp, int wild_max_hp, const char *wild_name);

/* Redraws the message box with up to two lines; line2 may be NULL or
 * empty for a single-line message. */
void battle_gui_message(const char *line1, const char *line2);

/* Redraws the message box as a 2x2 move/run menu (move_names[3] plus a
 * fixed "Run" fourth slot), mirroring the numbered text menu. */
void battle_gui_menu(const char *move_names[3], const int move_powers[3]);
