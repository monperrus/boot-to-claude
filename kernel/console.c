#include "console.h"

#include <stddef.h>
#include <stdint.h>

#define VGA_MEM ((volatile uint16_t *)0xB8000)
#define VGA_COLS 80
#define VGA_ROWS 25
#define VGA_ATTR 0x07

static size_t cursor_row = 0;
static size_t cursor_col = 0;

static void console_scroll(void) {
    for (size_t row = 1; row < VGA_ROWS; row++) {
        for (size_t col = 0; col < VGA_COLS; col++) {
            VGA_MEM[(row - 1) * VGA_COLS + col] = VGA_MEM[row * VGA_COLS + col];
        }
    }
    for (size_t col = 0; col < VGA_COLS; col++) {
        VGA_MEM[(VGA_ROWS - 1) * VGA_COLS + col] = (uint16_t)((VGA_ATTR << 8) | ' ');
    }
    cursor_row = VGA_ROWS - 1;
}

void console_clear(void) {
    for (size_t i = 0; i < VGA_COLS * VGA_ROWS; i++) {
        VGA_MEM[i] = (uint16_t)((VGA_ATTR << 8) | ' ');
    }
    cursor_row = 0;
    cursor_col = 0;
}

void console_putc(char c) {
    if (c == '\n') {
        cursor_col = 0;
        cursor_row++;
    } else if (c == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
        } else if (cursor_row > 0) {
            cursor_row--;
            cursor_col = VGA_COLS - 1;
        }
    } else {
        VGA_MEM[cursor_row * VGA_COLS + cursor_col] = (uint16_t)((VGA_ATTR << 8) | (uint8_t)c);
        cursor_col++;
        if (cursor_col >= VGA_COLS) {
            cursor_col = 0;
            cursor_row++;
        }
    }
    if (cursor_row >= VGA_ROWS) {
        console_scroll();
    }
}

void console_write(const char *s) {
    while (*s) {
        console_putc(*s++);
    }
}
