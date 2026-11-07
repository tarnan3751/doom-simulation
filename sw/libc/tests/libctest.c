/*
 * libctest.c - on-target self-test of the C library (RV32IM, real ISA).
 *
 * The same areas as the host unit tests, but against expected values and
 * simple reference implementations instead of a host oracle:
 *   printf case table, strtol/strtoul table, sscanf, string functions at
 *   all alignments, malloc stress with integrity checks and exhaustion, the
 *   memfile FILE layer, ctype; plus a few cycle counts.
 * Exit status 0 = all checks passed, else the number of failures (max 255).
 * Build with -fno-builtin so the reference loops stay loops.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <inttypes.h>
#include <malloc.h>
#include <memfile.h>
#include <bsp.h>

static unsigned n_checks, n_fail;
static const char *section;
static unsigned sec_fail;

static void begin(const char *name)
{
    section = name;
    sec_fail = 0;
}

static void end(void)
{
    printf("  %-28s %s\n", section, sec_fail ? "FAILED" : "ok");
}

#define CHECK(cond, ...) do {                                         \
        n_checks++;                                                   \
        if (!(cond)) {                                                \
            n_fail++;                                                 \
            if (sec_fail++ < 10) {                                    \
                printf("    FAIL [%s] line %d: ", section, __LINE__); \
                printf(__VA_ARGS__);                                  \
                printf("\n");                                         \
            }                                                         \
        }                                                             \
    } while (0)

static uint32_t rng = 12345;
static uint32_t rnd(uint32_t n)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return n ? rng % n : 0;
}

/* ---- printf ------------------------------------------------------------------------ */

static void test_printf(void)
{
    begin("printf case table");
    static char b[1200];
#define P(expected, ...) do {                                                       \
        int r_ = snprintf(b, sizeof b, __VA_ARGS__);                               \
        CHECK(strcmp(b, expected) == 0 && r_ == (int)strlen(expected),            \
              "%s -> \"%.60s\" (%d), want \"%.60s\"", #__VA_ARGS__, b, r_, expected); \
    } while (0);
#include "cases_printf.h"
#undef P
    char t[8];
    CHECK(snprintf(t, sizeof t, "%s", "truncated output") == 16 && !strcmp(t, "truncat"), "truncation");
    CHECK(snprintf(NULL, 0, "%d", 12345) == 5, "size query");
    /* 32-bit long on this target */
    snprintf(b, sizeof b, "%ld %lu %lx", LONG_MIN, ULONG_MAX, (unsigned long)LONG_MAX);
    CHECK(!strcmp(b, "-2147483648 4294967295 7fffffff"), "long is 32 bit: %s", b);
    snprintf(b, sizeof b, "%" PRId32 " %" PRIu64 " %" PRIxPTR " %zu", (int32_t)-5, (uint64_t)1 << 40, (uintptr_t)0x80000000u, sizeof(void *));
    CHECK(!strcmp(b, "-5 1099511627776 80000000 4"), "inttypes macros: %s", b);
    end();
}

/* ---- conversions -------------------------------------------------------------------- */

static void test_conversions(void)
{
    begin("strtol/strtoul/atoi/sscanf");
    static const struct { const char *s; int base; long v; int end; int err; } tab[] = {
        { "0", 10, 0, 1, 0 }, { "-17", 10, -17, 3, 0 }, { "  +42x", 10, 42, 5, 0 }, { "0x1F", 0, 31, 4, 0 },
        { "0x", 16, 0, 1, 0 }, { "0777", 0, 511, 4, 0 }, { "z", 36, 35, 1, 0 }, { "", 10, 0, 0, 0 },
        { "-", 10, 0, 0, 0 }, { "2147483647", 10, LONG_MAX, 10, 0 }, { "2147483648", 10, LONG_MAX, 10, ERANGE },
        { "-2147483648", 10, LONG_MIN, 11, 0 }, { "-2147483649", 10, LONG_MIN, 11, ERANGE }, { "1010", 2, 10, 4, 0 },
    };
    for (unsigned i = 0; i < sizeof tab / sizeof *tab; i++) {
        char *e;
        errno = 0;
        long v = strtol(tab[i].s, &e, tab[i].base);
        CHECK(v == tab[i].v && e - tab[i].s == tab[i].end && errno == tab[i].err, "strtol(\"%s\",%d) = %ld end %d errno %d",
              tab[i].s, tab[i].base, v, (int)(e - tab[i].s), errno);
    }
    char *e;
    errno = 0;
    CHECK(strtoul("4294967295", &e, 10) == 4294967295UL && errno == 0, "strtoul max");
    CHECK(strtoul("-1", &e, 10) == ULONG_MAX, "strtoul -1");
    errno = 0;
    CHECK(strtoul("4294967296", &e, 10) == ULONG_MAX && errno == ERANGE, "strtoul overflow");
    CHECK(atoi(" -123abc") == -123 && atoi("77") == 77, "atoi");

    int a = 0, b = 0, n = 0;
    char w[16] = "", rest[32] = "";
    int r = sscanf("  42 0x1f word tail of line", "%d %i %15s %n%31[^\n]", &a, &b, w, &n, rest);
    CHECK(r == 4 && a == 42 && b == 31 && !strcmp(w, "word") && n == 15 && !strcmp(rest, "tail of line"),
          "sscanf mixed: r=%d a=%d b=%d w=%s n=%d rest=%s", r, a, b, w, n, rest);
    CHECK(sscanf("0x1F", " 0x%x", &a) == 1 && a == 31, "sscanf 0x%%x");
    CHECK(sscanf("123", " 0x%x", &a) == 0, "sscanf literal mismatch");
    CHECK(sscanf("", "%d", &a) == EOF, "sscanf EOF");
    end();
}

/* ---- string functions vs reference loops ----------------------------------------------- */

static unsigned char A[1700], B[1700], C[1700];   /* >= 64 + 3 + n/2 + n + 3 for n = 1024 */

static void ref_copy(unsigned char *d, const unsigned char *s, size_t n)
{
    if (d < s)
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    else
        for (size_t i = n; i-- > 0;) d[i] = s[i];
}

static void test_strings(void)
{
    begin("mem*/str* all alignments");
    static const size_t lens[] = { 0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 100, 255, 1024 };
    for (unsigned li = 0; li < sizeof lens / sizeof *lens; li++) {
        size_t n = lens[li];
        for (int da = 0; da < 4; da++)
            for (int sa = 0; sa < 4; sa++) {
                for (size_t i = 0; i < sizeof A; i++) {
                    A[i] = (unsigned char)rnd(256);
                    B[i] = C[i] = (unsigned char)rnd(256);
                }
                memcpy(B + 32 + da, A + 32 + sa, n);
                ref_copy(C + 32 + da, A + 32 + sa, n);
                CHECK(memcmp(B, C, sizeof B) == 0 && memcmp(B + 32 + da, A + 32 + sa, n) == 0, "memcpy n=%u %d/%d", (unsigned)n, da, sa);
                /* overlapping moves in both directions */
                memcpy(C, A, sizeof A);
                memmove(A + 64 + da, A + 64 + sa + n / 2, n);
                ref_copy(C + 64 + da, C + 64 + sa + n / 2, n);
                CHECK(memcmp(A, C, sizeof A) == 0, "memmove down n=%u %d/%d", (unsigned)n, da, sa);
                memmove(A + 64 + da + n / 2, A + 64 + sa, n);
                ref_copy(C + 64 + da + n / 2, C + 64 + sa, n);
                CHECK(memcmp(A, C, sizeof A) == 0, "memmove up n=%u %d/%d", (unsigned)n, da, sa);
                /* memcmp sign at a random difference */
                memcpy(B + 16 + sa, A + 16 + da, n);
                CHECK(memcmp(A + 16 + da, B + 16 + sa, n) == 0, "memcmp equal n=%u", (unsigned)n);
                if (n) {
                    size_t p = rnd((uint32_t)n);
                    B[16 + sa + p] = (unsigned char)(A[16 + da + p] + 1 + rnd(254));
                    int want = A[16 + da + p] < B[16 + sa + p] ? -1 : 1;
                    int got = memcmp(A + 16 + da, B + 16 + sa, n);
                    CHECK((got < 0 ? -1 : 1) == want && got != 0, "memcmp diff n=%u", (unsigned)n);
                }
            }
        for (int da = 0; da < 4; da++) {
            memcpy(C, B, sizeof B);
            memset(B + 8 + da, 0xA5, n);
            for (size_t i = 0; i < n; i++)
                C[8 + da + i] = 0xA5;
            CHECK(memcmp(B, C, sizeof B) == 0, "memset n=%u +%d", (unsigned)n, da);
            /* strlen / strchr / memchr on a string with garbage after the NUL */
            for (size_t i = 0; i < n; i++)
                A[8 + da + i] = (unsigned char)(1 + rnd(255));
            A[8 + da + n] = 0;
            A[8 + da + n + 1] = 'x';
            CHECK(strlen((char *)A + 8 + da) == n, "strlen n=%u +%d", (unsigned)n, da);
            if (n) {
                size_t p = rnd((uint32_t)n);
                unsigned char c = A[8 + da + p];
                size_t first = 0;
                while (A[8 + da + first] != c)
                    first++;
                CHECK(memchr(A + 8 + da, c, n) == A + 8 + da + first, "memchr n=%u", (unsigned)n);
                CHECK(strchr((char *)A + 8 + da, c) == (char *)A + 8 + da + first, "strchr n=%u", (unsigned)n);
            }
        }
    }
    CHECK(strcmp("abc", "abd") < 0 && strcmp("b", "a") > 0 && strcmp("", "") == 0 && strcmp("\xff", "a") > 0, "strcmp");
    CHECK(strncmp("abcdef", "abcxyz", 3) == 0 && strncmp("abc", "abd", 3) < 0, "strncmp");
    CHECK(strcasecmp("DOOM1.WAD", "doom1.wad") == 0 && strncasecmp("TEXTURE1", "texture2", 7) == 0, "strcasecmp");
    CHECK(strstr("hello world", "o w") != NULL && strstr("abc", "abd") == NULL, "strstr");
    CHECK(strrchr("a/b/c", '/') && !strcmp(strrchr("a/b/c", '/'), "/c"), "strrchr");
    char buf[32];
    strncpy(buf, "abc", sizeof buf);
    CHECK(!strcmp(buf, "abc") && buf[31] == 0, "strncpy pads");
    char *d = strdup("copy me");
    CHECK(d && !strcmp(d, "copy me"), "strdup");
    free(d);
    CHECK(toupper('a') == 'A' && tolower('Q') == 'q' && isdigit('7') && !isalpha('1') && isspace('\t') && isprint(' ')
          && !isprint('\n') && isxdigit('F') && toupper(-1) == -1 && !isalpha(200), "ctype");
    end();
}

/* ---- malloc --------------------------------------------------------------------------------- */

static void test_malloc(void)
{
    begin("malloc stress/exhaustion");
    struct malloc_stats s0, s1;
    malloc_get_stats(&s0);
    printf("    heap: %u bytes managed, largest free block %u\n", (unsigned)s0.heap_size, (unsigned)s0.largest_free);
    enum { NB = 400 };
    static struct { unsigned char *p; size_t n; unsigned char tag; } b[NB];
    int bad_align = 0, bad_data = 0, bad_check = 0;
    /* sized so the whole test stays a few tens of millions of cycles (RTL-friendly) */
    for (int it = 0; it < 4000; it++) {
        int i = (int)rnd(NB);
        if (!b[i].p) {
            uint32_t r = rnd(100);
            size_t n = r < 70 ? rnd(64) : r < 95 ? rnd(2048) : rnd(16384);
            b[i].p = (r & 1) ? calloc(1, n) : malloc(n);
            if (!b[i].p)
                continue;
            if ((uintptr_t)b[i].p & 15)
                bad_align++;
            if (r & 1)
                for (size_t k = 0; k < n; k++)
                    if (b[i].p[k])
                        bad_data++;
            b[i].n = n;
            b[i].tag = (unsigned char)rnd(256);
            memset(b[i].p, b[i].tag, n);
        } else if (rnd(3)) {
            for (size_t k = 0; k < b[i].n; k++)
                if (b[i].p[k] != b[i].tag)
                    bad_data++;
            free(b[i].p);
            b[i].p = NULL;
        } else {
            size_t n = rnd(4096);
            unsigned char *q = realloc(b[i].p, n ? n : 1);
            if (!q)
                continue;
            size_t keep = n < b[i].n ? n : b[i].n;
            for (size_t k = 0; k < keep; k++)
                if (q[k] != b[i].tag)
                    bad_data++;
            b[i].p = q;
            b[i].n = n ? n : 1;
            memset(q, b[i].tag, b[i].n);
        }
        if (it % 500 == 0 && malloc_check() != 0)
            bad_check++;
    }
    CHECK(!bad_align && !bad_data && !bad_check, "stress: align %d data %d check %d", bad_align, bad_data, bad_check);
    for (int i = 0; i < NB; i++) {
        free(b[i].p);
        b[i].p = NULL;
    }
    malloc_get_stats(&s1);
    CHECK(malloc_check() == 0 && s1.n_alloc == 0 && s1.largest_free == s0.largest_free, "all freed and coalesced (%u free chunks)",
          (unsigned)s1.n_free);
    /* Doom-style: one 6 MB zone if the heap is big enough, then exhaustion */
    void *zone = malloc(6u << 20);
    printf("    6 MB zone: %s\n", zone ? "allocated" : "does not fit");
    void *hold[64];
    int nh = 0;
    errno = 0;
    while (nh < 64 && (hold[nh] = malloc(256u << 10)) != NULL)
        nh++;
    CHECK(nh < 64 && errno == ENOMEM, "exhaustion returns NULL with ENOMEM (%d blocks)", nh);
    CHECK(malloc((size_t)-1) == NULL && calloc(0x10000, 0x10000) == NULL, "absurd sizes fail");
    while (nh)
        free(hold[--nh]);
    free(zone);
    malloc_get_stats(&s1);
    CHECK(malloc_check() == 0 && s1.largest_free == s0.largest_free, "recovered after exhaustion");
    end();
}

/* ---- FILE layer ------------------------------------------------------------------------------ */

static void test_files(void)
{
    begin("memfile FILE layer");
    static unsigned char data[5000];
    for (unsigned i = 0; i < sizeof data; i++)
        data[i] = (unsigned char)(i * 13);
    memcpy(data, "PWAD", 4);
    CHECK(memfile_register("test.wad", data, sizeof data) == 0, "register");
    FILE *f = fopen("./saves/../TEST.WAD", "rb");
    CHECK(f != NULL, "fopen case-insensitive, directories ignored");
    if (f) {
        char h[4];
        CHECK(fread(h, 1, 4, f) == 4 && !memcmp(h, "PWAD", 4), "fread header");
        CHECK(fseek(f, 0, SEEK_END) == 0 && ftell(f) == 5000, "fseek/ftell end");
        CHECK(fseek(f, 1000, SEEK_SET) == 0 && fread(h, 1, 1, f) == 1 && (unsigned char)h[0] == data[1000], "fseek set + fread");
        unsigned char buf[600];
        CHECK(fseek(f, -500, SEEK_END) == 0 && fread(buf, 1, 600, f) == 500 && !memcmp(buf, data + 4500, 500),
              "short read");
        CHECK(fclose(f) == 0, "fclose");
    }
    errno = 0;
    CHECK(fopen("test.wad", "wb") == NULL && errno == EROFS, "write open fails");
    errno = 0;
    CHECK(fopen("missing.wad", "rb") == NULL && errno == ENOENT, "missing file");
    CHECK(remove("test.wad") != 0 && rename("test.wad", "x") != 0, "remove/rename fail");
    const struct bootinfo *bi = bsp_bootinfo();
    if (bi && bi->wad_size) {
        FILE *w = fopen(bi->wad_name, "rb");
        char id[5] = "";
        CHECK(w && fread(id, 1, 4, w) == 4 && (!strcmp(id, "IWAD") || !strcmp(id, "PWAD")), "boot WAD readable: %s", id);
        if (w)
            fclose(w);
    }
    end();
}

/* ---- a few cycle counts (instructions on the ISS) ------------------------------------------------- */

static void measure(void)
{
    static unsigned char src[4100], dst[4100];
    uint64_t t;
    printf("  cycle counts:\n");
#define MEASURE(label, bytes, stmt) do {                                    \
        t = bsp_cycles(); stmt; t = bsp_cycles() - t;                       \
        printf("    %-34s %7" PRIu64 " cycles  (%u.%02u cycles/byte)\n", label, t, \
               (unsigned)(t / (bytes)), (unsigned)(t * 100 / (bytes) % 100)); \
    } while (0)
    MEASURE("memcpy 4096 B aligned", 4096, memcpy(dst, src, 4096));
    MEASURE("memcpy 4096 B src misaligned", 4096, memcpy(dst, src + 1, 4096));
    MEASURE("memmove 4096 B overlapping up", 4096, memmove(dst + 4, dst, 4096));
    MEASURE("memset 4096 B", 4096, memset(dst, 7, 4096));
    memset(src, 'a', 4095);
    src[4095] = 0;
    volatile size_t len_sink;
    MEASURE("strlen 4095 B", 4096, len_sink = strlen((char *)src));
    (void)len_sink;
    char buf[64];
    t = bsp_cycles();
    snprintf(buf, sizeof buf, "%d", 123456789);
    printf("    %-34s %7" PRIu64 " cycles\n", "snprintf(\"%d\", 123456789)", bsp_cycles() - t);
    t = bsp_cycles();
    snprintf(buf, sizeof buf, "%f", 3.14159265);
    printf("    %-34s %7" PRIu64 " cycles\n", "snprintf(\"%f\", 3.14159265)", bsp_cycles() - t);
    t = bsp_cycles();
    void *p = malloc(100);
    free(p);
    printf("    %-34s %7" PRIu64 " cycles\n", "malloc(100) + free", bsp_cycles() - t);
}

static void at_exit_handler(void)
{
    printf("libctest: atexit handler ran\n");
}

#define TIMED(fn) do {                                                        \
        uint64_t t0_ = bsp_cycles();                                          \
        fn();                                                                 \
        printf("    (%s: %" PRIu64 " cycles)\n", #fn, bsp_cycles() - t0_);   \
    } while (0)

int main(void)
{
    printf("libctest: C library self-test on the target\n");
    atexit(at_exit_handler);
    TIMED(test_printf);
    TIMED(test_conversions);
    TIMED(test_strings);
    TIMED(test_malloc);
    TIMED(test_files);
    measure();
    printf("libctest: %u checks, %u failures -> %s\n", n_checks, n_fail, n_fail ? "FAILED" : "PASSED");
    return n_fail > 255 ? 255 : (int)n_fail;
}
