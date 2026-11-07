// ALU + load/store + branch loop; ~6 instructions per iteration. Iterations from the command line
// (decimal, default 100000000/6 style: ITER). Exit status = checksum low byte.
#include "t.h"
#ifndef ITER
#define ITER 50000000u
#endif
static uint32_t buf[1024];
int main(void) {
    uint32_t x = 12345, sum = 0;
    for (uint32_t i = 0; i < ITER; i++) {
        x = x * 1664525u + 1013904223u;
        uint32_t k = (x >> 16) & 1023;
        buf[k] += x;
        sum ^= buf[k];
    }
    return (int)(sum & 0x7F);
}
