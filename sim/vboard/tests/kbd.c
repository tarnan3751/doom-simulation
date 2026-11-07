// Echo every keyboard event as "+HH" (press) / "-HH" (release) with the Doom key code in hex.
// Ends (status 0) after the release of 'q' (0x71).
#include "t.h"
int main(void) {
    for (;;) {
        uint32_t e = R32(KBD_DATA);
        if (!(e & KBD_DATA_VALID)) continue;
        putc_((e & KBD_DATA_PRESSED) ? '+' : '-');
        puthex(e & 0xFF, 2);
        putc_('\n');
        if (!(e & KBD_DATA_PRESSED) && (e & 0xFF) == 'q') return 0;
    }
}
