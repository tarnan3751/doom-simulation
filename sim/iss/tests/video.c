#include "t.h"
int main(void)
{
    volatile uint8_t *fb = (volatile uint8_t *)(uintptr_t)VID_FB_BASE;
    volatile uint16_t *fb16 = (volatile uint16_t *)(uintptr_t)VID_FB_BASE;
    volatile uint32_t *pal = (volatile uint32_t *)(uintptr_t)VID_PALETTE_BASE;
    pal[0] = 0x00112233;
    pal[1] = 0x00FF0000;
    pal[255] = 0xFFEEDDCC;                       /* top byte must not be kept */
    ((volatile uint8_t *)&pal[2])[1] = 0xAB;     /* byte lane 1 only */
    fb[0] = 1; fb[1] = 2; fb[2] = 3; fb[3] = 4;  /* byte lanes */
    fb16[2] = 0x0605;                            /* halfword at 4..5 */
    fb16[3] = 0x0807;                            /* halfword at 6..7 (upper half of word) */
    R32(VID_FB_BASE + 8) = 0x0C0B0A09;           /* word */
    fb[63999] = 0x77;                            /* last pixel */
    fb[64000] = 0x55;                            /* outside the 64000 used bytes: ignored */
    CHECK(1, R32(VID_FLIP) == 0);
    R32(VID_FLIP) = 1;
    CHECK(2, R32(VID_FLIP) == 1);
    fb[0] = 9;
    R32(VID_FLIP) = 0xFFFFFFFF;
    R32(VID_FLIP) = 0;
    CHECK(3, R32(VID_FLIP) == 3);
    CHECK(4, (R32(VID_STATUS) & 1) == 1);
    return 0;
}
