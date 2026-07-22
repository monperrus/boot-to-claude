#pragma once

#include <stdint.h>

#define FONT_W 5
#define FONT_H 7

/* Returns FONT_H bytes, one per row, each row's bottom FONT_W bits are
 * the pixels left-to-right (bit FONT_W-1 = leftmost). Covers 0x20..0x7A
 * (space through 'z'); anything else - or a character in that range
 * with no defined glyph, e.g. punctuation this game never prints -
 * comes back blank rather than garbage. */
const uint8_t *font_glyph(char c);