#include "vm.h"

#include "../console.h"
#include "../serial.h"
#include "../string.h"
#include "bytecode.h"

typedef struct {
    uint16_t return_pc;
    uint16_t locals_base;
} vm_frame_t;

typedef struct {
    int32_t value_stack[VM_VALUE_STACK_MAX];
    int sp;

    int32_t locals[VM_LOCALS_STACK_MAX];
    int locals_top;
    int locals_base;

    vm_frame_t call_stack[VM_CALL_STACK_MAX];
    int call_depth;

    int32_t globals[VM_GLOBALS_MAX];
} vm_t;

static void report_error(const char *msg) {
    console_write("vm: error: ");
    console_write(msg);
    console_write("\n");
    serial_write("vm: error: ");
    serial_write(msg);
    serial_write("\n");
}

static void vm_push(vm_t *vm, int32_t v) {
    if (vm->sp >= VM_VALUE_STACK_MAX) {
        return; /* stack overflow - silently clamp, tiny scope */
    }
    vm->value_stack[vm->sp++] = v;
}

static int32_t vm_pop(vm_t *vm) {
    if (vm->sp <= 0) {
        return 0;
    }
    return vm->value_stack[--vm->sp];
}

void vm_run(const uint8_t *file, uint32_t len) {
    if (len < sizeof(tbc_header_t)) {
        report_error("truncated .tbc file");
        return;
    }

    tbc_header_t hdr;
    memcpy(&hdr, file, sizeof(hdr));
    if (hdr.magic != TBC_MAGIC) {
        report_error("bad .tbc magic");
        return;
    }
    if (hdr.func_count > VM_FUNCS_MAX) {
        report_error("too many functions for this VM");
        return;
    }

    uint32_t off = sizeof(hdr);
    tbc_func_t funcs[VM_FUNCS_MAX];
    for (int i = 0; i < hdr.func_count; i++) {
        if (off + sizeof(tbc_func_t) > len) {
            report_error("truncated .tbc function table");
            return;
        }
        memcpy(&funcs[i], file + off, sizeof(tbc_func_t));
        off += sizeof(tbc_func_t);
    }

    if (off + hdr.code_size > len) {
        report_error("truncated .tbc code section");
        return;
    }
    const uint8_t *code = file + off;
    uint32_t code_size = hdr.code_size;

    static vm_t vm;
    memset(&vm, 0, sizeof(vm));

    uint32_t pc = hdr.entry_pc;

    for (;;) {
        if (pc >= code_size) {
            report_error("program counter ran off the end of code");
            return;
        }
        uint8_t op = code[pc++];

        switch ((bc_opcode_t)op) {
            case OP_HALT:
                return;

            case OP_PUSH_CONST: {
                int32_t v;
                memcpy(&v, code + pc, sizeof(v));
                pc += sizeof(v);
                vm_push(&vm, v);
                break;
            }
            case OP_POP:
                vm_pop(&vm);
                break;
            case OP_DUP: {
                int32_t v = vm.sp > 0 ? vm.value_stack[vm.sp - 1] : 0;
                vm_push(&vm, v);
                break;
            }
            case OP_LOAD_LOCAL: {
                uint8_t slot = code[pc++];
                vm_push(&vm, vm.locals[vm.locals_base + slot]);
                break;
            }
            case OP_STORE_LOCAL: {
                uint8_t slot = code[pc++];
                vm.locals[vm.locals_base + slot] = vm.sp > 0 ? vm.value_stack[vm.sp - 1] : 0;
                vm_pop(&vm);
                break;
            }
            case OP_LOAD_GLOBAL: {
                uint8_t slot = code[pc++];
                vm_push(&vm, vm.globals[slot]);
                break;
            }
            case OP_STORE_GLOBAL: {
                uint8_t slot = code[pc++];
                vm.globals[slot] = vm.sp > 0 ? vm.value_stack[vm.sp - 1] : 0;
                vm_pop(&vm);
                break;
            }

            case OP_ADD: { int32_t b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, a + b); break; }
            case OP_SUB: { int32_t b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, a - b); break; }
            case OP_MUL: { int32_t b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, a * b); break; }
            case OP_DIV: { int32_t b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, b != 0 ? a / b : 0); break; }
            case OP_MOD: { int32_t b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, b != 0 ? a % b : 0); break; }
            case OP_EQ:  { int32_t b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, a == b); break; }
            case OP_NEQ: { int32_t b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, a != b); break; }
            case OP_LT:  { int32_t b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, a < b); break; }
            case OP_LE:  { int32_t b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, a <= b); break; }
            case OP_GT:  { int32_t b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, a > b); break; }
            case OP_GE:  { int32_t b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, a >= b); break; }
            case OP_AND: { int32_t b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, (a != 0) && (b != 0)); break; }
            case OP_OR:  { int32_t b = vm_pop(&vm), a = vm_pop(&vm); vm_push(&vm, (a != 0) || (b != 0)); break; }
            case OP_NEG: { int32_t a = vm_pop(&vm); vm_push(&vm, -a); break; }
            case OP_NOT: { int32_t a = vm_pop(&vm); vm_push(&vm, !a); break; }

            case OP_JMP: {
                uint16_t addr;
                memcpy(&addr, code + pc, sizeof(addr));
                pc = addr;
                break;
            }
            case OP_JMP_IF_FALSE: {
                uint16_t addr;
                memcpy(&addr, code + pc, sizeof(addr));
                pc += sizeof(addr);
                int32_t cond = vm_pop(&vm);
                if (!cond) {
                    pc = addr;
                }
                break;
            }

            case OP_CALL: {
                uint16_t func_idx;
                memcpy(&func_idx, code + pc, sizeof(func_idx));
                pc += sizeof(func_idx);

                if (func_idx >= hdr.func_count) {
                    report_error("call to invalid function index");
                    return;
                }
                tbc_func_t *fe = &funcs[func_idx];

                int32_t args[VM_MAX_CALL_ARGS];
                int argc = fe->param_count;
                for (int i = argc - 1; i >= 0; i--) {
                    args[i] = vm_pop(&vm);
                }

                if (vm.call_depth >= VM_CALL_STACK_MAX) {
                    report_error("call stack overflow");
                    return;
                }
                vm.call_stack[vm.call_depth].return_pc = (uint16_t)pc;
                vm.call_stack[vm.call_depth].locals_base = (uint16_t)vm.locals_base;
                vm.call_depth++;

                if (vm.locals_top + fe->local_count > VM_LOCALS_STACK_MAX) {
                    report_error("locals stack overflow");
                    return;
                }
                int new_base = vm.locals_top;
                for (int i = 0; i < fe->local_count; i++) {
                    vm.locals[new_base + i] = (i < argc) ? args[i] : 0;
                }
                vm.locals_base = new_base;
                vm.locals_top = new_base + fe->local_count;

                pc = fe->entry_addr;
                break;
            }
            case OP_RET: {
                int32_t retval = vm_pop(&vm);
                if (vm.call_depth <= 0) {
                    report_error("return with no active call frame");
                    return;
                }
                vm.call_depth--;
                vm.locals_top = vm.locals_base;
                pc = vm.call_stack[vm.call_depth].return_pc;
                vm.locals_base = vm.call_stack[vm.call_depth].locals_base;
                vm_push(&vm, retval);
                break;
            }

            case OP_SYS_PRINT: {
                int32_t v = vm_pop(&vm);
                char buf[16];
                itoa(v, buf, 10);
                console_write(buf);
                console_write("\n");
                serial_write(buf);
                serial_write("\n");
                vm_push(&vm, 0);
                break;
            }

            default:
                report_error("invalid opcode");
                return;
        }
    }
}
