/*
 * testpattern.c - palette + 320x200 framebuffer test pattern.
 *
 * Usage: testpattern [frames [ms_per_frame]]
 *   frames        number of frames to present (default 8; 0 = run forever)
 *   ms_per_frame  optional busy-wait per frame, to slow the animation down
 *
 * What the picture tells you:
 *   - 1-pixel white border on the outermost rows/columns: a missing or doubled
 *     edge means an off-by-one in scan-out or in the copy.
 *   - corner squares TL red, TR green, BL blue, BR yellow, plus "TOP" at the
 *     top: orientation and mirroring.
 *   - colour bars W Y C G M R B K with letters below: palette byte order
 *     (R/B swapped shows red where blue belongs and vice versa).
 *   - a strip of all 256 palette indices, one pixel each (0 at the left),
 *     with ticks every 16 and labels 0 / 128 / 255: index off-by-one.
 *     Indices 0-15 are the named colours, 16-255 a continuous hue ramp.
 *   - "RGBC" above a run of 4-pixel groups red,green,blue,cyan: byte-lane
 *     order of 32-bit framebuffer stores.
 *   - 1-pixel checkerboard and alternating lines: pixel doubling / row step.
 *   - animation: a box bouncing along the bottom track, a frame counter, and
 *     palette entry 15 pulsing; every frame is presented with VID_FLIP.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <bsp.h>
#include "font5x7.h"

#define W VID_WIDTH
#define H VID_HEIGHT

enum {
    C_BLACK, C_WHITE, C_RED, C_GREEN, C_BLUE, C_CYAN, C_MAGENTA, C_YELLOW,
    C_DGRAY, C_GRAY, C_LGRAY, C_ORANGE, C_NAVY, C_DGREEN, C_BROWN, C_PULSE,
    C_RAMP0 = 16
};

static const uint32_t named[16] = {
    0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00, 0x0000FF, 0x00FFFF, 0xFF00FF, 0xFFFF00,
    0x404040, 0x808080, 0xC0C0C0, 0xFF8000, 0x000080, 0x008000, 0x804000, 0xFFFFFF,
};

static uint8_t fb[W * H] __attribute__((aligned(4)));   /* drawn in RAM, copied */
static uint32_t pal[256];

/* hue ramp for indices 16..255 (240 entries, full saturation) */
static uint32_t hue_rgb(int i, int n)
{
    int h = i * 6 * 256 / n;            /* 0 .. 6*256 */
    int seg = h >> 8, f = h & 255;
    int r, g, b;
    switch (seg) {
    case 0:  r = 255;     g = f;       b = 0;       break;   /* red -> yellow    */
    case 1:  r = 255 - f; g = 255;     b = 0;       break;   /* yellow -> green  */
    case 2:  r = 0;       g = 255;     b = f;       break;   /* green -> cyan    */
    case 3:  r = 0;       g = 255 - f; b = 255;     break;   /* cyan -> blue     */
    case 4:  r = f;       g = 0;       b = 255;     break;   /* blue -> magenta  */
    default: r = 255;     g = 0;       b = 255 - f; break;   /* magenta -> red   */
    }
    return (uint32_t)(r << 16 | g << 8 | b);
}

static void set_palette(void)
{
    for (int i = 0; i < 16; i++)
        pal[i] = named[i];
    for (int i = C_RAMP0; i < 256; i++)
        pal[i] = hue_rgb(i - C_RAMP0, 256 - C_RAMP0);
    for (int i = 0; i < 256; i++)
        VID_PALETTE[i] = pal[i];
}

static inline void px(int x, int y, uint8_t c)
{
    if ((unsigned)x < W && (unsigned)y < H)
        fb[y * W + x] = c;
}

static void rect(int x, int y, int w, int h, uint8_t c)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            px(i, j, c);
}

static const uint8_t *glyph(char ch)
{
    for (unsigned k = 0; k < sizeof font5x7 / sizeof font5x7[0]; k++)
        if (font5x7[k].ch == ch)
            return font5x7[k].rows;
    if (ch >= 'a' && ch <= 'z')
        return glyph((char)(ch - 32));
    return font5x7[0].rows;             /* unknown: blank */
}

/* text with a 6x8 cell per character, scaled by s */
static void text(int x, int y, const char *s, uint8_t c, int sc)
{
    for (; *s; s++, x += 6 * sc) {
        const uint8_t *g = glyph(*s);
        for (int r = 0; r < 7; r++)
            for (int col = 0; col < 5; col++)
                if (g[r] & (0x10 >> col))
                    rect(x + col * sc, y + r * sc, sc, sc, c);
    }
}

static int text_width(const char *s, int sc)
{
    return (int)strlen(s) * 6 * sc - sc;
}

#define TRACK_Y  172
#define TRACK_H  16
#define BOX_W    16

static void draw_static(void)
{
    memset(fb, C_BLACK, sizeof fb);

    /* border on the outermost pixels */
    rect(0, 0, W, 1, C_WHITE);
    rect(0, H - 1, W, 1, C_WHITE);
    rect(0, 0, 1, H, C_WHITE);
    rect(W - 1, 0, 1, H, C_WHITE);
    /* corner squares just inside the border */
    rect(2, 2, 8, 8, C_RED);
    rect(W - 10, 2, 8, 8, C_GREEN);
    rect(2, H - 10, 8, 8, C_BLUE);
    rect(W - 10, H - 10, 8, 8, C_YELLOW);

    const char *title = "TOP  SOC TEST PATTERN 320x200";
    text((W - text_width(title, 1)) / 2, 4, title, C_WHITE, 1);

    /* colour bars, SMPTE order, 8 x 38 px */
    static const uint8_t bars[8] = { C_WHITE, C_YELLOW, C_CYAN, C_GREEN, C_MAGENTA, C_RED, C_BLUE, C_BLACK };
    static const char *labels = "WYCGMRBK";
    for (int b = 0; b < 8; b++) {
        int x = 8 + 38 * b;
        rect(x, 16, 38, 56, bars[b]);
        char l[2] = { labels[b], 0 };
        text(x + 16, 75, l, b == 7 ? C_GRAY : C_WHITE, 1);
    }
    rect(8 + 38 * 7, 16, 38, 1, C_DGRAY);       /* outline the black bar */
    rect(8 + 38 * 7, 71, 38, 1, C_DGRAY);
    rect(8 + 38 * 8 - 1, 16, 1, 56, C_DGRAY);

    /* all 256 palette indices, one pixel each, x = 32 + index */
    for (int i = 0; i < 256; i++)
        rect(32 + i, 88, 1, 20, (uint8_t)i);
    for (int i = 0; i <= 256; i += 16) {
        int x = 32 + (i == 256 ? 255 : i);
        rect(x, 109, 1, (i % 128 == 0 || i == 256) ? 6 : 3, C_WHITE);
    }
    text(32 - 2, 117, "0", C_WHITE, 1);
    text(32 + 128 - 8, 117, "128", C_WHITE, 1);
    text(32 + 255 - 14, 117, "255", C_WHITE, 1);

    /* byte-lane order: groups of four pixels red, green, blue, cyan */
    text(16, 130, "RGBC", C_WHITE, 1);
    for (int x = 16; x < 112; x++)
        rect(x, 140, 1, 16, (uint8_t)(C_RED + (x & 3)));     /* x = 16 is a word boundary */
    /* 1-pixel checkerboard */
    text(128, 130, "CHECK", C_WHITE, 1);
    for (int y = 140; y < 156; y++)
        for (int x = 128; x < 192; x++)
            px(x, y, ((x ^ y) & 1) ? C_WHITE : C_BLACK);
    /* alternating rows and columns */
    text(208, 130, "ROWS COLS", C_WHITE, 1);
    for (int y = 140; y < 156; y++)
        for (int x = 208; x < 240; x++)
            px(x, y, (y & 1) ? C_WHITE : C_NAVY);
    for (int y = 140; y < 156; y++)
        for (int x = 248; x < 304; x++)
            px(x, y, (x & 1) ? C_WHITE : C_DGREEN);

    /* animation track */
    rect(4, TRACK_Y - 2, W - 8, 1, C_DGRAY);
    rect(4, TRACK_Y + TRACK_H + 1, W - 8, 1, C_DGRAY);
}

static void draw_frame(unsigned f)
{
    /* clear track, draw box */
    rect(4, TRACK_Y, W - 8, TRACK_H, C_BLACK);
    int span = W - 8 - BOX_W - 120;      /* box travels in x = 124 .. 124+span */
    int pos = (int)((f * 4) % (unsigned)(2 * span));
    if (pos > span)
        pos = 2 * span - pos;
    rect(124 + pos, TRACK_Y, BOX_W, TRACK_H, (uint8_t)(C_RAMP0 + (f * 7) % 240));
    rect(124 + pos + 4, TRACK_Y + 4, BOX_W - 8, TRACK_H - 8, C_PULSE);
    char buf[32];
    snprintf(buf, sizeof buf, "FRAME %06u", f);
    text(8, TRACK_Y + 4, buf, C_WHITE, 1);
}

static void present(void)
{
    /* wait until scan-out is outside the 400 active framebuffer lines
     * (always true in the FAST build), then copy with word stores */
    while (!(mmio_read(VID_STATUS) & VID_STATUS_VBLANK))
        ;
    const uint32_t *src = (const uint32_t *)(const void *)fb;
    volatile uint32_t *dst = VID_FB32;
    for (int i = 0; i < W * H / 4; i += 4) {
        uint32_t a = src[i], b = src[i + 1], c = src[i + 2], d = src[i + 3];
        dst[i] = a;
        dst[i + 1] = b;
        dst[i + 2] = c;
        dst[i + 3] = d;
    }
    mmio_write(VID_FLIP, 1);
}

int main(int argc, char **argv)
{
    unsigned frames = 8, delay_ms = 0;
    if (argc > 1)
        frames = (unsigned)strtoul(argv[1], NULL, 0);
    if (argc > 2)
        delay_ms = (unsigned)strtoul(argv[2], NULL, 0);
    printf("testpattern: %u frames%s, %u ms/frame extra delay\n", frames,
           frames ? "" : " (forever)", delay_ms);

    uint32_t flips0 = mmio_read(VID_FLIP);
    set_palette();
    draw_static();
    uint64_t t0 = bsp_cycles();
    unsigned f = 0;
    for (; frames == 0 || f < frames; f++) {
        draw_frame(f);
        /* palette entry 15 pulses between white and orange */
        unsigned k = (f * 16) % 512;
        unsigned v = k < 256 ? k : 511 - k;
        VID_PALETTE[C_PULSE] = 0xFF0000u | ((255 - v / 2) << 8) | (255 - v);
        present();
        if (delay_ms)
            bsp_delay_ms(delay_ms);
    }
    uint64_t dt = bsp_cycles() - t0;
    uint32_t flips = mmio_read(VID_FLIP) - flips0;
    uint32_t hz = bsp_clk_hz();
    printf("testpattern: presented %u frames in %" PRIu64 " cycles: %" PRIu64 " cycles/frame, %.2f frames/s at %" PRIu32 " Hz\n",
           f, dt, f ? dt / f : 0, dt ? (double)f * hz / (double)dt : 0.0, hz);
    printf("testpattern: VID_FLIP counter advanced by %" PRIu32 " (%s)\n", flips, flips == f ? "ok" : "MISMATCH");
    return flips == f ? 0 : 1;
}
