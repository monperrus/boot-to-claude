#include "battle_gui.h"

#include "console.h"
#include "sprites.h"
#include "string.h"
#include "vga.h"

/* Scene palette - indices 0-10, disjoint from sprites.c's 16-27. */
#define COL_BLACK    0
#define COL_WHITE    1
#define COL_SKY      2
#define COL_GROUND   3
#define COL_HORIZON  4
#define COL_BOX_BG   5
#define COL_HP_GREEN 6
#define COL_HP_YELLOW 7
#define COL_HP_RED   8
#define COL_HP_BG    9
#define COL_PLATFORM 10

#define HPBOX_W 132
#define HPBOX_H 34

#define MSGBOX_X 8
#define MSGBOX_Y 148
#define MSGBOX_W (VGA_WIDTH - 16)
#define MSGBOX_H 44

static void scene_palette_init(void) {
    vga_set_palette_color(COL_BLACK, 0, 0, 0);
    vga_set_palette_color(COL_WHITE, 63, 63, 63);
    vga_set_palette_color(COL_SKY, 20, 38, 63);
    vga_set_palette_color(COL_GROUND, 20, 45, 15);
    vga_set_palette_color(COL_HORIZON, 12, 30, 10);
    vga_set_palette_color(COL_BOX_BG, 58, 56, 46);
    vga_set_palette_color(COL_HP_GREEN, 20, 55, 15);
    vga_set_palette_color(COL_HP_YELLOW, 60, 55, 10);
    vga_set_palette_color(COL_HP_RED, 55, 10, 10);
    vga_set_palette_color(COL_HP_BG, 25, 25, 25);
    vga_set_palette_color(COL_PLATFORM, 45, 35, 20);
    sprite_init_palette();
}

void battle_gui_enter(void) {
    vga_set_graphics_mode();
    scene_palette_init();
}

void battle_gui_exit(void) {
    vga_set_text_mode();
    /* Mode 13h drawing and text-mode character cells share the same
     * underlying VGA RAM (see vga.c) - the text screen's old contents
     * don't survive a trip through graphics mode, so leave the shell a
     * clean slate instead of a garbled one. */
    console_clear();
}

/* Concatenates up to 4 strings (any of which may be NULL) into out,
 * truncating to n-1 bytes - this kernel has no snprintf, and every
 * message this module builds is a small, fixed number of parts. */
static void gui_concat(char *out, int n, const char *a, const char *b, const char *c, const char *d) {
    int k = 0;
    if (a) {
        for (int i = 0; a[i] && k < n - 1; i++) {
            out[k++] = a[i];
        }
    }
    if (b) {
        for (int i = 0; b[i] && k < n - 1; i++) {
            out[k++] = b[i];
        }
    }
    if (c) {
        for (int i = 0; c[i] && k < n - 1; i++) {
            out[k++] = c[i];
        }
    }
    if (d) {
        for (int i = 0; d[i] && k < n - 1; i++) {
            out[k++] = d[i];
        }
    }
    out[k] = '\0';
}

/* The battlefield (sky+ground) occupies everything above the message
 * box (MSGBOX_Y); the ground band stops right at that line so the
 * player's platform/sprite never gets clipped by the box drawn on top
 * of it. */
static void draw_background(void) {
    vga_fill_rect(0, 0, VGA_WIDTH, 95, COL_SKY);
    vga_fill_rect(0, 95, VGA_WIDTH, 4, COL_HORIZON);
    vga_fill_rect(0, 99, VGA_WIDTH, MSGBOX_Y - 99, COL_GROUND);
    vga_fill_ellipse(240, 75, 45, 8, COL_PLATFORM);
    vga_fill_ellipse(75, 138, 55, 8, COL_PLATFORM);
}

static void draw_hp_box(int x, int y, const char *name, int hp, int max_hp) {
    vga_fill_rect(x, y, HPBOX_W, HPBOX_H, COL_BOX_BG);
    vga_draw_rect(x, y, HPBOX_W, HPBOX_H, COL_BLACK);
    vga_draw_text(x + 4, y + 4, name, COL_BLACK);

    int bar_x = x + 4;
    int bar_y = y + 17;
    int bar_w = HPBOX_W - 8;
    int bar_h = 6;
    vga_fill_rect(bar_x, bar_y, bar_w, bar_h, COL_HP_BG);
    if (max_hp > 0) {
        int filled = bar_w * hp / max_hp;
        if (filled < 0) {
            filled = 0;
        }
        if (filled > bar_w) {
            filled = bar_w;
        }
        uint8_t hp_color = COL_HP_GREEN;
        if (hp * 3 <= max_hp) {
            hp_color = COL_HP_RED;
        } else if (hp * 3 <= max_hp * 2) {
            hp_color = COL_HP_YELLOW;
        }
        if (filled > 0) {
            vga_fill_rect(bar_x, bar_y, filled, bar_h, hp_color);
        }
    }
    vga_draw_rect(bar_x, bar_y, bar_w, bar_h, COL_BLACK);

    char hp_buf[8];
    char max_buf[8];
    char line[24];
    gui_concat(line, sizeof(line), itoa(hp, hp_buf, 10), "/", itoa(max_hp, max_buf, 10), NULL);
    vga_draw_text(bar_x, bar_y + 9, line, COL_BLACK);
}

void battle_gui_starter_select(const char *names[3]) {
    vga_clear(COL_SKY);
    vga_fill_rect(0, 140, VGA_WIDTH, VGA_HEIGHT - 140, COL_GROUND);
    vga_draw_text(70, 12, "Choose your starter (1-3):", COL_WHITE);

    static const int xs[3] = { 24, 140, 256 };
    for (int i = 0; i < 3; i++) {
        sprite_draw(i, xs[i], 80);
        char label[4];
        label[0] = (char)('1' + i);
        label[1] = ')';
        label[2] = ' ';
        label[3] = '\0';
        char line[24];
        gui_concat(line, sizeof(line), label, names[i], NULL, NULL);
        vga_draw_text(xs[i] - 4, 150, line, COL_WHITE);
    }
}

void battle_gui_scene(int player_species, int player_hp, int player_max_hp, const char *player_name,
                       int wild_species, int wild_hp, int wild_max_hp, const char *wild_name) {
    draw_background();
    sprite_draw(wild_species, 210, 35);
    sprite_draw(player_species, 30, 100);
    draw_hp_box(8, 8, wild_name, wild_hp, wild_max_hp);
    draw_hp_box(180, 100, player_name, player_hp, player_max_hp);
}

static void draw_message_box_frame(void) {
    vga_fill_rect(MSGBOX_X, MSGBOX_Y, MSGBOX_W, MSGBOX_H, COL_BOX_BG);
    vga_draw_rect(MSGBOX_X, MSGBOX_Y, MSGBOX_W, MSGBOX_H, COL_BLACK);
    vga_draw_rect(MSGBOX_X + 2, MSGBOX_Y + 2, MSGBOX_W - 4, MSGBOX_H - 4, COL_BLACK);
}

/* There's no timer interrupt in this kernel (see input.h/game.c's RNG
 * comment) - a real "sleep milliseconds" isn't available, so this is a
 * fixed busy-wait spin, calibrated by measurement rather than a clock.
 * Without it, each message would be overwritten by the next draw call
 * before a human watching the screen could read it - the text/serial
 * side has no equivalent gap (an AI agent isn't reading pixels), so this
 * only affects the graphical panel's pacing, not the protocol. */
static void gui_pause(void) {
    for (volatile uint32_t i = 0; i < 120000000u; i++) {
    }
}

void battle_gui_message(const char *line1, const char *line2) {
    draw_message_box_frame();
    vga_draw_text(MSGBOX_X + 10, MSGBOX_Y + 9, line1, COL_BLACK);
    if (line2 && line2[0]) {
        vga_draw_text(MSGBOX_X + 10, MSGBOX_Y + 24, line2, COL_BLACK);
    }
    gui_pause();
}

void battle_gui_menu(const char *move_names[3], const int move_powers[3]) {
    draw_message_box_frame();
    int col_w = MSGBOX_W / 2;
    for (int i = 0; i < 3; i++) {
        int col = i % 2;
        int row = i / 2;
        int tx = MSGBOX_X + 10 + col * col_w;
        int ty = MSGBOX_Y + 9 + row * 16;

        char label[4];
        label[0] = (char)('1' + i);
        label[1] = ')';
        label[2] = ' ';
        label[3] = '\0';
        char pow_buf[8];
        char pow_str[12];
        gui_concat(pow_str, sizeof(pow_str), " (", itoa(move_powers[i], pow_buf, 10), ")", NULL);
        char line[28];
        gui_concat(line, sizeof(line), label, move_names[i], pow_str, NULL);
        vga_draw_text(tx, ty, line, COL_BLACK);
    }
    vga_draw_text(MSGBOX_X + 10 + col_w, MSGBOX_Y + 9 + 16, "4) Run", COL_BLACK);
}
