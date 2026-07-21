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

/* Same-size alias, not a real 64-bit type - CC's own source (as/cc/ld,
 * Phase T8's self-hosting target) declares plenty of int64_t-typed
 * fields (relocation offsets, token values), but every value ever
 * stored in them fits comfortably in 32 bits for this project's tiny
 * disk images and source files. Exactly the same reasoning as `long`
 * aliasing to a 4-byte int in parser.c's parse_type_spec - no real
 * 64-bit arithmetic exists anywhere in this codebase. */
typedef int32_t int64_t;
typedef uint32_t uint64_t;
