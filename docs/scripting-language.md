# tsh scripting language

The dynamic, tree-walking counterpart to the [tiny compiler](bytecode-isa.md).
Run with the shell's `run <file>` command; see `tests/hello.tsh` for a
worked example.

## Syntax

C-like statements, no semicolons after blocks:

```
x = 2 + 3 * 4;
print("x =", x);

func add(a, b) {
    return a + b;
}
print("add(3,4) =", add(3, 4));

i = 0;
while (i < 3) {
    print("loop", i);
    i = i + 1;
}
```

- Types: 32-bit integers and strings (`"..."`). No arrays, no booleans -
  `0`/empty-string are falsy, everything else truthy.
- Variables: no declarations: first assignment creates them. All
  variables in a script share one global scope, *except* inside a
  function body, which gets its own isolated local scope - functions
  cannot see the caller's variables or each other's (no closures).
- Control flow: `if (...) {...} else {...}` (including `else if` chains),
  `while (...) {...}`.
- Functions: `func name(params) {...}`, `return expr;` (or bare `return;`
  / falling off the end, both give `0`).
- Operators: `+ - * / %`, `== != < <= > >=`, `&& ||` (short-circuit),
  unary `- !`. `+` on any string operand concatenates (numbers are
  stringified).
- Comments: `# to end of line`.

## Builtins

| call | effect |
|---|---|
| `print(a, b, ...)` | writes args space-separated + newline to console and serial |
| `fread(name)` | returns a file's contents as a string, or `""` if missing |
| `fwrite(name, contents)` | creates/overwrites a tinyfs file, returns `1`/`0` |
| `flist()` | prints every file name (one per line), returns the count |

## Implementation

`kernel/lang/lexer.c` (shared with the compiler) tokenizes;
`kernel/lang/script/parser.c` builds an AST (`ast.h`) via recursive
descent; `kernel/lang/script/eval.c` tree-walks it directly - there is no
intermediate bytecode, unlike the compiled language. AST nodes and string
literals are allocated from the kernel heap and never freed; fine for a
single `run` of a short script, since the heap resets on reboot.
