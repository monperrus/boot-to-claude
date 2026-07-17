#pragma once

/* Blocks until a key is pressed and returns its ASCII value. Polled PS/2,
 * scancode set 1, US layout subset - no interrupts, matching the rest of
 * this single-tasking kernel. */
char keyboard_getchar(void);
