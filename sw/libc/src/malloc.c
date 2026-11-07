/*
 * malloc.c - boundary-tag allocator over one fixed heap region.
 *
 * Chunk layout (W = sizeof(size_t): 4 on RV32):
 *
 *     c + 0        head  = size | INUSE | PREV_INUSE   (size: multiple of 16)
 *     c + W        payload (16-byte aligned)  -- in a free chunk:
 *                    next, prev free-list links ...
 *     c + size - W   foot = size                    (free chunks only)
 *
 * An allocated chunk costs one word of overhead.  Its successor's
 * PREV_INUSE bit says whether the footer in front of it is valid, so free()
 * coalesces with both neighbours in O(1); two free chunks are never
 * adjacent.  The heap ends with a zero-size, permanently in-use sentinel
 * header.  Free chunks live in segregated, doubly linked lists: exact
 * classes of 16 bytes below 512 bytes, then two classes per power of two;
 * allocation takes the first fit from the smallest class that can hold the
 * request and splits off the remainder.
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <libc_port.h>
#include "libc_impl.h"

#define W          (sizeof(size_t))
#define ALIGNMENT  16u
#define INUSE      ((size_t)1)
#define PREV_INUSE ((size_t)2)
#define FLAGS      ((size_t)15)
#define MINCHUNK   ((4 * W + ALIGNMENT - 1) & ~(size_t)(ALIGNMENT - 1))
#define NBINS      80

typedef struct chunk {
    size_t        head;
    struct chunk *next;   /* free chunks only */
    struct chunk *prev;
} chunk;

static chunk  *bins[NBINS];
static char   *heap_lo;          /* first chunk                         */
static chunk  *heap_end;         /* sentinel header                      */
static int     heap_state;       /* 0 = not initialised, 1 = ok, -1 = none */
static size_t  stat_in_use, stat_peak, stat_nalloc;

#define CSIZE(c)        ((c)->head & ~FLAGS)
#define NEXT_CHUNK(c)   ((chunk *)((char *)(c) + CSIZE(c)))
#define FOOT(c, sz)     (*(size_t *)((char *)(c) + (sz) - W))
#define MEM2CHUNK(p)    ((chunk *)((char *)(p) - W))
#define CHUNK2MEM(c)    ((void *)((char *)(c) + W))

static void heap_init(void)
{
    void *start = NULL, *end = NULL;
    heap_state = -1;
    __libc_heap_region(&start, &end);
    uintptr_t lo = (uintptr_t)start, hi = (uintptr_t)end;
    if (!start || hi <= lo || hi - lo < 4 * MINCHUNK)
        return;
    uintptr_t first = ((lo + W + ALIGNMENT - 1) & ~(uintptr_t)(ALIGNMENT - 1)) - W;
    uintptr_t sent  = (hi & ~(uintptr_t)(ALIGNMENT - 1)) - W;
    if (sent <= first || sent - first < MINCHUNK)
        return;
    chunk *c = (chunk *)first;
    size_t sz = (size_t)(sent - first);
    heap_lo = (char *)first;
    heap_end = (chunk *)sent;
    heap_end->head = 0 | INUSE;              /* PREV_INUSE clear: c is free */
    c->head = sz | PREV_INUSE;
    FOOT(c, sz) = sz;
    heap_state = 1;
}

static inline int bin_index(size_t sz)
{
    if (sz < 512)
        return (int)(sz >> 4);                       /* 1..31 */
    int lg = (int)(sizeof(unsigned long) * 8 - 1) - __builtin_clzl((unsigned long)sz);
    int half = (int)((sz >> (lg - 1)) & 1);
    int idx = 32 + 2 * (lg - 9) + half;
    return idx < NBINS ? idx : NBINS - 1;
}

static inline void bin_insert(chunk *c)
{
    int i = bin_index(CSIZE(c));
    c->prev = NULL;
    c->next = bins[i];
    if (bins[i])
        bins[i]->prev = c;
    bins[i] = c;
}

static inline void bin_unlink(chunk *c)
{
    if (c->prev)
        c->prev->next = c->next;
    else
        bins[bin_index(CSIZE(c))] = c->next;
    if (c->next)
        c->next->prev = c->prev;
}

static inline int ensure_heap(void)
{
    if (heap_state == 0) {
        heap_init();
        if (heap_state == 1)
            bin_insert((chunk *)heap_lo);
    }
    return heap_state == 1;
}

static __noreturn_ void heap_corrupt(const char *what, const void *p)
{
    fprintf(stderr, "malloc: %s (pointer %p)\n", what, p);
    abort();
}

/* Request size -> chunk size, or 0 if impossible. */
static inline size_t request_size(size_t n)
{
    if (n > ((size_t)-1) / 2)
        return 0;
    size_t sz = (n + W + ALIGNMENT - 1) & ~(size_t)(ALIGNMENT - 1);
    return sz < MINCHUNK ? MINCHUNK : sz;
}

/* Makes chunk c (already unlinked or freshly carved, of size csz) in use
 * with a payload of at least `need` bytes, giving the tail back if it is
 * big enough to be a chunk of its own. */
static void carve(chunk *c, size_t csz, size_t need)
{
    size_t prevflag = c->head & PREV_INUSE;
    if (csz - need >= MINCHUNK) {
        chunk *rest = (chunk *)((char *)c + need);
        size_t rsz = csz - need;
        c->head = need | INUSE | prevflag;
        rest->head = rsz | PREV_INUSE;
        FOOT(rest, rsz) = rsz;
        /* the chunk after `rest` already has PREV_INUSE clear or is fixed by caller */
        chunk *after = (chunk *)((char *)rest + rsz);
        after->head &= ~PREV_INUSE;
        bin_insert(rest);
    } else {
        c->head = csz | INUSE | prevflag;
        NEXT_CHUNK(c)->head |= PREV_INUSE;
    }
}

void *malloc(size_t n)
{
    if (!ensure_heap()) {
        errno = ENOMEM;
        return NULL;
    }
    size_t need = request_size(n);
    if (need == 0) {
        errno = ENOMEM;
        return NULL;
    }
    for (int i = bin_index(need); i < NBINS; i++) {
        for (chunk *c = bins[i]; c; c = c->next) {
            size_t csz = CSIZE(c);
            if (csz >= need) {
                bin_unlink(c);
                carve(c, csz, need);
                stat_in_use += CSIZE(c);
                stat_nalloc++;
                if (stat_in_use > stat_peak)
                    stat_peak = stat_in_use;
                return CHUNK2MEM(c);
            }
        }
    }
    errno = ENOMEM;
    return NULL;
}

static chunk *checked_chunk(void *p, const char *who)
{
    chunk *c = MEM2CHUNK(p);
    if (heap_state != 1 || (char *)c < heap_lo || c >= heap_end
        || ((uintptr_t)p & (ALIGNMENT - 1)))
        heap_corrupt(who, p);
    size_t sz = CSIZE(c);
    if (!(c->head & INUSE))
        heap_corrupt("double free or pointer to a free block", p);
    if (sz < MINCHUNK || (sz & (ALIGNMENT - 1)) || sz > (size_t)((char *)heap_end - (char *)c)
        || !(NEXT_CHUNK(c)->head & PREV_INUSE))
        heap_corrupt("corrupted chunk header (buffer overrun?)", p);
    return c;
}

/* Frees chunk c (in use, size sz), merging with free neighbours. */
static void release(chunk *c)
{
    size_t sz = CSIZE(c);
    chunk *nx = (chunk *)((char *)c + sz);
    if (!(nx->head & INUSE)) {
        bin_unlink(nx);
        sz += CSIZE(nx);
    }
    if (!(c->head & PREV_INUSE)) {
        size_t psz = *(size_t *)((char *)c - W);
        chunk *pv = (chunk *)((char *)c - psz);
        bin_unlink(pv);
        sz += psz;
        c = pv;
    }
    c->head = sz | PREV_INUSE;               /* no two free chunks are adjacent */
    FOOT(c, sz) = sz;
    ((chunk *)((char *)c + sz))->head &= ~PREV_INUSE;
    bin_insert(c);
}

void free(void *p)
{
    if (!p)
        return;
    chunk *c = checked_chunk(p, "free() of a pointer malloc did not return");
    stat_in_use -= CSIZE(c);
    stat_nalloc--;
    release(c);
}

void *calloc(size_t nmemb, size_t size)
{
    size_t total;
    if (__builtin_mul_overflow(nmemb, size, &total)) {
        errno = ENOMEM;
        return NULL;
    }
    void *p = malloc(total);
    if (p)
        memset(p, 0, total);
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!p)
        return malloc(n);
    if (n == 0) {
        free(p);
        return NULL;
    }
    chunk *c = checked_chunk(p, "realloc() of a pointer malloc did not return");
    size_t need = request_size(n);
    if (need == 0) {
        errno = ENOMEM;
        return NULL;
    }
    size_t csz = CSIZE(c);
    if (need <= csz) {
        if (csz - need >= MINCHUNK) {
            /* split off the tail and free it (it may merge with the next) */
            chunk *rest = (chunk *)((char *)c + need);
            c->head = need | INUSE | (c->head & PREV_INUSE);
            rest->head = (csz - need) | INUSE | PREV_INUSE;
            stat_in_use -= csz - need;
            release(rest);
        }
        return p;
    }
    chunk *nx = (chunk *)((char *)c + csz);
    if (!(nx->head & INUSE) && csz + CSIZE(nx) >= need) {
        size_t total = csz + CSIZE(nx);
        bin_unlink(nx);
        stat_in_use -= csz;
        carve(c, total, need);
        stat_in_use += CSIZE(c);
        if (stat_in_use > stat_peak)
            stat_peak = stat_in_use;
        return p;
    }
    void *q = malloc(n);
    if (!q)
        return NULL;
    memcpy(q, p, csz - W);
    free(p);
    return q;
}

void malloc_get_stats(struct malloc_stats *st)
{
    memset(st, 0, sizeof *st);
    if (!ensure_heap())
        return;
    st->heap_size = (size_t)((char *)heap_end - heap_lo) + W;
    for (chunk *c = (chunk *)heap_lo; c != heap_end; c = NEXT_CHUNK(c)) {
        size_t sz = CSIZE(c);
        if (c->head & INUSE) {
            st->in_use += sz;
            st->n_alloc++;
        } else {
            st->free_bytes += sz;
            st->n_free++;
            if (sz > st->largest_free)
                st->largest_free = sz;
        }
    }
    st->peak_in_use = stat_peak;
}

int malloc_check(void)
{
    if (!ensure_heap())
        return 0;
    size_t nfree = 0, in_use = 0, nalloc = 0;
    int prev_inuse = 1;
    chunk *c = (chunk *)heap_lo;
    while (c != heap_end) {
        if ((char *)c < heap_lo || c > heap_end)
            return -1;                               /* walked out of the heap */
        size_t sz = CSIZE(c);
        if (sz < MINCHUNK || (sz & (ALIGNMENT - 1)) || sz > (size_t)((char *)heap_end - (char *)c))
            return -2;                               /* bad size */
        if (((c->head & PREV_INUSE) != 0) != prev_inuse)
            return -3;                               /* PREV_INUSE disagrees */
        if (c->head & INUSE) {
            in_use += sz;
            nalloc++;
            prev_inuse = 1;
        } else {
            if (!prev_inuse)
                return -4;                           /* two adjacent free chunks */
            if (FOOT(c, sz) != sz)
                return -5;                           /* bad footer */
            nfree++;
            prev_inuse = 0;
        }
        c = (chunk *)((char *)c + sz);
    }
    if (((heap_end->head & PREV_INUSE) != 0) != prev_inuse || !(heap_end->head & INUSE))
        return -6;                                   /* sentinel damaged */
    size_t listed = 0;
    for (int i = 0; i < NBINS; i++) {
        chunk *prev = NULL;
        for (chunk *f = bins[i]; f; f = f->next) {
            if ((char *)f < heap_lo || f >= heap_end || (f->head & INUSE))
                return -7;                           /* list holds a non-free chunk */
            if (f->prev != prev)
                return -8;                           /* broken back link */
            if (bin_index(CSIZE(f)) != i)
                return -9;                           /* chunk in the wrong class */
            if (++listed > nfree)
                return -10;                          /* cycle or stray entry */
            prev = f;
        }
    }
    if (listed != nfree)
        return -11;                                  /* free chunk missing from lists */
    if (in_use != stat_in_use || nalloc != stat_nalloc)
        return -12;                                  /* statistics drifted */
    return 0;
}
