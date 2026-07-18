#include "cc.h"

#include <stdlib.h>
#include <string.h>

type_t *type_new(type_kind_t kind) {
    type_t *t = calloc(1, sizeof(type_t));
    t->kind = kind;
    return t;
}

node_t *node_new(node_kind_t kind) {
    node_t *n = calloc(1, sizeof(node_t));
    n->kind = kind;
    return n;
}

type_t *ptr_to(type_t *base) {
    type_t *t = type_new(TY_PTR);
    t->base = base;
    t->size = 4;
    t->align = 4;
    return t;
}

int type_is_int(type_t *t) {
    return t->kind == TY_CHAR || t->kind == TY_SHORT || t->kind == TY_INT || t->kind == TY_ENUM;
}

static int round_up(int n, int align) { return (n + align - 1) / align * align; }

int type_align(type_t *t) {
    if (t->align) return t->align;
    switch (t->kind) {
        case TY_CHAR: t->align = 1; break;
        case TY_SHORT: t->align = 2; break;
        case TY_INT: case TY_ENUM: case TY_PTR: t->align = 4; break;
        case TY_ARRAY: t->align = type_align(t->base); break;
        case TY_STRUCT: case TY_UNION: {
            int a = 1;
            for (member_t *m = t->members; m; m = m->next) {
                int ma = t->is_packed ? 1 : type_align(m->type);
                if (ma > a) a = ma;
            }
            t->align = a ? a : 1;
            break;
        }
        default: t->align = 1; break;
    }
    return t->align;
}

int type_size(type_t *t) {
    if (t->size) return t->size;
    switch (t->kind) {
        case TY_VOID: t->size = 0; break;
        case TY_CHAR: t->size = 1; break;
        case TY_SHORT: t->size = 2; break;
        case TY_INT: case TY_ENUM: case TY_PTR: t->size = 4; break;
        case TY_ARRAY: t->size = type_size(t->base) * (t->array_len > 0 ? t->array_len : 0); break;
        case TY_UNION: {
            int sz = 0;
            for (member_t *m = t->members; m; m = m->next) {
                int msz = type_size(m->type);
                if (msz > sz) sz = msz;
            }
            int a = type_align(t);
            t->size = round_up(sz, a);
            break;
        }
        case TY_STRUCT: {
            int off = 0;
            int a = t->is_packed ? 1 : type_align(t);
            for (member_t *m = t->members; m; m = m->next) {
                int ma = t->is_packed ? 1 : type_align(m->type);
                off = round_up(off, ma);
                m->offset = off;
                off += type_size(m->type);
            }
            t->size = t->is_packed ? off : round_up(off, a);
            break;
        }
        default: t->size = 0; break;
    }
    return t->size;
}
