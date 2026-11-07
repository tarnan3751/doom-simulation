// Animated pattern forever (for windowed smoke tests and throughput measurements).
#include "t.h"
int main(void) {
    for (int i = 0; i < 256; i++) VID_PALETTE[i] = ((uint32_t)i << 16) | ((uint32_t)(255 - i) << 8) | 0x40u;
    for (uint32_t f = 0;; f++) {
        for (int y = 0; y < VID_HEIGHT; y++)
            for (int x = 0; x < VID_WIDTH; x++) VID_FB[y * VID_WIDTH + x] = (uint8_t)(x + y + f);
        mmio_write(VID_FLIP, 1);
    }
}
