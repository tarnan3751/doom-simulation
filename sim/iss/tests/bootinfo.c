#include "t.h"
static int streq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
int main(void)
{
    const struct bootinfo *bi = BOOTINFO;
    CHECK(1, bi->magic == BOOTINFO_MAGIC);
    CHECK(2, bi->reserved == 0);
    if (bi->cmdline[0] == 'N') {                  /* no-WAD mode: argv = "N x" */
        CHECK(3, bi->wad_addr == 0 && bi->wad_size == 0 && bi->wad_name[0] == 0);
        CHECK(4, streq(bi->cmdline, "N x"));
        return 0;
    }
    CHECK(5, streq(bi->cmdline, "foo -bar baz"));
    CHECK(6, bi->wad_addr == WAD_ADDR);
    CHECK(7, bi->wad_size == 1001);
    CHECK(8, streq(bi->wad_name, "test.wad"));    /* directories stripped */
    const uint8_t *w = (const uint8_t *)(uintptr_t)WAD_ADDR;
    const char *pat = "ABCDEFGH\n";
    for (unsigned i = 0; i < 1001; i++) CHECK(9, w[i] == (uint8_t)pat[i % 9]);
    CHECK(10, w[1001] == 0);
    return 0;
}
