/*
 * fpconv.c - the exact decimal expansion of a double, for printf.
 *
 * Every double is a finite decimal: __libc_dtoa_exact() writes all of its
 * digits, and printf rounds them half-to-even at the requested digit, so the
 * output is correctly rounded like glibc's / macOS's.  A tiny bignum
 * (little-endian limbs in base 10^9) and integer arithmetic only: the target
 * has no FPU.
 */
#include <string.h>
#include "libc_impl.h"

#define BASE       1000000000u
#define BIG_LIMBS  144              /* 1296 digits: more than the 767 of the smallest subnormal */

typedef struct {
    int      n;                     /* limbs in use (0 = zero) */
    uint32_t d[BIG_LIMBS];
} big;

static void big_set_u64(big *b, uint64_t v)
{
    b->n = 0;
    while (v) {
        b->d[b->n++] = (uint32_t)(v % BASE);
        v /= BASE;
    }
}

static void big_mul_small(big *b, uint32_t f)
{
    uint64_t carry = 0;
    for (int i = 0; i < b->n; i++) {
        uint64_t t = (uint64_t)b->d[i] * f + carry;
        b->d[i] = (uint32_t)(t % BASE);
        carry = t / BASE;
    }
    while (carry && b->n < BIG_LIMBS) {
        b->d[b->n++] = (uint32_t)(carry % BASE);
        carry /= BASE;
    }
}

static void big_mul_pow2(big *b, int e)
{
    for (; e >= 29; e -= 29)
        big_mul_small(b, 1u << 29);
    if (e > 0)
        big_mul_small(b, 1u << e);
}

static void big_mul_pow5(big *b, int e)
{
    static const uint32_t p5[14] = { 1, 5, 25, 125, 625, 3125, 15625, 78125, 390625,
                                     1953125, 9765625, 48828125, 244140625, 1220703125 };
    for (; e >= 13; e -= 13)
        big_mul_small(b, p5[13]);
    if (e > 0)
        big_mul_small(b, p5[e]);
}

static inline uint64_t dbits(double x)
{
    uint64_t u;
    memcpy(&u, &x, sizeof u);
    return u;
}

#define FRAC_BITS   52
#define FRAC_MASK   ((1ULL << FRAC_BITS) - 1)
#define HIDDEN      (1ULL << FRAC_BITS)
#define EXP_BIAS    1075            /* value = mantissa * 2^(e - 1075) */

/* bits of a positive finite double -> mantissa m and exponent k, value m*2^k */
static inline void decompose(uint64_t bits, uint64_t *m, int *k)
{
    int be = (int)(bits >> FRAC_BITS);
    if (be == 0) {
        *m = bits & FRAC_MASK;
        *k = 1 - EXP_BIAS;
    } else {
        *m = (bits & FRAC_MASK) | HIDDEN;
        *k = be - EXP_BIAS;
    }
}

int __libc_dtoa_exact(double v, char *out, int *dexp)
{
    uint64_t m;
    int k;
    decompose(dbits(v) & ~(1ULL << 63), &m, &k);
    if (m == 0) {
        *dexp = 1;
        return 0;
    }
    while (!(m & 1)) {          /* fewer limbs to multiply */
        m >>= 1;
        k++;
    }
    big b;
    big_set_u64(&b, m);
    int frac = 0;
    if (k > 0) {
        big_mul_pow2(&b, k);
    } else if (k < 0) {
        big_mul_pow5(&b, -k);   /* m * 2^k = m * 5^-k / 10^-k */
        frac = -k;
    }

    /* limbs -> digits, most significant first */
    char *p = out;
    uint32_t top = b.d[b.n - 1];
    char tmp[10];
    int t = 0;
    while (top) {
        tmp[t++] = (char)('0' + top % 10);
        top /= 10;
    }
    while (t)
        *p++ = tmp[--t];
    for (int i = b.n - 2; i >= 0; i--) {
        uint32_t x = b.d[i];
        for (int j = 8; j >= 0; j--) {
            p[j] = (char)('0' + x % 10);
            x /= 10;
        }
        p += 9;
    }
    int n = (int)(p - out);
    *dexp = n - frac;
    while (n > 0 && out[n - 1] == '0')
        n--;
    return n;
}
