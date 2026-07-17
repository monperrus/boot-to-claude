#pragma once

/* Shared between kernel/lang/compiler/codegen.c (producer) and
 * kernel/lang/vm.c (consumer). See docs/bytecode-isa.md. */

#include <stdint.h>

#define TBC_MAGIC 0x31434254u /* "TBC1" */

typedef enum {
    OP_HALT = 0,
    OP_PUSH_CONST,
    OP_POP,
    OP_DUP,
    OP_LOAD_LOCAL,
    OP_STORE_LOCAL,
    OP_LOAD_GLOBAL,
    OP_STORE_GLOBAL,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_EQ,
    OP_NEQ,
    OP_LT,
    OP_LE,
    OP_GT,
    OP_GE,
    OP_AND,
    OP_OR,
    OP_NEG,
    OP_NOT,
    OP_JMP,
    OP_JMP_IF_FALSE,
    OP_CALL,
    OP_RET,
    OP_SYS_PRINT,
} bc_opcode_t;

typedef struct {
    uint32_t magic;
    uint16_t entry_pc;
    uint16_t global_count;
    uint16_t func_count;
    uint16_t code_size;
} __attribute__((packed)) tbc_header_t;

typedef struct {
    uint16_t entry_addr;
    uint8_t  param_count;
    uint8_t  local_count;
} __attribute__((packed)) tbc_func_t;

#define VM_FUNCS_MAX 16
#define VM_GLOBALS_MAX 32
#define VM_LOCALS_PER_FRAME_MAX 32 /* param_count + other locals, per function */
#define VM_MAX_CALL_ARGS 8

/* vm.c run-time limits */
#define VM_VALUE_STACK_MAX 256
#define VM_LOCALS_STACK_MAX 512 /* sum of live frames' local_count */
#define VM_CALL_STACK_MAX 32
