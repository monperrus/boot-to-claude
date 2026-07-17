#include "shell.h"

#include <stdint.h>

#include "console.h"
#include "fs/tinyfs.h"
#include "keyboard.h"
#include "lang/compiler/codegen.h"
#include "lang/compiler/parser.h"
#include "lang/script/eval.h"
#include "lang/vm.h"
#include "serial.h"
#include "string.h"

#define LINE_MAX 128

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void echo_char(char c) {
    console_putc(c);
    serial_putc(c);
}

static void print_line(const char *s) {
    console_write(s);
    console_write("\n");
    serial_write(s);
    serial_write("\n");
}

static int read_line(char *buf, int max_len) {
    int len = 0;
    for (;;) {
        char c = keyboard_getchar();
        if (c == '\n') {
            echo_char('\n');
            buf[len] = '\0';
            return len;
        } else if (c == '\b') {
            if (len > 0) {
                len--;
                echo_char('\b');
                echo_char(' ');
                echo_char('\b');
            }
        } else if (c >= 32 && c < 127 && len < max_len - 1) {
            buf[len++] = c;
            echo_char(c);
        }
    }
}

static void skip_spaces(const char **p) {
    while (**p == ' ') {
        (*p)++;
    }
}

static int next_word(const char **p, char *out, int max_len) {
    skip_spaces(p);
    int n = 0;
    while (**p && **p != ' ' && n < max_len - 1) {
        out[n++] = *(*p)++;
    }
    out[n] = '\0';
    return n;
}

static void cmd_help(void) {
    print_line("commands: ls, cat <file>, write <file> <text>, rm <file>, run <file>,");
    print_line("          build <src> <out>, exec <file>, help, clear, reboot");
}

static void cmd_ls(void) {
    tinyfs_dirent_t entries[TINYFS_NUM_INODES];
    int n = tinyfs_list(entries, TINYFS_NUM_INODES);
    char numbuf[16];
    for (int i = 0; i < n; i++) {
        console_write(entries[i].name);
        console_write("  ");
        console_write(itoa((int)entries[i].size, numbuf, 10));
        console_write("\n");
        serial_write(entries[i].name);
        serial_write("  ");
        serial_write(itoa((int)entries[i].size, numbuf, 10));
        serial_write("\n");
    }
}

static void cmd_cat(const char *name) {
    static char buf[TINYFS_MAX_FILE_SIZE + 1];
    uint32_t len = 0;
    if (tinyfs_read(name, buf, sizeof(buf) - 1, &len) != 0) {
        print_line("cat: no such file");
        return;
    }
    buf[len] = '\0';
    console_write(buf);
    serial_write(buf);
    if (len == 0 || buf[len - 1] != '\n') {
        console_write("\n");
        serial_write("\n");
    }
}

static void cmd_write(const char *name, const char *contents) {
    tinyfs_create(name); /* ignored if it already exists - write() below overwrites it */
    uint32_t len = (uint32_t)strlen(contents);
    if (len > TINYFS_MAX_FILE_SIZE) {
        print_line("write: content too long");
        return;
    }
    if (tinyfs_write(name, contents, len) != 0) {
        print_line("write: failed (no space or bad name)");
        return;
    }
    print_line("write: ok");
}

static void cmd_run(const char *name) {
    static char buf[TINYFS_MAX_FILE_SIZE + 1];
    uint32_t len = 0;
    if (tinyfs_read(name, buf, sizeof(buf) - 1, &len) != 0) {
        print_line("run: no such file");
        return;
    }
    buf[len] = '\0';
    script_run(buf);
}

static void cmd_build(const char *src_name, const char *out_name) {
    static char src[TINYFS_MAX_FILE_SIZE + 1];
    uint32_t src_len = 0;
    if (tinyfs_read(src_name, src, sizeof(src) - 1, &src_len) != 0) {
        print_line("build: no such file");
        return;
    }
    src[src_len] = '\0';

    tc_parser_t p;
    tc_parser_init(&p, src);
    ast_node_t *prog = tc_parse_program(&p);
    if (p.error) {
        console_write("build: parse error: ");
        console_write(p.error_msg);
        console_write("\n");
        serial_write("build: parse error: ");
        serial_write(p.error_msg);
        serial_write("\n");
        return;
    }

    static uint8_t out_buf[TINYFS_MAX_FILE_SIZE];
    uint32_t out_len = 0;
    char err[64];
    if (codegen_compile(prog, out_buf, sizeof(out_buf), &out_len, err, sizeof(err)) != 0) {
        console_write("build: compile error: ");
        console_write(err);
        console_write("\n");
        serial_write("build: compile error: ");
        serial_write(err);
        serial_write("\n");
        return;
    }

    tinyfs_create(out_name);
    if (tinyfs_write(out_name, out_buf, out_len) != 0) {
        print_line("build: failed to write output file");
        return;
    }
    print_line("build: ok");
}

static void cmd_exec(const char *name) {
    static uint8_t buf[TINYFS_MAX_FILE_SIZE];
    uint32_t len = 0;
    if (tinyfs_read(name, buf, sizeof(buf), &len) != 0) {
        print_line("exec: no such file");
        return;
    }
    vm_run(buf, len);
}

static void cmd_rm(const char *name) {
    if (tinyfs_delete(name) != 0) {
        print_line("rm: no such file");
    } else {
        print_line("rm: ok");
    }
}

static void cmd_reboot(void) {
    print_line("rebooting...");
    uint8_t status;
    do {
        status = inb(0x64);
    } while (status & 0x02);
    outb(0x64, 0xFE); /* pulse the keyboard controller's reset line */
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void shell_run(void) {
    static char line[LINE_MAX];

    print_line("tiny-os shell. type 'help' for commands.");

    for (;;) {
        console_write("> ");
        serial_write("> ");
        read_line(line, sizeof(line));

        const char *p = line;
        char cmd[32];
        if (next_word(&p, cmd, sizeof(cmd)) == 0) {
            continue;
        }

        if (strcmp(cmd, "help") == 0) {
            cmd_help();
        } else if (strcmp(cmd, "ls") == 0) {
            cmd_ls();
        } else if (strcmp(cmd, "clear") == 0) {
            console_clear();
        } else if (strcmp(cmd, "reboot") == 0) {
            cmd_reboot();
        } else if (strcmp(cmd, "cat") == 0) {
            char name[TINYFS_NAME_MAX];
            if (next_word(&p, name, sizeof(name)) == 0) {
                print_line("usage: cat <file>");
            } else {
                cmd_cat(name);
            }
        } else if (strcmp(cmd, "rm") == 0) {
            char name[TINYFS_NAME_MAX];
            if (next_word(&p, name, sizeof(name)) == 0) {
                print_line("usage: rm <file>");
            } else {
                cmd_rm(name);
            }
        } else if (strcmp(cmd, "run") == 0) {
            char name[TINYFS_NAME_MAX];
            if (next_word(&p, name, sizeof(name)) == 0) {
                print_line("usage: run <file>");
            } else {
                cmd_run(name);
            }
        } else if (strcmp(cmd, "write") == 0) {
            char name[TINYFS_NAME_MAX];
            if (next_word(&p, name, sizeof(name)) == 0) {
                print_line("usage: write <file> <text>");
            } else {
                skip_spaces(&p);
                cmd_write(name, p);
            }
        } else if (strcmp(cmd, "build") == 0) {
            char src_name[TINYFS_NAME_MAX];
            char out_name[TINYFS_NAME_MAX];
            if (next_word(&p, src_name, sizeof(src_name)) == 0 ||
                next_word(&p, out_name, sizeof(out_name)) == 0) {
                print_line("usage: build <src> <out>");
            } else {
                cmd_build(src_name, out_name);
            }
        } else if (strcmp(cmd, "exec") == 0) {
            char name[TINYFS_NAME_MAX];
            if (next_word(&p, name, sizeof(name)) == 0) {
                print_line("usage: exec <file>");
            } else {
                cmd_exec(name);
            }
        } else {
            print_line("unknown command (type 'help')");
        }
    }
}
