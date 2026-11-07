/*
 * doomgeneric_soc.c - doomgeneric platform layer for the SoC (sw/doom/README.md, "Platform layers").
 * The WAD is opened with plain fopen() by the Doom code; the C library exposes it as a read-only file.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "soc.h"
#include "doomgeneric.h"
#include "i_video.h"      /* I_VideoBuffer, palette_changed, colors[] (CMAP256) */

#define FB_WORDS (VID_FB_BYTES / 4)       /* 16 000 */

/* ---- time: milliseconds from the low 32 bits of the cycle counter (exact modulo 2^32, i.e.
 * for polls less than 85 s apart at 50 MHz), one 32-bit divide per call with the remainder
 * carried; it starts at 1 ms because i_timer.c treats basetime == 0 as "unset" ---- */
static uint32_t clk_last;        /* cycle counter at the previous call        */
static uint32_t clk_rem;         /* cycles not yet converted into a full ms   */
static uint32_t clk_ms = 1;      /* milliseconds since DG_Init (+1)           */
static uint32_t clk_per_ms = 50000;

static inline uint32_t rdcycle32(void)
{
    uint32_t v;
    __asm__ volatile("rdcycle %0" : "=r"(v));
    return v;
}

static void clock_init(void)
{
    uint32_t hz = mmio_read(SYS_CLK_HZ);

    clk_per_ms = hz >= 1000 ? hz / 1000 : 1;
    clk_last = rdcycle32();
    clk_rem = 0;
    clk_ms = 1;
}

uint32_t DG_GetTicksMs(void)
{
    uint32_t now = rdcycle32();
    uint32_t d = now - clk_last;          /* modulo 2^32 */
    uint32_t q = d / clk_per_ms;
    uint32_t r = d - q * clk_per_ms;

    clk_last = now;
    clk_rem += r;
    if (clk_rem >= clk_per_ms) {
        clk_rem -= clk_per_ms;
        q++;
    }
    clk_ms += q;
    return clk_ms;
}

void DG_SleepMs(uint32_t ms)
{
    uint32_t t0 = DG_GetTicksMs();

    while (DG_GetTicksMs() - t0 < ms)
        ;
}

/* ---- video ---- */

void DG_Init(void)
{
    clock_init();
    /* the first presented frame must always load the palette: the palette RAM
       is not reset */
    palette_changed = true;
}

void DG_DrawFrame(void)
{
    /* I_VideoBuffer comes from Z_Malloc: word aligned (MEM_ALIGN) */
    const uint32_t *src = (const uint32_t *)(const void *)I_VideoBuffer;
    const uint32_t *end = src + FB_WORDS;
    volatile uint32_t *dst = VID_FB32;

    if (palette_changed) {
        volatile uint32_t *pal = VID_PALETTE;
        int i;

        for (i = 0; i < 256; i++)
            pal[i] = ((uint32_t)colors[i].r << 16) |
                     ((uint32_t)colors[i].g << 8) |
                      (uint32_t)colors[i].b;
        palette_changed = false;
    }

    /* two cache lines per iteration: 16 loads then 16 stores, 1 000 iterations */
    do {
        uint32_t a0 = src[0], a1 = src[1], a2 = src[2], a3 = src[3];
        uint32_t a4 = src[4], a5 = src[5], a6 = src[6], a7 = src[7];
        uint32_t b0 = src[8], b1 = src[9], b2 = src[10], b3 = src[11];
        uint32_t b4 = src[12], b5 = src[13], b6 = src[14], b7 = src[15];

        dst[0] = a0; dst[1] = a1; dst[2] = a2; dst[3] = a3;
        dst[4] = a4; dst[5] = a5; dst[6] = a6; dst[7] = a7;
        dst[8] = b0; dst[9] = b1; dst[10] = b2; dst[11] = b3;
        dst[12] = b4; dst[13] = b5; dst[14] = b6; dst[15] = b7;
        src += 16;
        dst += 16;
    } while (src != end);

    mmio_write(VID_FLIP, 1);
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;
}

/* ---- keyboard ---- */

int DG_GetKey(int *pressed, unsigned char *key)
{
    uint32_t v = mmio_read(KBD_DATA);     /* pops one event, 0 when empty */

    if (!(v & KBD_DATA_VALID))
        return 0;
    *pressed = (v & KBD_DATA_PRESSED) != 0;
    *key = (unsigned char)(v & KBD_DATA_CODE_MASK);
    return 1;
}

/* ---- entry point ---- */

int main(int argc, char **argv)
{
    static char wad_name[sizeof BOOTINFO->wad_name + 1];
    char **av;
    int i, ac, have_iwad = 0;

    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-iwad"))
            have_iwad = 1;

    av = malloc((size_t)(argc + 3) * sizeof *av);
    if (!av)
        return 1;
    for (i = 0; i < argc; i++)
        av[i] = argv[i];
    ac = argc;

    /* No -iwad on the command line: use the WAD the board preloaded. */
    if (!have_iwad && BOOTINFO->magic == BOOTINFO_MAGIC &&
        BOOTINFO->wad_addr != 0 && BOOTINFO->wad_name[0] != '\0') {
        memcpy(wad_name, BOOTINFO->wad_name, sizeof BOOTINFO->wad_name);
        wad_name[sizeof BOOTINFO->wad_name] = '\0';
        av[ac++] = "-iwad";
        av[ac++] = wad_name;
    }
    av[ac] = NULL;

    doomgeneric_Create(ac, av);
    for (;;)
        doomgeneric_Tick();
    return 0;
}
