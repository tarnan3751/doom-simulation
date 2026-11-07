#include "t.h"
#include "bench_kernel.h"
static uint32_t buf[1024];
int main(void)
{
    uint32_t n = 0;
    for (const char *p = BOOTINFO->cmdline; *p >= '0' && *p <= '9'; p++) n = n * 10 + (uint32_t)(*p - '0');
    if (!n) n = 100000;
    return (int)bench_kernel(n, buf);
}
