#pragma once

/* Blocks until a key is pressed and returns its ASCII value. Polled PS/2,
 * scancode set 1, US layout subset - no interrupts, matching the rest of
 * this single-tasking kernel. */
char keyboard_getchar(void);

/* Non-blocking: is a raw scancode byte waiting in the controller? Doesn't
 * guarantee keyboard_getchar() returns immediately (a lone modifier-key
 * scancode consumes itself with no char produced), only that calling it
 * won't busy-wait on *this* byte. */
int keyboard_has_char(void);
