/* Forward-declared tag (typedef before the real definition) plus a
 * self-referential member inside that definition - the pattern
 * kernel/lang/script/ast.h uses for its AST node type. */
typedef struct node node_t2;

struct node {
    int value;
    struct node *next;
};

int sum_list(node_t2 *head) {
    int total = 0;
    for (node_t2 *n = head; n; n = n->next) total += n->value;
    return total;
}

int main(void) {
    struct node c, b, a;
    c.value = 3; c.next = 0;
    b.value = 2; b.next = &c;
    a.value = 1; a.next = &b;
    return sum_list(&a); /* 1+2+3 = 6 */
}
