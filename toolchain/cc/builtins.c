#include "cc.h"

#include <stdio.h>
#include <string.h>

/* The fixed builtin list this compiler supports, in place of a general
 * GCC-extended-inline-asm parser (see docs/toolchain.md for why - the
 * kernel's actual inline-asm usage reduces to exactly these ~7
 * operations). Each maps 1:1 to a short, fixed instruction sequence;
 * arguments are evaluated left-to-right via the normal expression
 * codegen, matching how a real function call would evaluate them,
 * then moved into the fixed registers x86 port I/O and syscalls
 * require. `emit_arg(node)` is codegen_expr (result in EAX);
 * `emitf(fmt, ...)` writes a line of assembly text. */

typedef void (*emit_arg_fn)(node_t *);
typedef void (*emitf_fn)(const char *, ...);

static int g_builtin_label_id;

static node_t *nth(node_t *args, int i) {
    for (; args && i > 0; args = args->next, i--) {}
    return args;
}

int is_builtin_name(const char *name) {
    static const char *names[] = {
        "__builtin_inb", "__builtin_outb", "__builtin_inw", "__builtin_outw",
        "__builtin_lidt", "__builtin_gdt_flush", "__builtin_hlt",
        "__sys_open", "__sys_read", "__sys_write", "__sys_lseek", "__sys_close", "__sys_exit",
        NULL,
    };
    for (int i = 0; names[i]; i++) if (strcmp(name, names[i]) == 0) return 1;
    return 0;
}

/* Returns 1 if `name` was a builtin (and emitted code for it), 0
 * otherwise (ordinary call - the caller falls back to a normal `call`). */
int builtin_emit_call(const char *name, node_t *args, cc_target_t target,
                       emit_arg_fn emit_arg, emitf_fn emitf) {
    (void)target;

    /* --- kernel port I/O + privileged instructions --- */
    if (strcmp(name, "__builtin_outb") == 0) {
        emit_arg(nth(args, 0)); emitf("    push eax\n");    /* port */
        emit_arg(nth(args, 1)); emitf("    mov ecx, eax\n");  /* value */
        emitf("    pop eax\n");     /* eax = port */
        emitf("    mov edx, eax\n"); /* edx = port (dx usable) */
        emitf("    mov eax, ecx\n");  /* eax = value (al usable) */
        emitf("    out dx, al\n");
        return 1;
    }
    if (strcmp(name, "__builtin_inb") == 0) {
        emit_arg(nth(args, 0)); /* port */
        emitf("    mov edx, eax\n    in al, dx\n    movzx eax, al\n");
        return 1;
    }
    if (strcmp(name, "__builtin_outw") == 0) {
        emit_arg(nth(args, 0)); emitf("    push eax\n");
        emit_arg(nth(args, 1)); emitf("    mov ecx, eax\n");
        emitf("    pop eax\n    mov edx, eax\n    mov eax, ecx\n    out dx, ax\n");
        return 1;
    }
    if (strcmp(name, "__builtin_inw") == 0) {
        emit_arg(nth(args, 0));
        emitf("    mov edx, eax\n    in ax, dx\n    movzx eax, ax\n");
        return 1;
    }
    if (strcmp(name, "__builtin_hlt") == 0) { emitf("    hlt\n"); return 1; }
    if (strcmp(name, "__builtin_lidt") == 0) {
        emit_arg(nth(args, 0)); /* pointer to the idt descriptor */
        emitf("    mov ecx, eax\n    lidt [ecx]\n");
        return 1;
    }
    if (strcmp(name, "__builtin_gdt_flush") == 0) {
        /* Loads the GDT, reloads every data/stack segment to the flat
         * data selector (0x10, matching gdt.c's layout), and far-jumps
         * to the flat code selector (0x08) to reload CS - the one
         * sequence in the kernel that inherently can't be a single
         * instruction. A unique label per call site (own counter,
         * ".Lbi" prefix so it can never collide with codegen.c's own
         * ".L" control-flow labels) since this can be called more than
         * once in a translation unit. */
        char lbl[MAX_NAME];
        snprintf(lbl, sizeof(lbl), ".Lbi%d", g_builtin_label_id++);
        emit_arg(nth(args, 0)); /* pointer to the gdt descriptor */
        emitf("    mov ecx, eax\n    lgdt [ecx]\n");
        emitf("    mov ax, 0x10\n    mov ds, ax\n    mov es, ax\n    mov fs, ax\n    mov gs, ax\n    mov ss, ax\n");
        emitf("    jmp 0x08:%s\n", lbl);
        emitf("%s:\n", lbl);
        return 1;
    }

    /* --- raw Linux syscalls (host target only - see docs/toolchain.md) --- */
    if (strcmp(name, "__sys_exit") == 0) {
        emit_arg(nth(args, 0)); emitf("    mov ebx, eax\n    mov eax, 1\n    int 0x80\n");
        return 1;
    }
    if (strcmp(name, "__sys_close") == 0) {
        emit_arg(nth(args, 0)); emitf("    mov ebx, eax\n    mov eax, 6\n    int 0x80\n");
        return 1;
    }
    if (strcmp(name, "__sys_open") == 0) {
        emit_arg(nth(args, 0)); emitf("    push eax\n");   /* path */
        emit_arg(nth(args, 1)); emitf("    push eax\n");     /* flags */
        emitf("    mov edx, 420\n"); /* mode 0644, used only when O_CREAT is set */
        emitf("    pop ecx\n    pop ebx\n    mov eax, 5\n    int 0x80\n");
        return 1;
    }
    if (strcmp(name, "__sys_read") == 0 || strcmp(name, "__sys_write") == 0) {
        int nr = strcmp(name, "__sys_read") == 0 ? 3 : 4;
        emit_arg(nth(args, 0)); emitf("    push eax\n");    /* fd */
        emit_arg(nth(args, 1)); emitf("    push eax\n");      /* buf */
        emit_arg(nth(args, 2)); emitf("    mov edx, eax\n");    /* count */
        emitf("    pop ecx\n    pop ebx\n    mov eax, %d\n    int 0x80\n", nr);
        return 1;
    }
    if (strcmp(name, "__sys_lseek") == 0) {
        emit_arg(nth(args, 0)); emitf("    push eax\n");    /* fd */
        emit_arg(nth(args, 1)); emitf("    push eax\n");      /* offset */
        emit_arg(nth(args, 2)); emitf("    mov edx, eax\n");    /* whence */
        emitf("    pop ecx\n    pop ebx\n    mov eax, 19\n    int 0x80\n");
        return 1;
    }

    return 0;
}
