#include "game.h"

#include <stdint.h>

#include "console.h"
#include "input.h"
#include "serial.h"
#include "string.h"

#define LINE_MAX 64
#define WIN_STAGE 5 /* defeat this many wild monsters to become champion */

/* --- dual console+serial output, same convention as shell.c --- */

static void w(const char *s) {
    console_write(s);
    serial_write(s);
}

static void wln(const char *s) {
    w(s);
    w("\n");
}

static void w_int(int v) {
    char buf[16];
    w(itoa(v, buf, 10));
}

/* --- line input, same convention as shell.c's read_line (dual keyboard
 * +serial via input_getchar - see its comment) --- */

static void echo_char(char c) {
    console_putc(c);
    serial_putc(c);
}

static int read_line(char *buf, int max_len) {
    int len = 0;
    for (;;) {
        char c = input_getchar();
        if (c == '\n') {
            echo_char('\n');
            buf[len] = '\0';
            return len;
        } else if (c == '\b') {
            if (len > 0) {
                len--;
                echo_char('\b');
                echo_char(' ');
                echo_char('\b');
            }
        } else if (c >= 32 && c < 127 && len < max_len - 1) {
            buf[len++] = c;
            echo_char(c);
        }
    }
}

/* --- a small xorshift32 PRNG, perturbed each draw with input_ticks()
 * (see input.h) - there's no timer interrupt in this kernel, so real
 * elapsed-wait-time jitter is the only entropy source available. */

static uint32_t rng_state = 2463534242u; /* must be nonzero */

static uint32_t rng_next(void) {
    rng_state ^= input_ticks() + 1;
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static int rng_range(int lo, int hi) { /* inclusive */
    /* CC's codegen only ever emits signed idiv, never true unsigned
     * division (see docs/toolchain.md) - the rest of the kernel avoids
     * this by construction (nothing else divides a value that could
     * have the sign bit set), but rng_next()'s raw uint32_t output can.
     * Left unmasked, a result with the sign bit set gets `%`'d as if
     * negative, producing a negative "random" index and an out-of-bounds
     * moves[] read (garbage move name, huge damage). Clearing the sign
     * bit first keeps the value in idiv's safe signed range. */
    uint32_t r = rng_next() & 0x7FFFFFFFu;
    return lo + (int)(r % (uint32_t)(hi - lo + 1));
}

/* --- game data --- */

typedef enum { TYPE_FIRE, TYPE_WATER, TYPE_GRASS } mon_type_t;

typedef struct {
    const char *name;
    int power;
} move_t;

typedef struct {
    const char *name;
    mon_type_t type;
    int max_hp;
    move_t moves[3];
} species_t;

#define NUM_SPECIES 3

static const species_t SPECIES[NUM_SPECIES] = {
    { "Emberpup", TYPE_FIRE, 28, { { "Ember", 7 }, { "Scratch", 5 }, { "Flame Burst", 10 } } },
    { "Aqualing", TYPE_WATER, 30, { { "Water Jet", 7 }, { "Tail Slap", 5 }, { "Tide Wave", 10 } } },
    { "Leafling", TYPE_GRASS, 32, { { "Vine Whip", 7 }, { "Tackle", 5 }, { "Leaf Storm", 10 } } },
};

/* Rock-paper-scissors: fire > grass > water > fire. */
static int type_beats(mon_type_t a, mon_type_t d) {
    return (a == TYPE_FIRE && d == TYPE_GRASS) ||
           (a == TYPE_GRASS && d == TYPE_WATER) ||
           (a == TYPE_WATER && d == TYPE_FIRE);
}

static int compute_damage(mon_type_t attacker_type, mon_type_t defender_type, int power) {
    int dmg = power;
    if (type_beats(attacker_type, defender_type)) {
        w("It's super effective!\n");
        dmg = dmg * 3 / 2;
    } else if (type_beats(defender_type, attacker_type)) {
        w("It's not very effective...\n");
        dmg = dmg / 2;
    }
    dmg += rng_range(-2, 2);
    if (dmg < 1) {
        dmg = 1;
    }
    return dmg;
}

/* --- status line helpers --- */

static void print_hp(const char *label, const char *name, int hp, int max_hp) {
    w(label);
    w(name);
    w(": HP ");
    w_int(hp);
    w("/");
    w_int(max_hp);
    w("\n");
}

/* Parses a line already trimmed of leading spaces: a leading digit is a
 * numbered menu choice (1-9), otherwise "run"/"quit" are recognized as
 * words - covers both a human pressing a number key and an AI agent
 * typing a full word, whichever is more natural for it. Returns the
 * choice as 1-9, 0 for "run", -1 for "quit", -2 if unrecognized. */
static int parse_choice(const char *line) {
    while (*line == ' ') {
        line++;
    }
    if (*line >= '1' && *line <= '9') {
        return *line - '0';
    }
    if (strcmp(line, "run") == 0) {
        return 0;
    }
    if (strcmp(line, "quit") == 0) {
        return -1;
    }
    return -2;
}

/* Returns the chosen starter index (0..NUM_SPECIES-1), or -1 if the
 * player typed "quit". */
static int choose_starter(void) {
    char line[LINE_MAX];
    wln("Choose your starter:");
    for (int i = 0; i < NUM_SPECIES; i++) {
        w_int(i + 1);
        w(") ");
        w(SPECIES[i].name);
        w(" (");
        w(SPECIES[i].type == TYPE_FIRE ? "Fire" : SPECIES[i].type == TYPE_WATER ? "Water" : "Grass");
        wln(")");
    }
    for (;;) {
        w("> ");
        read_line(line, sizeof(line));
        int choice = parse_choice(line);
        if (choice == -1) {
            return -1;
        }
        if (choice >= 1 && choice <= NUM_SPECIES) {
            return choice - 1;
        }
        wln("invalid choice - type a number, 'run', or 'quit'");
    }
}

void game_run(void) {
    char line[LINE_MAX];

    wln("=== Tiny Monsters ===");
    wln("Defeat 5 wild monsters to become champion. Type 'quit' any time to leave.");

    int player_idx = choose_starter();
    if (player_idx < 0) {
        wln("Left the game.");
        return;
    }
    const species_t *player = &SPECIES[player_idx];
    int player_hp = player->max_hp;
    int stage = 1;

    for (;;) {
        int wild_idx = rng_range(0, NUM_SPECIES - 1);
        const species_t *wild = &SPECIES[wild_idx];
        int wild_max_hp = wild->max_hp + (stage - 1) * 4;
        int wild_hp = wild_max_hp;

        w("=== A wild ");
        w(wild->name);
        w(" appears! (stage ");
        w_int(stage);
        wln(")");

        for (;;) {
            print_hp("Your ", player->name, player_hp, player->max_hp);
            print_hp("Wild ", wild->name, wild_hp, wild_max_hp);
            for (int i = 0; i < 3; i++) {
                w_int(i + 1);
                w(") ");
                w(player->moves[i].name);
                w(" (");
                w_int(player->moves[i].power);
                wln(")");
            }
            wln("4) Run");
            w("> ");
            read_line(line, sizeof(line));
            int choice = parse_choice(line);

            if (choice == -1) {
                wln("Left the game.");
                return;
            }
            if (choice == 0 || choice == 4) {
                wln("You ran away!");
                break; /* next encounter, same stage */
            }
            if (choice < 1 || choice > 3) {
                wln("invalid choice - type 1-3 for a move, 'run', or 'quit'");
                continue;
            }

            const move_t *mv = &player->moves[choice - 1];
            w("Your ");
            w(player->name);
            w(" uses ");
            w(mv->name);
            wln("!");
            int dmg = compute_damage(player->type, wild->type, mv->power);
            wild_hp -= dmg;
            if (wild_hp < 0) {
                wild_hp = 0;
            }
            w("Wild ");
            w(wild->name);
            w(" takes ");
            w_int(dmg);
            w(" damage. (HP ");
            w_int(wild_hp);
            w("/");
            w_int(wild_max_hp);
            wln(")");

            if (wild_hp <= 0) {
                w("Wild ");
                w(wild->name);
                wln(" fainted! You win!");
                stage++;
                if (stage > WIN_STAGE) {
                    w("*** You defeated ");
                    w_int(WIN_STAGE);
                    wln(" wild monsters - you are the Champion! ***");
                    return;
                }
                break; /* next encounter */
            }

            const move_t *wmv = &wild->moves[rng_range(0, 2)];
            w("Wild ");
            w(wild->name);
            w(" uses ");
            w(wmv->name);
            wln("!");
            int wdmg = compute_damage(wild->type, player->type, wmv->power);
            player_hp -= wdmg;
            if (player_hp < 0) {
                player_hp = 0;
            }
            w("Your ");
            w(player->name);
            w(" takes ");
            w_int(wdmg);
            w(" damage. (HP ");
            w_int(player_hp);
            w("/");
            w_int(player->max_hp);
            wln(")");

            if (player_hp <= 0) {
                w("Your ");
                w(player->name);
                w(" fainted... Game over! You reached stage ");
                w_int(stage);
                wln(".");
                return;
            }
        }
    }
}