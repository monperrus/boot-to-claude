#include "sprites.h"

#include "vga.h"

/* Palette layout for the three starters - plain fixed indices (16-27),
 * disjoint from battle_gui.c's scene colors (0-10), same convention as
 * vga.c's own hand-picked palette assignments elsewhere. Each creature
 * is built from vga.c's circle/ellipse/triangle primitives rather than
 * a stored pixel array - procedural shapes in the same spirit as
 * font.c's stroke-based glyphs, no external art asset involved. */

#define EMBER_BODY  16
#define EMBER_DARK  17
#define EMBER_BELLY 18
#define EMBER_FLAME 19

#define AQUA_BODY  20
#define AQUA_DARK  21
#define AQUA_BELLY 22

#define LEAF_BODY  24
#define LEAF_DARK  25
#define LEAF_BELLY 26
#define LEAF_SPOT  27

void sprite_init_palette(void) {
    vga_set_palette_color(EMBER_BODY, 63, 32, 8);
    vga_set_palette_color(EMBER_DARK, 48, 20, 4);
    vga_set_palette_color(EMBER_BELLY, 63, 55, 35);
    vga_set_palette_color(EMBER_FLAME, 63, 50, 10);

    vga_set_palette_color(AQUA_BODY, 10, 35, 63);
    vga_set_palette_color(AQUA_DARK, 5, 20, 45);
    vga_set_palette_color(AQUA_BELLY, 35, 55, 63);

    vga_set_palette_color(LEAF_BODY, 15, 45, 15);
    vga_set_palette_color(LEAF_DARK, 8, 30, 8);
    vga_set_palette_color(LEAF_BELLY, 40, 60, 30);
    vga_set_palette_color(LEAF_SPOT, 45, 55, 10);
}

static void draw_emberpup(int x, int y) {
    vga_fill_triangle(x + 8, y + 16, x + 15, y + 16, x + 11, y + 4, EMBER_DARK);
    vga_fill_triangle(x + 25, y + 16, x + 32, y + 16, x + 29, y + 4, EMBER_DARK);
    vga_fill_circle(x + 36, y + 14, 3, EMBER_FLAME);
    vga_fill_circle(x + 34, y + 20, 5, EMBER_FLAME);
    vga_fill_ellipse(x + 20, y + 24, 13, 11, EMBER_BODY);
    vga_fill_ellipse(x + 20, y + 29, 7, 6, EMBER_BELLY);
    vga_fill_circle(x + 15, y + 22, 2, 0);
    vga_fill_circle(x + 25, y + 22, 2, 0);
}

static void draw_aqualing(int x, int y) {
    vga_fill_circle(x + 34, y + 26, 4, AQUA_DARK);
    vga_fill_triangle(x + 13, y + 14, x + 27, y + 14, x + 20, y + 3, AQUA_DARK);
    vga_fill_ellipse(x + 20, y + 24, 13, 12, AQUA_BODY);
    vga_fill_ellipse(x + 20, y + 30, 7, 5, AQUA_BELLY);
    vga_fill_circle(x + 15, y + 22, 2, 0);
    vga_fill_circle(x + 25, y + 22, 2, 0);
}

static void draw_leafling(int x, int y) {
    vga_fill_triangle(x + 20, y + 13, x + 10, y + 3, x + 20, y + 3, LEAF_DARK);
    vga_fill_triangle(x + 20, y + 13, x + 30, y + 3, x + 20, y + 3, LEAF_DARK);
    vga_fill_circle(x + 20, y + 25, 13, LEAF_BODY);
    vga_fill_ellipse(x + 20, y + 30, 7, 5, LEAF_BELLY);
    vga_fill_circle(x + 13, y + 27, 2, LEAF_SPOT);
    vga_fill_circle(x + 27, y + 24, 2, LEAF_SPOT);
    vga_fill_circle(x + 15, y + 23, 2, 0);
    vga_fill_circle(x + 25, y + 23, 2, 0);
}

void sprite_draw(int species_idx, int x, int y) {
    if (species_idx == 0) {
        draw_emberpup(x, y);
    } else if (species_idx == 1) {
        draw_aqualing(x, y);
    } else if (species_idx == 2) {
        draw_leafling(x, y);
    }
}
