/*
 * mem.c - memcpy, memmove, memset, memcmp, memchr.
 *
 * RV32 has no misaligned loads/stores (they trap), so the word paths only
 * ever touch aligned words.  memcpy/memmove align the destination and, when
 * the source is at a different offset, merge two aligned source words with
 * shifts (little-endian).  The library is built with -fno-builtin and
 * -fno-tree-loop-distribute-patterns so GCC cannot turn these loops back
 * into calls to the functions being defined.
 */
#include <string.h>
#include "libc_impl.h"

#define WMASK (LIBC_WORD - 1)

/* Forward copy; also correct for overlapping buffers with dst < src
 * (memmove relies on this). */
static inline __attribute__((always_inline)) void copy_fwd(unsigned char *d, const unsigned char *s, size_t n)
{
    if (n >= 2 * LIBC_WORD) {
        size_t head = (size_t)(-(uintptr_t)d) & WMASK;
        n -= head;
        while (head--)
            *d++ = *s++;

        libc_word *dw = (libc_word *)(void *)d;
        unsigned off = (unsigned)((uintptr_t)s & WMASK);
        if (off == 0) {
            const libc_word *sw = (const libc_word *)(const void *)s;
            while (n >= 8 * LIBC_WORD) {
                libc_word a = sw[0], b = sw[1], c = sw[2], e = sw[3];
                libc_word f = sw[4], g = sw[5], h = sw[6], i = sw[7];
                dw[0] = a; dw[1] = b; dw[2] = c; dw[3] = e;
                dw[4] = f; dw[5] = g; dw[6] = h; dw[7] = i;
                sw += 8; dw += 8; n -= 8 * LIBC_WORD;
            }
            while (n >= LIBC_WORD) {
                *dw++ = *sw++;
                n -= LIBC_WORD;
            }
            s = (const unsigned char *)sw;
        } else {
            /* Source misaligned by `off`: every output word is assembled from
             * two neighbouring aligned source words.  The last word loaded
             * always contains a byte that is copied, so nothing outside the
             * source buffer's words is read. */
            unsigned rs = 8 * off, ls = 32 - rs;
            const libc_word *sw = (const libc_word *)(const void *)(s - off);
            libc_word w0 = *sw++;
            while (n >= 4 * LIBC_WORD) {
                libc_word w1 = sw[0], w2 = sw[1], w3 = sw[2], w4 = sw[3];
                dw[0] = (w0 >> rs) | (w1 << ls);
                dw[1] = (w1 >> rs) | (w2 << ls);
                dw[2] = (w2 >> rs) | (w3 << ls);
                dw[3] = (w3 >> rs) | (w4 << ls);
                w0 = w4; sw += 4; dw += 4; n -= 4 * LIBC_WORD;
            }
            while (n >= LIBC_WORD) {
                libc_word w1 = *sw++;
                *dw++ = (w0 >> rs) | (w1 << ls);
                w0 = w1;
                n -= LIBC_WORD;
            }
            s = (const unsigned char *)sw - LIBC_WORD + off;
        }
        d = (unsigned char *)dw;
    }
    while (n--)
        *d++ = *s++;
}

/* Backward copy (dst > src, overlapping). */
static inline void copy_bwd(unsigned char *d, const unsigned char *s, size_t n)
{
    d += n;
    s += n;
    if (n >= 2 * LIBC_WORD && (((uintptr_t)d ^ (uintptr_t)s) & WMASK) == 0) {
        size_t tail = (uintptr_t)d & WMASK;
        n -= tail;
        while (tail--)
            *--d = *--s;
        libc_word *dw = (libc_word *)(void *)d;
        const libc_word *sw = (const libc_word *)(const void *)s;
        while (n >= 4 * LIBC_WORD) {
            libc_word a = sw[-1], b = sw[-2], c = sw[-3], e = sw[-4];
            dw[-1] = a; dw[-2] = b; dw[-3] = c; dw[-4] = e;
            sw -= 4; dw -= 4; n -= 4 * LIBC_WORD;
        }
        while (n >= LIBC_WORD) {
            *--dw = *--sw;
            n -= LIBC_WORD;
        }
        d = (unsigned char *)dw;
        s = (const unsigned char *)sw;
    }
    while (n--)
        *--d = *--s;
}

void *memcpy(void *restrict dst, const void *restrict src, size_t n)
{
    copy_fwd(dst, src, n);
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d == s || n == 0)
        return dst;
    if ((uintptr_t)d - (uintptr_t)s >= n)   /* d < s, or no overlap */
        memcpy(d, s, n);                    /* ours copies forward: safe */
    else
        copy_bwd(d, s, n);
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    unsigned char b = (unsigned char)c;
    if (n >= 2 * LIBC_WORD) {
        size_t head = (size_t)(-(uintptr_t)d) & WMASK;
        n -= head;
        while (head--)
            *d++ = b;
        libc_word w = (libc_word)b * LIBC_ONES;
        libc_word *dw = (libc_word *)(void *)d;
        while (n >= 8 * LIBC_WORD) {
            dw[0] = w; dw[1] = w; dw[2] = w; dw[3] = w;
            dw[4] = w; dw[5] = w; dw[6] = w; dw[7] = w;
            dw += 8; n -= 8 * LIBC_WORD;
        }
        while (n >= LIBC_WORD) {
            *dw++ = w;
            n -= LIBC_WORD;
        }
        d = (unsigned char *)dw;
    }
    while (n--)
        *d++ = b;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    if (n >= 2 * LIBC_WORD && (((uintptr_t)p ^ (uintptr_t)q) & WMASK) == 0) {
        while ((uintptr_t)p & WMASK) {
            if (*p != *q)
                return *p - *q;
            p++; q++; n--;
        }
        const libc_word *pw = (const libc_word *)(const void *)p;
        const libc_word *qw = (const libc_word *)(const void *)q;
        while (n >= LIBC_WORD && *pw == *qw) {
            pw++; qw++; n -= LIBC_WORD;
        }
        p = (const unsigned char *)pw;
        q = (const unsigned char *)qw;
    }
    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p - *q;
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    unsigned char b = (unsigned char)c;
    while (n && ((uintptr_t)p & WMASK)) {
        if (*p == b)
            return (void *)p;
        p++; n--;
    }
    if (n >= LIBC_WORD) {
        libc_word pat = (libc_word)b * LIBC_ONES;
        const libc_word *w = (const libc_word *)(const void *)p;
        while (n >= LIBC_WORD) {
            libc_word x = *w ^ pat;
            if (LIBC_HASZERO(x))
                break;
            w++; n -= LIBC_WORD;
        }
        p = (const unsigned char *)w;
    }
    for (; n; n--, p++)
        if (*p == b)
            return (void *)p;
    return NULL;
}
