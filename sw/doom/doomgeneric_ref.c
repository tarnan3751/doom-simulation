/*
 * doomgeneric_ref.c - headless native reference platform for doomgeneric.
 *
 * Produces the reference frames for the Doom-on-SoC verification.  It is
 * built from the same patched source tree as the SoC platform
 * (doomgeneric_soc.c); the only differences are this file and the compiler.
 *
 * Extra options (removed from argv before Doom sees it):
 *   --dump-frames FILE   write every presented frame to FILE in DFRM format
 *                        (docs/ARCHITECTURE.md section 13)
 *   --max-frames N       stop (exit status 3) after N presented frames
 *
 * The clock is virtual: it only advances when Doom sleeps and by one tic
 * (1/35 s) per presented frame, so nothing depends on wall time.
 * No input is ever generated.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomgeneric.h"
#include "i_video.h"      /* palette_changed, colors[] (CMAP256) */

#define DFRM_W 320
#define DFRM_H 200

static FILE    *dump;
static uint32_t frames_presented;
static uint32_t max_frames;          /* 0 = unlimited */
static uint64_t vclock_us;           /* virtual clock, microseconds */

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void dump_close(void)
{
    if (dump) {
        fclose(dump);
        dump = NULL;
    }
}

void DG_Init(void)
{
    /* start away from 0: i_timer.c treats basetime == 0 as "unset" */
    vclock_us = 1000000;
}

void DG_DrawFrame(void)
{
    if (dump) {
        uint8_t pal[768];
        int i;

        for (i = 0; i < 256; i++) {
            pal[3 * i + 0] = (uint8_t)colors[i].r;
            pal[3 * i + 1] = (uint8_t)colors[i].g;
            pal[3 * i + 2] = (uint8_t)colors[i].b;
        }
        if (fwrite(pal, 1, sizeof pal, dump) != sizeof pal ||
            fwrite(DG_ScreenBuffer, 1, DFRM_W * DFRM_H, dump) != DFRM_W * DFRM_H) {
            fprintf(stderr, "doom-ref: write error on frame dump\n");
            exit(2);
        }
    }
    palette_changed = false;
    frames_presented++;
    vclock_us += 1000000u / 35u;
    if (max_frames && frames_presented >= max_frames) {
        fprintf(stderr, "doom-ref: --max-frames reached (%u)\n", frames_presented);
        exit(3);
    }
}

void DG_SleepMs(uint32_t ms)
{
    vclock_us += (uint64_t)ms * 1000u;
}

uint32_t DG_GetTicksMs(void)
{
    return (uint32_t)(vclock_us / 1000u);
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    (void)pressed; (void)key;
    return 0;
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;
}

int main(int argc, char **argv)
{
    const char *dump_path = NULL;
    int i, o = 0;

    /* strip our own options */
    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--dump-frames") && i + 1 < argc) {
            dump_path = argv[++i];
        } else if (!strcmp(argv[i], "--max-frames") && i + 1 < argc) {
            max_frames = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else {
            argv[o++] = argv[i];
        }
    }
    argc = o;
    argv[argc] = NULL;

    if (dump_path) {
        uint8_t hdr[16] = { 'D', 'F', 'R', 'M' };

        dump = fopen(dump_path, "wb");
        if (!dump) {
            perror(dump_path);
            return 2;
        }
        put_le32(hdr + 4, 1);
        put_le32(hdr + 8, DFRM_W);
        put_le32(hdr + 12, DFRM_H);
        fwrite(hdr, 1, sizeof hdr, dump);
        atexit(dump_close);
    }

    doomgeneric_Create(argc, argv);
    for (;;)
        doomgeneric_Tick();
    return 0;
}
