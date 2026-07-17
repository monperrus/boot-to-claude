#pragma once

#include <stdint.h>

#include "../script/ast.h"

/* Compiles a parsed program into a .tbc byte buffer (see
 * docs/bytecode-isa.md). Returns 0 on success and fills *out_len; on
 * failure returns -1 and fills error_msg. out_buf must be at least
 * out_buf_size bytes. */
int codegen_compile(ast_node_t *prog, uint8_t *out_buf, uint32_t out_buf_size,
                     uint32_t *out_len, char *error_msg, uint32_t error_msg_size);
