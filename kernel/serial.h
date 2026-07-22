#pragma once

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);

/* Non-blocking: is there a byte waiting to be read from COM1? */
int serial_has_char(void);
/* Blocks until a byte arrives on COM1 and returns it. */
char serial_getc(void);
