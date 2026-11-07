#include "t.h"
int main(void)
{
    CHECK(1, R32(UART_STATUS) == 1);
    CHECK(2, R32(SYS_CLK_HZ) == 50000000u);
    CHECK(3, R32(SYS_FLAGS) == 1);
    CHECK(4, R32(KBD_STATUS) == 0);
    CHECK(5, R32(KBD_DATA) == 0);
    CHECK(6, R32(KBD_STATUS) == 0);
    CHECK(7, (R32(VID_STATUS) & 1) == 1);
    CHECK(8, R32(VID_FLIP) == 0);
    /* write-only registers read 0 */
    CHECK(9, R32(UART_TX) == 0);
    CHECK(10, R32(SYS_EXIT) == 0);
    /* unmapped registers inside mapped blocks: read 0, writes ignored */
    static const uint32_t unm[] = { UART_BASE + 8, UART_BASE + 0xFFC, SYS_BASE + 0xC, SYS_BASE + 0xFFC,
                                    KBD_BASE + 8, KBD_BASE + 0xFFC, VID_BASE + 8, VID_BASE + 0x3FC,
                                    VID_BASE + 0x800, VID_BASE + 0xFFC, VID_FB_BASE + 64000, VID_FB_BASE + 0xFFFC };
    for (unsigned i = 0; i < sizeof unm / sizeof unm[0]; i++) {
        R32(unm[i]) = 0xDEADBEEF;
        CHECK(20 + i, R32(unm[i]) == 0);
    }
    CHECK(40, R32(VID_FLIP) == 0);   /* none of the above flipped */
    CHECK(41, tr_count == 0);
    /* sub-word reads of MMIO registers */
    CHECK(42, *(volatile uint8_t *)(uintptr_t)UART_STATUS == 1);
    CHECK(43, *(volatile uint8_t *)(uintptr_t)(UART_STATUS + 1) == 0);
    CHECK(44, *(volatile uint16_t *)(uintptr_t)(SYS_CLK_HZ + 2) == (50000000u >> 16));
    return 0;
}
