#include "t.h"
/* cmdline[0]: 'l' load unmapped, 's' store to unmapped MMIO block, 'f' jump out of RAM,
   'r' load above 32 MB RAM, 'n' fetch at null (0x0) */
int main(void)
{
    volatile uint32_t sink;
    switch (BOOTINFO->cmdline[0]) {
    case 'l': sink = R32(0x20000000u); break;
    case 's': R32(0x10004000u) = 1; break;
    case 'r': sink = R32(0x82000000u); break;
    case 'f': ((void (*)(void))(uintptr_t)0x10000000u)(); break;
    case 'n': ((void (*)(void))(uintptr_t)0u)(); break;
    }
    (void)sink;
    return 0;
}
