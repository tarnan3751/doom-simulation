/*
 * fixed_test.c - host test of the SoC build's FixedDiv (patches-soc/0003)
 * against upstream's expression.
 *
 * The SoC m_fixed.c is compiled with FixedDiv renamed to soc_FixedDiv (see
 * the Makefile's `test` target) and with the same Doom flags as both builds
 * (-fwrapv ...).  Every result must be bit-identical to the reference
 * for edge values, all pairs of them, the overflow-test boundary, both sides
 * of the divisor ranges the SoC code distinguishes, and random inputs.
 * Usage: fixed_test [random pairs, default 200000000]
 */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef int fixed_t;
fixed_t soc_FixedDiv(fixed_t a, fixed_t b);

static fixed_t ref_div(fixed_t a, fixed_t b)
{
    if ((abs(a) >> 14) >= abs(b))
        return (a ^ b) < 0 ? INT_MIN : INT_MAX;
    return (fixed_t) (((int64_t) a << 16) / b);
}

static unsigned long long checks, fails;

static void check(fixed_t a, fixed_t b)
{
    fixed_t r = ref_div(a, b), s = soc_FixedDiv(a, b);

    checks++;
    if (r != s && fails++ < 20)
        printf("FixedDiv(%d, %d): soc %d, reference %d\n", a, b, s, r);
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t) (rng >> 16);
}

/* a random value with a random number of significant bits and sign */
static fixed_t rnd_mag(void)
{
    uint32_t v = rnd() >> (rnd() & 31);

    return (rnd() & 1) ? -(fixed_t) v : (fixed_t) v;
}

int main(int argc, char **argv)
{
    unsigned long long n = argc > 1 ? strtoull(argv[1], NULL, 0) : 200000000ull, i;
    static fixed_t edge[512];
    int ne = 0, x, y, k;

    edge[ne++] = 0;
    edge[ne++] = INT_MIN;
    edge[ne++] = INT_MAX;
    edge[ne++] = INT_MIN + 1;
    for (k = 0; k < 31; k++) {
        fixed_t p = (fixed_t) (1u << k);

        edge[ne++] = p; edge[ne++] = -p;
        edge[ne++] = p + 1; edge[ne++] = -(p + 1);
        edge[ne++] = p - 1; edge[ne++] = -(p - 1);
        edge[ne++] = (fixed_t) (3u << k >> 1); edge[ne++] = -(fixed_t) (3u << k >> 1);
    }
    edge[ne++] = 0xffff; edge[ne++] = 0x10001; edge[ne++] = -0xffff; edge[ne++] = -0x10001;

    for (x = 0; x < ne; x++)                       /* all pairs of edge values */
        for (y = 0; y < ne; y++)
            check(edge[x], edge[y]);

    for (i = 0; i < 2000000; i++) {                /* the overflow-test boundary */
        fixed_t b = rnd_mag(), a;
        int d = (int) (rnd() & 0x3fff) - 0x2000;

        if (b == INT_MIN)
            continue;
        a = (fixed_t) ((uint32_t) abs(b) << 14) + d;
        check(a, b);
        check(-a, b);
    }
    for (i = 0; i < 4000000; i++) {                /* divisors around 2^16 */
        fixed_t b = 0x10000 + (int) (rnd() & 0xff) - 0x80;

        check(rnd_mag(), (rnd() & 1) ? b : -b);
    }
    for (i = 0; i < n; i++) {                      /* random magnitudes, then uniform */
        check(rnd_mag(), rnd_mag());
        if ((i & 3) == 0)
            check((fixed_t) rnd(), (fixed_t) rnd());
    }

    printf("fixed_test: %llu checks, %llu failures -> %s\n", checks, fails, fails ? "FAILED" : "PASSED");
    return fails != 0;
}
