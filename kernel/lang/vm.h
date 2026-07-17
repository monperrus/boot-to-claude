#pragma once

#include <stdint.h>

/* Executes a compiled .tbc program (see docs/bytecode-isa.md). Reports
 * errors to console+serial the same way script_run does. */
void vm_run(const uint8_t *bytecode, uint32_t len);
