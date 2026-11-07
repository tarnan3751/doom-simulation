// Prints what the boot-info page says, plus the first bytes of the WAD.
#include "t.h"
int main(void) {
    const struct bootinfo *b = BOOTINFO;
    if (b->magic != BOOTINFO_MAGIC) { puts_("no bootinfo\n"); return 1; }
    puts_("wad_addr="); puthex(b->wad_addr, 8);
    puts_(" wad_size="); puthex(b->wad_size, 8);
    puts_("\nwad_name=["); puts_(b->wad_name);
    puts_("]\ncmdline=["); puts_(b->cmdline);
    puts_("]\n");
    if (b->wad_addr) {
        puts_("wad head=");
        const volatile char *w = (const volatile char *)(uintptr_t)b->wad_addr;
        for (int i = 0; i < 8; i++) putc_(w[i]);
        puts_(" tail=");
        for (uint32_t i = b->wad_size - 4; i < b->wad_size; i++) putc_(w[i]);
        putc_('\n');
    }
    return 0;
}
