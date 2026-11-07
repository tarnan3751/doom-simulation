/* Mixed ALU / load-store / branch / mul / div kernel, shared by the target
   benchmark and the host reference (so the ISS result can be cross-checked). */
#pragma once
#include <stdint.h>
static inline uint32_t bench_kernel(uint32_t iters, uint32_t *buf /* 1024 words, zeroed */)
{
    uint32_t s = 12345u, acc = 0;
    for (uint32_t i = 0; i < iters; i++) {
        s = s * 1664525u + 1013904223u;
        uint32_t idx = (s >> 12) & 1023u;
        uint32_t v = buf[idx];
        v ^= s;
        v = (v << 3) | (v >> 29);
        buf[idx] = v + i;
        if (s & 1u) acc += v; else acc ^= v;
        if ((s & 6u) == 4u) acc -= i;
        acc += (uint32_t)((int32_t)v / (int32_t)((s >> 20) | 1u));
        acc += v % ((s >> 8) | 1u);
        acc += (uint32_t)(((int64_t)(int32_t)v * (int32_t)s) >> 32);
        acc += (uint32_t)(((uint64_t)v * s) >> 32);
        acc += ((volatile uint8_t *)buf)[(s >> 3) & 4095u];
        acc += (uint32_t)(int32_t)((volatile int16_t *)buf)[(s >> 5) & 2047u];
        ((volatile uint8_t *)buf)[(s >> 7) & 4095u] = (uint8_t)acc;
        acc ^= acc >> 7;
    }
    return acc;
}
