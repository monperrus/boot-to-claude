#pragma once
/* Hand-rolled freestanding stdint.h for CC - just the typedefs this
 * codebase actually uses (no 64-bit types, so CC's type-specifier
 * grammar never needs to support `long`). Real gcc bundles one of these
 * for free; CC needs its own since it doesn't ship a libc. */

typedef signed char    int8_t;
typedef unsigned char   uint8_t;
typedef signed short     int16_t;
typedef unsigned short    uint16_t;
typedef signed int          int32_t;
typedef unsigned int         uint32_t;
