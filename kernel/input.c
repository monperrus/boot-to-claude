#include "input.h"

#include "keyboard.h"
#include "serial.h"

static unsigned int g_ticks;

char input_getchar(void) {
    for (;;) {
        if (keyboard_has_char()) {
            return keyboard_getchar();
        }
        if (serial_has_char()) {
            return serial_getc();
        }
        g_ticks++;
    }
}

unsigned int input_ticks(void) {
    return g_ticks;
}