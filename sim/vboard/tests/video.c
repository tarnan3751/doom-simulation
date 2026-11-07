// Draws a known pattern and flips FRAMES times; frame k differs from frame k-1.
//   palette[0] = background (changes per frame), palette[1..7] = fixed colours.
//   Pixel (x,y) = 1 + ((x/40 + y/25) % 7) in the grid; a 20x20 box of colour 8 moves by 16 px per frame.
// Exits when FRAMES flips are done (status 0), after printing a line.
#include "t.h"
#ifndef FRAMES
#define FRAMES 5
#endif
#define PW 8
int main(void) {
    static const uint32_t cols[PW] = {0x000000, 0xE03030, 0x30E030, 0x3030E0, 0xE0E030, 0x30E0E0, 0xE030E0, 0xF0F0F0};
    for (int i = 1; i < PW; i++) VID_PALETTE[i] = cols[i];
    VID_PALETTE[8] = 0xFF8000;
    for (int f = 0; f < FRAMES; f++) {
        VID_PALETTE[0] = 0x101010u * (uint32_t)(f + 1);
        for (int y = 0; y < VID_HEIGHT; y++)
            for (int x = 0; x < VID_WIDTH; x++) {
                uint8_t p = (uint8_t)(1 + ((x / 40 + y / 25) % 7));
                if (x < 2 || y < 2 || x >= VID_WIDTH - 2 || y >= VID_HEIGHT - 2) p = 0;        // 2 px border in palette[0]
                int bx = 16 * f + 8, by = 8 + 20 * f;
                if (x >= bx && x < bx + 20 && y >= by && y < by + 20) p = 8;
                VID_FB[y * VID_WIDTH + x] = p;
            }
        mmio_write(VID_FLIP, 1);
    }
#ifdef HOLD
    { uint64_t t0 = rdcycle64(); while (rdcycle64() - t0 < HOLD) ; }   // keep the last frame static for a few scan-out frames
#endif
    puts_("video done\n");
    return 0;
}
