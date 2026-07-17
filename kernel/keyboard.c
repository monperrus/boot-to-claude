#include "keyboard.h"

#include <stddef.h>
#include <stdint.h>

#define KBD_DATA   0x60
#define KBD_STATUS 0x64

#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Scancode set 1, indexed by make code. 0 = unmapped/non-printable. */
static const char lower_table[] = {
    0,    27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0 /*ctrl*/, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0 /*lshift*/, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0 /*rshift*/, '*', 0 /*alt*/, ' ',
};

static const char upper_table[] = {
    0,    27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0, '*', 0, ' ',
};

#define TABLE_LEN (sizeof(lower_table) / sizeof(lower_table[0]))

static int shift_down = 0;

static int scancode_ready(void) {
    return inb(KBD_STATUS) & 1;
}

char keyboard_getchar(void) {
    for (;;) {
        while (!scancode_ready()) {}
        uint8_t sc = inb(KBD_DATA);

        int release = sc & 0x80;
        uint8_t code = sc & 0x7F;

        if (code == SC_LSHIFT || code == SC_RSHIFT) {
            shift_down = !release;
            continue;
        }
        if (release || code >= TABLE_LEN) {
            continue;
        }

        char c = shift_down ? upper_table[code] : lower_table[code];
        if (c == 0) {
            continue;
        }
        return c;
    }
}
