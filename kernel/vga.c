#include "vga.h"

#include "builtins.h"
#include "font.h"

static inline void outb(uint16_t port, uint8_t val) { __builtin_outb(port, val); }
static inline uint8_t inb(uint16_t port) { return __builtin_inb(port); }

#define VGA_MISC_WRITE 0x3C2
#define VGA_SEQ_INDEX  0x3C4
#define VGA_SEQ_DATA   0x3C5
#define VGA_CRTC_INDEX 0x3D4
#define VGA_CRTC_DATA  0x3D5
#define VGA_GC_INDEX   0x3CE
#define VGA_GC_DATA    0x3CF
#define VGA_AC_INDEX   0x3C0
#define VGA_AC_WRITE   0x3C0
#define VGA_INSTAT_READ 0x3DA
#define VGA_DAC_WRITE_INDEX 0x3C8
#define VGA_DAC_DATA        0x3C9

#define FRAMEBUFFER ((uint8_t *)0xA0000)

/* Standard register dumps for mode 0x13 (320x200x256) and mode 0x03
 * (80x25 text) - the same fixed sequences of Misc/Sequencer/CRTC/
 * Graphics/Attribute Controller values used throughout the bare-metal
 * OS dev community to switch VGA modes via direct port I/O instead of
 * the (real-mode-only) INT 10h BIOS video service. Hardware register
 * layout, not code - the same kind of fixed reference table as the PS/2
 * scancode set in keyboard.c or the ATA command bytes in ata.c. */

static const uint8_t MODE13_SEQ[5] = { 0x03, 0x01, 0x0F, 0x00, 0x0E };
static const uint8_t MODE13_CRTC[25] = {
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
    0x00, 0x41, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3,
    0xFF,
};
static const uint8_t MODE13_GC[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF };
static const uint8_t MODE13_AC[21] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x41, 0x00, 0x0F, 0x00, 0x00,
};

static const uint8_t TEXT_SEQ[5] = { 0x03, 0x00, 0x03, 0x00, 0x02 };
static const uint8_t TEXT_CRTC[25] = {
    0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
    0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x00,
    0x9C, 0x0E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3,
    0xFF,
};
static const uint8_t TEXT_GC[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF };
static const uint8_t TEXT_AC[21] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x0C, 0x00, 0x0F, 0x08, 0x00,
};

static void vga_load_registers(uint8_t misc, const uint8_t *seq, const uint8_t *crtc,
                                 const uint8_t *gc, const uint8_t *ac) {
    outb(VGA_MISC_WRITE, misc);

    for (int i = 0; i < 5; i++) {
        outb(VGA_SEQ_INDEX, (uint8_t)i);
        outb(VGA_SEQ_DATA, seq[i]);
    }

    /* CRTC registers 0-7 are write-protected until bit 7 of index 0x03
     * and the lock bit of index 0x11 are cleared - unlock first. */
    outb(VGA_CRTC_INDEX, 0x03);
    outb(VGA_CRTC_DATA, (uint8_t)(inb(VGA_CRTC_DATA) | 0x80));
    outb(VGA_CRTC_INDEX, 0x11);
    outb(VGA_CRTC_DATA, (uint8_t)(inb(VGA_CRTC_DATA) & ~0x80));
    /* Re-apply the real values for those two indices below anyway (they
     * come from the table), so this only matters transiently. */
    for (int i = 0; i < 25; i++) {
        outb(VGA_CRTC_INDEX, (uint8_t)i);
        outb(VGA_CRTC_DATA, crtc[i]);
    }

    for (int i = 0; i < 9; i++) {
        outb(VGA_GC_INDEX, (uint8_t)i);
        outb(VGA_GC_DATA, gc[i]);
    }

    /* The Attribute Controller's index and data are the same port,
     * distinguished by an internal flip-flop that's reset by reading
     * the input status register. */
    (void)inb(VGA_INSTAT_READ);
    for (int i = 0; i < 21; i++) {
        outb(VGA_AC_INDEX, (uint8_t)i);
        outb(VGA_AC_WRITE, ac[i]);
    }
    (void)inb(VGA_INSTAT_READ);
    outb(VGA_AC_INDEX, 0x20); /* re-enable video output (PAS bit) */
}

/* Mode 0x13's linear framebuffer at 0xA0000 and text mode's character
 * generator both live in the same underlying VGA RAM, just addressed
 * differently - "chained" (linear, 1 byte/pixel) for graphics vs. one
 * glyph bitmap per character code in "plane 2" for text. Any drawing in
 * graphics mode clobbers whatever was in plane 2, so switching back to
 * text mode with the *registers* alone reproduces a real 80x25 grid,
 * but every cell renders garbage - there's no glyph data left to read.
 * Saving a copy of the BIOS-loaded font (present at boot, before this
 * kernel ever touches graphics mode) and writing it back on the way out
 * sidesteps needing to author/embed a whole replacement font ourselves:
 * this just restores the exact font the console already relies on. */
#define FONT_GLYPHS 256
#define FONT_ROWS 16
static uint8_t g_saved_font[FONT_GLYPHS * FONT_ROWS];
static int g_font_saved;

/* Routes the 0xA0000 window to plane 2 only, sequential (non-chained,
 * non-odd/even) addressing - the standard VGA "font access" register
 * configuration, distinct from both the mode 0x13 and text-mode tables
 * above. Whichever of vga_set_graphics_mode/vga_set_text_mode runs next
 * fully reprograms these same registers to its own real values anyway,
 * so this only matters for the font-copy loop that follows it. */
static void vga_select_font_plane(void) {
    /* The standard "read/write a VGA font plane directly" register
     * configuration (used well beyond this project - the same values
     * show up in most bare-metal font-dump code): Sequencer index 4 must
     * have chain-4 (bit 3) OFF and odd/even (bit 2) OFF - 0x06, not
     * mode 0x13's 0x0E (chain-4 on) or text mode's 0x02 (odd/even on) -
     * so bytes at 0xA0000 map 1:1 onto plane 2's own address space
     * instead of either linear framebuffer pixels or interleaved
     * char/attribute bytes. Graphics Controller index 6 then selects the
     * 0xA0000-0xBFFFF window with alphanumeric addressing still off
     * (0x00). Beyond these register values, the CRTC also has to already
     * be in a text-mode-like state for a plane-2 read/write here to land
     * correctly at all (QEMU's VGA emulation appears to key the planar
     * addressing path off overall CRTC state, not the Sequencer/Graphics
     * Controller registers alone) - see the call-site comment in
     * vga_set_text_mode below. */
    outb(VGA_SEQ_INDEX, 0x02); outb(VGA_SEQ_DATA, 0x04); /* map mask: plane 2 */
    outb(VGA_SEQ_INDEX, 0x04); outb(VGA_SEQ_DATA, 0x06); /* sequential, extended memory */
    outb(VGA_GC_INDEX, 0x04); outb(VGA_GC_DATA, 0x02);   /* read map select: plane 2 */
    outb(VGA_GC_INDEX, 0x05); outb(VGA_GC_DATA, 0x00);   /* write mode 0, read mode 0 */
    outb(VGA_GC_INDEX, 0x06); outb(VGA_GC_DATA, 0x00);   /* map 0xA0000-0xBFFFF, alphanumeric addressing off */
}

static void vga_save_text_font(void) {
    if (g_font_saved) {
        return;
    }
    vga_select_font_plane();
    volatile uint8_t *mem = (volatile uint8_t *)0xA0000;
    for (int g = 0; g < FONT_GLYPHS; g++) {
        for (int row = 0; row < FONT_ROWS; row++) {
            g_saved_font[g * FONT_ROWS + row] = mem[g * 32 + row];
        }
    }
    g_font_saved = 1;
}

static void vga_restore_text_font(void) {
    if (!g_font_saved) {
        return;
    }
    vga_select_font_plane();
    volatile uint8_t *mem = (volatile uint8_t *)0xA0000;
    for (int g = 0; g < FONT_GLYPHS; g++) {
        for (int row = 0; row < FONT_ROWS; row++) {
            mem[g * 32 + row] = g_saved_font[g * FONT_ROWS + row];
        }
    }
}

void vga_set_graphics_mode(void) {
    vga_save_text_font();
    vga_load_registers(0x63, MODE13_SEQ, MODE13_CRTC, MODE13_GC, MODE13_AC);
}

void vga_set_text_mode(void) {
    /* Load the text-mode CRTC/Attribute Controller state *before*
     * touching the font plane, not after: vga_debug_dump_font showed
     * that plane-2 reads/writes at 0xA0000 only land correctly when the
     * CRTC is already in a text-mode-like state (matching how
     * vga_save_text_font in vga_set_graphics_mode runs *before* the
     * mode 0x13 registers are loaded, i.e. the same ordering) - QEMU's
     * VGA emulation appears to key the planar/non-chain4 addressing path
     * off overall CRTC state, not the Sequencer/Graphics Controller
     * registers alone. */
    vga_load_registers(0x67, TEXT_SEQ, TEXT_CRTC, TEXT_GC, TEXT_AC);
    vga_restore_text_font();
}

void vga_set_palette_color(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
    outb(VGA_DAC_WRITE_INDEX, index);
    outb(VGA_DAC_DATA, (uint8_t)(r & 0x3F));
    outb(VGA_DAC_DATA, (uint8_t)(g & 0x3F));
    outb(VGA_DAC_DATA, (uint8_t)(b & 0x3F));
}

void vga_clear(uint8_t color) {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        FRAMEBUFFER[i] = color;
    }
}

void vga_put_pixel(int x, int y, uint8_t color) {
    if (x < 0 || x >= VGA_WIDTH || y < 0 || y >= VGA_HEIGHT) {
        return;
    }
    FRAMEBUFFER[y * VGA_WIDTH + x] = color;
}

void vga_fill_rect(int x, int y, int w, int h, uint8_t color) {
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            vga_put_pixel(x + col, y + row, color);
        }
    }
}

void vga_draw_rect(int x, int y, int w, int h, uint8_t color) {
    vga_fill_rect(x, y, w, 1, color);
    vga_fill_rect(x, y + h - 1, w, 1, color);
    vga_fill_rect(x, y, 1, h, color);
    vga_fill_rect(x + w - 1, y, 1, h, color);
}

/* Integer sqrt via Newton's method - the circle/ellipse fillers below are
 * the only callers, and their radii are always small (sprite/HP-bar
 * scale), so a few iterations from a rough starting guess is plenty. */
static int isqrt_approx(int n) {
    if (n <= 0) {
        return 0;
    }
    int x = n;
    int y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return x;
}

void vga_fill_circle(int cx, int cy, int r, uint8_t color) {
    for (int dy = -r; dy <= r; dy++) {
        int dx = isqrt_approx(r * r - dy * dy);
        vga_fill_rect(cx - dx, cy + dy, dx * 2 + 1, 1, color);
    }
}

void vga_fill_ellipse(int cx, int cy, int rx, int ry, uint8_t color) {
    if (ry <= 0) {
        return;
    }
    for (int dy = -ry; dy <= ry; dy++) {
        /* dx/rx = sqrt(1 - (dy/ry)^2), rearranged to stay in integers. */
        int dx = isqrt_approx((rx * rx) * (ry * ry - dy * dy) / (ry * ry));
        vga_fill_rect(cx - dx, cy + dy, dx * 2 + 1, 1, color);
    }
}

/* Edge function (twice the signed triangle area) - used both to get a
 * consistent winding-independent inside/outside test and, via its sign,
 * to handle triangles specified in either winding order. */
static int edge_fn(int ax, int ay, int bx, int by, int px, int py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

void vga_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint8_t color) {
    int min_x = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    int max_x = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int min_y = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int max_y = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);

    for (int py = min_y; py <= max_y; py++) {
        for (int px = min_x; px <= max_x; px++) {
            int w0 = edge_fn(x1, y1, x2, y2, px, py);
            int w1 = edge_fn(x2, y2, x0, y0, px, py);
            int w2 = edge_fn(x0, y0, x1, y1, px, py);
            if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0)) {
                vga_put_pixel(px, py, color);
            }
        }
    }
}

void vga_draw_char(int x, int y, char c, uint8_t color) {
    const uint8_t *glyph = font_glyph(c);
    for (int row = 0; row < FONT_H; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_W; col++) {
            if (bits & (1 << (FONT_W - 1 - col))) {
                vga_put_pixel(x + col, y + row, color);
            }
        }
    }
}

void vga_draw_text(int x, int y, const char *s, uint8_t color) {
    int cx = x;
    while (*s) {
        if (*s == '\n') {
            cx = x;
            y += FONT_H + 1;
        } else {
            vga_draw_char(cx, y, *s, color);
            cx += FONT_W + 1;
        }
        s++;
    }
}

int vga_text_width(const char *s) {
    int n = 0;
    int max = 0;
    while (*s) {
        if (*s == '\n') {
            if (n > max) {
                max = n;
            }
            n = 0;
        } else {
            n++;
        }
        s++;
    }
    if (n > max) {
        max = n;
    }
    return max * (FONT_W + 1);
}