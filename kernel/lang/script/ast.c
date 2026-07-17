#include "ast.h"

#include "../../memory.h"
#include "../../string.h"

/* Nodes are allocated from the kernel heap and never freed - a script's
 * whole AST lives for the duration of one `run`. Fine for tiny scripts;
 * the heap resets on reboot. */
ast_node_t *ast_new(ast_kind_t kind) {
    ast_node_t *n = kmalloc(sizeof(ast_node_t));
    memset(n, 0, sizeof(*n));
    n->kind = kind;
    return n;
}
