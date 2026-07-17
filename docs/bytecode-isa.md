# tiny compiler bytecode ISA

A stack machine, similar in spirit to tinyfs: minimal, fixed-size, no
dynamic growth. Values are 32-bit signed integers only - the compiled
language (unlike the scripting language) has no string type.

## Design invariant

Every expression, once compiled, leaves **exactly one** value on the value
stack. Statement-level wrappers (`OP_POP` after an expression-statement)
and binary/unary ops (pop N, push 1) all rely on this. Assignment is an
expression too: it evaluates to the assigned value, via `<value> DUP
STORE_*` rather than a plain `STORE_*` that would leave nothing behind.

## VM state

Four fixed-size arrays, no heap allocation at run time:

- **value stack** - expression operands/results.
- **locals stack** - one contiguous region per active call frame (params
  then other locals, slot-indexed - names are resolved to slots at compile
  time, the VM has no notion of variable names).
- **call stack** - `{return_pc, locals_base}` per active call, for nested
  and recursive calls.
- **globals** - fixed array, slot-indexed, for variables assigned outside
  any function.

Functions cannot see globals or an enclosing scope - same "no closures"
rule as the scripting language's interpreter (kernel/lang/script/eval.c),
so both languages behave consistently.

## Opcodes

| op | value | operand | stack effect |
|---|---|---|---|
| `OP_HALT` | 0 | - | stops execution |
| `OP_PUSH_CONST` | 1 | i32 | push const |
| `OP_POP` | 2 | - | pop |
| `OP_DUP` | 3 | - | push top again |
| `OP_LOAD_LOCAL` | 4 | u8 slot | push locals[base+slot] |
| `OP_STORE_LOCAL` | 5 | u8 slot | pop -> locals[base+slot] |
| `OP_LOAD_GLOBAL` | 6 | u8 slot | push globals[slot] |
| `OP_STORE_GLOBAL` | 7 | u8 slot | pop -> globals[slot] |
| `OP_ADD`..`OP_OR` | 8-20 | - | pop 2, push 1 (`+ - * / % == != < <= > >= && \|\|`) |
| `OP_NEG` | 21 | - | pop 1, push 1 |
| `OP_NOT` | 22 | - | pop 1, push 1 |
| `OP_JMP` | 23 | u16 addr | `pc = addr` |
| `OP_JMP_IF_FALSE` | 24 | u16 addr | pop cond; if false, `pc = addr` |
| `OP_CALL` | 25 | u16 func index | pop `param_count` args, push new frame, jump to entry |
| `OP_RET` | 26 | - | pop return value, pop frame, push return value back |
| `OP_SYS_PRINT` | 27 | - | pop 1, print as decimal + newline, push 0 |

`OP_AND`/`OP_OR` are eager (both operands always evaluated), unlike the
scripting language's short-circuit `&&`/`||` - a deliberate simplification
for the compiled path.

Division/modulo by zero yields 0 rather than trapping, matching the
scripting language's behavior.

## File format (`.tbc`)

```
struct tbc_header {        // 12 bytes, packed
    uint32_t magic;         // 'T','B','C','1'
    uint16_t entry_pc;       // where top-level code starts, within code[]
    uint16_t global_count;
    uint16_t func_count;
    uint16_t code_size;
};

struct tbc_func {          // 4 bytes, packed, func_count of these follow the header
    uint16_t entry_addr;    // offset within code[]
    uint8_t  param_count;
    uint8_t  local_count;   // includes params, which occupy slots 0..param_count-1
};

uint8_t code[code_size];   // follows the func table
```

The whole file must fit in one tinyfs file (`TINYFS_MAX_FILE_SIZE`, 4KB).

## Source language

C-like and statically scoped: `int`-only variables (no declarations -
first assignment creates the variable, in whichever scope - global or
current function - is active), `if`/`else`, `while`, `func name(params)
{...}` with `return`, arithmetic/comparison/logical operators, and one
builtin, `print(expr)`. Parser: `kernel/lang/compiler/parser.c`, reusing
the scripting language's AST node type (`kernel/lang/script/ast.h`) since
the tree shape is identical minus string literals. Codegen:
`kernel/lang/compiler/codegen.c`. VM: `kernel/lang/vm.c`.

Shell usage: `build prog.tc prog.tbc` compiles, `exec prog.tbc` runs it.
