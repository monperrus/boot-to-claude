#pragma once

#include <stdint.h>

#define VGA_WIDTH 320
#define VGA_HEIGHT 200

/* Switches the VGA card into mode 0x13 (320x200, 256 colors, one byte
 * per pixel, linear framebuffer at 0xA0000) via direct register I/O -
 * no BIOS/VBE call, matching how everything else in this kernel talks
 * to hardware directly (see docs/toolchain.md's builtin port-I/O ops).
 * The real INT 10h video BIOS is only reachable from real mode, which
 * would mean unwinding all the way back out of protected mode - direct
 * register programming is the standard bare-metal alternative. */
void vga_set_graphics_mode(void);

/* Switches back to mode 0x03 (80x25, 16-color text), the mode
 * console.c already assumes - called when a graphical screen (the game)
 * hands control back to the shell. */
void vga_set_text_mode(void);

void vga_set_palette_color(uint8_t index, uint8_t r, uint8_t g, uint8_t b);

void vga_clear(uint8_t color);
void vga_put_pixel(int x, int y, uint8_t color);
void vga_fill_rect(int x, int y, int w, int h, uint8_t color);
void vga_draw_rect(int x, int y, int w, int h, uint8_t color);

/* 5x7 pixel bitmap font (kernel/font.c) - covers the ASCII range this
 * game's text actually uses. */
void vga_draw_char(int x, int y, char c, uint8_t color);
void vga_draw_text(int x, int y, const char *s, uint8_t color);
int vga_text_width(const char *s);