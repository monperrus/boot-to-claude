#pragma once
/* va_list is just a byte pointer into the caller's cdecl argument area -
 * see toolchain/cc/parser.c for how va_start/va_arg/va_end are actually
 * implemented (as parser-level desugaring, not macros - CC's
 * preprocessor has no function-like macros, and va_arg's second
 * argument is a type name anyway, which no ordinary function/macro call
 * could accept). This header exists only so `va_list ap;` has a type to
 * name; va_start/va_arg/va_end themselves need no declaration here. */
typedef char *va_list;
