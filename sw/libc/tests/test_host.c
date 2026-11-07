/*
 * test_host.c - unit tests of the C library on the host, with the host C
 * library as the oracle.  Built and run by `make -C sw/libc test`.
 * Exit status 0 = every check passed.
 */
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tlibc.h"

/* ---- tiny framework ------------------------------------------------------ */

static unsigned long n_checks, n_fail;
static const char *section;
static unsigned long sec_checks, sec_fail;

#define MAX_REPORTS_PER_SECTION 25

static void begin(const char *name)
{
    section = name;
    sec_checks = sec_fail = 0;
}

static void end(void)
{
    printf("  %-34s %9lu checks  %s\n", section, sec_checks,
           sec_fail ? "FAILED" : "ok");
    if (sec_fail)
        printf("  %-34s %9lu failures\n", "", sec_fail);
}

static int check(int ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int check(int ok, const char *fmt, ...)
{
    n_checks++;
    sec_checks++;
    if (!ok) {
        n_fail++;
        if (sec_fail++ < MAX_REPORTS_PER_SECTION) {
            va_list ap;
            va_start(ap, fmt);
            printf("    FAIL [%s] ", section);
            vprintf(fmt, ap);
            printf("\n");
            va_end(ap);
        }
    }
    return ok;
}

/* xorshift64* - deterministic test randomness */
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint64_t rnd64(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 2685821657736338717ull;
}
static uint32_t rnd(uint32_t n) { return n ? (uint32_t)(rnd64() % n) : 0; }

static double bits_to_double(uint64_t u)
{
    double d;
    memcpy(&d, &u, sizeof d);
    return d;
}

/* ---- printf ---------------------------------------------------------------- */

#define PBUF 2048

static void test_printf_table(void)
{
    begin("printf: shared case table");
    char a[PBUF], b[PBUF];
#define P(expected, ...) do {                                                   \
        int ra = snprintf(a, sizeof a, __VA_ARGS__);                           \
        int rb = tl_snprintf(b, sizeof b, __VA_ARGS__);                        \
        check(strcmp(a, expected) == 0, "table row %d: host gives \"%s\" for %s, table says \"%s\"", \
              __LINE__, a, #__VA_ARGS__, expected);                            \
        check(strcmp(b, expected) == 0 && rb == (int)strlen(expected),        \
              "row %d %s: got \"%s\" (%d), want \"%s\"", __LINE__, #__VA_ARGS__, b, rb, expected); \
        (void)ra;                                                              \
    } while (0);
#include "cases_printf.h"
#undef P
    end();
}

/* compare one formatted value: host vs ours, string and return value */
#define CMP_FMT(fmtstr, ...) do {                                                \
        char ha_[PBUF], hb_[PBUF];                                               \
        int ra_ = snprintf(ha_, sizeof ha_, fmtstr, __VA_ARGS__);                \
        int rb_ = tl_snprintf(hb_, sizeof hb_, fmtstr, __VA_ARGS__);             \
        check(ra_ == rb_ && strcmp(ha_, hb_) == 0,                               \
              "fmt \"%s\": host \"%s\" (%d) vs ours \"%s\" (%d)", fmtstr, ha_, ra_, hb_, rb_); \
    } while (0)

static void make_formats(char out[][32], int *count, const char *lens, const char *convs, int with_prec)
{
    static const char *flagsets[] = { "", "-", "+", " ", "#", "0", "-+", "+0", " 0", "#0",
                                      "-#", "+ ", "-0", "#+0", "- #", "-+ #0" };
    static const char *widths[] = { "", "1", "4", "9", "21" };
    static const char *precs[] = { "", ".", ".0", ".1", ".3", ".8", ".17" };
    int n = 0;
    for (const char *c = convs; *c; c++)
        for (size_t f = 0; f < sizeof flagsets / sizeof *flagsets; f++)
            for (size_t w = 0; w < sizeof widths / sizeof *widths; w++)
                for (size_t p = 0; p < (with_prec ? sizeof precs / sizeof *precs : 1); p++) {
                    /* skip combinations the C standard leaves undefined */
                    if (strchr(flagsets[f], '#') && (*c == 'd' || *c == 'i' || *c == 'u' || *c == 'c' || *c == 's'))
                        continue;
                    if (strchr(flagsets[f], '0') && (*c == 'c' || *c == 's' || *c == 'p'))
                        continue;
                    snprintf(out[n++], 32, "%%%s%s%s%s%c", flagsets[f], widths[w], precs[p], lens, *c);
                }
    *count = n;
}

static char fmtbuf[4000][32];

static void test_printf_integers(void)
{
    begin("printf: integer combinations");
    static const long long sval[] = { 0, 1, -1, 7, -7, 42, 99, 100, -100, 127, -128, 255, 256,
                                      32767, -32768, 65535, 65536, 2147483647LL, -2147483647LL - 1,
                                      4294967295LL, 4294967296LL, 1234567890123LL, -987654321098LL,
                                      LLONG_MAX, LLONG_MIN, 0x7EDCBA9876543210LL };
    int nf;
    /* int-sized */
    make_formats(fmtbuf, &nf, "", "diuxXo", 1);
    for (int i = 0; i < nf; i++)
        for (size_t v = 0; v < sizeof sval / sizeof *sval; v++) {
            if (fmtbuf[i][strlen(fmtbuf[i]) - 1] == 'd' || fmtbuf[i][strlen(fmtbuf[i]) - 1] == 'i')
                CMP_FMT(fmtbuf[i], (int)sval[v]);
            else
                CMP_FMT(fmtbuf[i], (unsigned)sval[v]);
        }
    /* long long */
    make_formats(fmtbuf, &nf, "ll", "diuxXo", 1);
    for (int i = 0; i < nf; i++)
        for (size_t v = 0; v < sizeof sval / sizeof *sval; v++)
            CMP_FMT(fmtbuf[i], sval[v]);
    /* other length modifiers (fewer flag combinations) */
    static const char *lens[] = { "hh", "h", "l", "j", "z", "t" };
    for (size_t L = 0; L < sizeof lens / sizeof *lens; L++) {
        make_formats(fmtbuf, &nf, lens[L], "dux", 1);
        for (int i = 0; i < nf; i += 3)
            for (size_t v = 0; v < sizeof sval / sizeof *sval; v++) {
                const char *lf = lens[L];
                if (!strcmp(lf, "l"))
                    CMP_FMT(fmtbuf[i], (long)sval[v]);
                else if (!strcmp(lf, "j"))
                    CMP_FMT(fmtbuf[i], (intmax_t)sval[v]);
                else if (!strcmp(lf, "z"))
                    CMP_FMT(fmtbuf[i], (size_t)sval[v]);
                else if (!strcmp(lf, "t"))
                    CMP_FMT(fmtbuf[i], (ptrdiff_t)sval[v]);
                else
                    CMP_FMT(fmtbuf[i], (int)sval[v]);
            }
    }
    /* random 64-bit values */
    for (int k = 0; k < 20000; k++) {
        unsigned long long u = rnd64() >> rnd(64);
        CMP_FMT("%llu", u);
        CMP_FMT("%lld", (long long)u);
        CMP_FMT("%-+25lld|", -(long long)u);
        CMP_FMT("%#.20llx", u);
        CMP_FMT("%#llo", u);
        CMP_FMT("%x %d %llu", (unsigned)u, (int)(u >> 7), u);
    }
    /* chars, strings, pointers */
    make_formats(fmtbuf, &nf, "", "cs", 1);
    for (int i = 0; i < nf; i++) {
        if (fmtbuf[i][strlen(fmtbuf[i]) - 1] == 'c') {
            CMP_FMT(fmtbuf[i], 'Q');
        } else {
            CMP_FMT(fmtbuf[i], "");
            CMP_FMT(fmtbuf[i], "abc");
            CMP_FMT(fmtbuf[i], "a much longer string, 34 chars...");
        }
    }
    make_formats(fmtbuf, &nf, "", "p", 0);
    for (int i = 0; i < nf; i++) {
        CMP_FMT(fmtbuf[i], (void *)0x1234);
        CMP_FMT(fmtbuf[i], (void *)(uintptr_t)0xFEDCBA9876ull);
    }
    /* * width / precision */
    for (int w = -12; w <= 12; w += 3)
        for (int p = -2; p <= 9; p += 2) {
            CMP_FMT("%*.*d|", w, p, -1234);
            CMP_FMT("%*.*x|", w, p, 0xbeefu);
            CMP_FMT("%*.*s|", w, p, "string");
            CMP_FMT("%-*.*f|", w, p, 3.25);
        }
    end();
}

static void test_printf_floats(void)
{
    begin("printf: float combinations");
    static const double fval[] = { 0.0, -0.0, 1.0, -1.0, 0.5, 1.5, 2.5, 0.1, 0.2, 0.3, 1.0 / 3, 2.0 / 3,
                                   9.5, 99.5, 999.5, 0.05, 0.005, 0.0005, 1e-5, 1e-4, 123456.0,
                                   999999.5, 1e6, 1e7, 1e15, 1e16, 1e17, 1e21, 1e22, 1e23, 1e100, 1e300,
                                   DBL_MAX, DBL_MIN, 4.9406564584124654e-324, 2.2250738585072009e-308,
                                   3.141592653589793, 2.718281828459045, 6.02214076e23, 1.602176634e-19,
                                   0.1 + 0.2, 1e-310, 12345.678901234567, -0.000123, 65536.0 * 65536.0 };
    int nf;
    make_formats(fmtbuf, &nf, "", "feEgG", 1);
    for (int i = 0; i < nf; i++)
        for (size_t v = 0; v < sizeof fval / sizeof *fval; v++) {
            CMP_FMT(fmtbuf[i], fval[v]);
            CMP_FMT(fmtbuf[i], -fval[v]);
        }
    /* special values (zero flag and precision only affect padding) */
    static const char *sfmt[] = { "%f", "%e", "%g", "%F", "%E", "%G", "%10f", "%-10e|", "%+g", "% f", "%010f", "%.3f" };
    for (size_t i = 0; i < sizeof sfmt / sizeof *sfmt; i++) {
        CMP_FMT(sfmt[i], INFINITY);
        CMP_FMT(sfmt[i], -INFINITY);
        if (!strpbrk(sfmt[i], "+ "))        /* macOS drops the sign of NaN; glibc and we keep it */
            CMP_FMT(sfmt[i], NAN);
    }
    {
        char b1[16], b2[16];
        tl_snprintf(b1, sizeof b1, "%+f", NAN);
        tl_snprintf(b2, sizeof b2, "%f", -NAN);
        check(!strcmp(b1, "+nan") && !strcmp(b2, "-nan"), "NaN sign (glibc style): %s %s", b1, b2);
    }
    /* random bit patterns: every exponent, every precision */
    static const char *rfmt[] = { "%.17e", "%.25g", "%.0e", "%.3e", "%g", "%.10g", "%.40f", "%f", "%.0f", "%#.0f", "%a" };
    for (int k = 0; k < 30000; k++) {
        uint64_t u = rnd64();
        double d = bits_to_double(u);
        if (isnan(d) || isinf(d))
            continue;
        for (size_t i = 0; i < sizeof rfmt / sizeof *rfmt; i++) {
            if (!strcmp(rfmt[i], "%a"))
                continue;                       /* %a prints as %e here (documented) */
            if (fabs(d) > 1e60 && strstr(rfmt[i], "f"))
                continue;                       /* keep strings within the buffer */
            CMP_FMT(rfmt[i], d);
        }
    }
    /* decimal-looking numbers that sit on rounding boundaries */
    for (int k = 0; k < 20000; k++) {
        double d = (double)rnd(2000000) / 1000.0 + (rnd(2) ? 0.0005 : 0.0);
        CMP_FMT("%.2f", d);
        CMP_FMT("%.3f", d);
        CMP_FMT("%.1e", d);
        CMP_FMT("%.4g", d);
    }
    /* huge precision on small and large values */
    CMP_FMT("%.1074f", 4.9406564584124654e-324);
    CMP_FMT("%.1100e", 4.9406564584124654e-324);
    CMP_FMT("%.700f", DBL_MAX / 1e290);
    CMP_FMT("%f", DBL_MAX);
    CMP_FMT("%.300g", 1.0 / 3);
    end();
}

static void test_printf_misc(void)
{
    begin("printf: buffers, %n, variants");
    char buf[64];
    /* truncation: return value is the full length, output NUL-terminated */
    for (size_t n = 0; n < 20; n++) {
        char h[64], m[64];
        memset(h, 'Z', sizeof h);
        memset(m, 'Z', sizeof m);
        int rh = snprintf(h, n, "abc%ddef%sghi", 12345, "XYZ");
        int rm = tl_snprintf(m, n, "abc%ddef%sghi", 12345, "XYZ");
        check(rh == rm && memcmp(h, m, sizeof h) == 0, "snprintf size %zu: \"%.20s\" vs \"%.20s\"", n, h, m);
    }
    check(tl_snprintf(NULL, 0, "%d%s", 123, "four") == 7, "snprintf(NULL, 0) length");
    /* %n */
    int n1 = -1, n2 = -1;
    long long n3 = -1;
    signed char n4 = -1;
    tl_snprintf(buf, sizeof buf, "ab%ncdef%n%5d%lln%hhn", &n1, &n2, 7, &n3, &n4);
    check(n1 == 2 && n2 == 6 && n3 == 11 && n4 == 11, "%%n: %d %d %lld %d", n1, n2, n3, n4);
    /* printf to the console */
    tl_console_reset();
    int r = tl_printf("hello %d %s\n", 42, "world");
    check(r == 15 && strcmp(tl_console, "hello 42 world\n") == 0, "printf to console: \"%s\"", tl_console);
    tl_console_reset();
    char longstr[600];
    memset(longstr, 'L', sizeof longstr - 1);
    longstr[sizeof longstr - 1] = 0;
    r = tl_fprintf(tl_stderr, "<%s>%c", longstr, '!');
    check(r == 602 && tl_console_len == 602 && tl_console[0] == '<' && tl_console[600] == '>'
          && tl_console[601] == '!', "fprintf(stderr) long output: %d %zu", r, tl_console_len);
    tl_console_reset();
    tl_puts("line");
    tl_putchar('c');
    tl_fputc('!', tl_stderr);
    tl_fwrite("abcdef", 2, 3, tl_stdout);
    check(strcmp(tl_console, "line\nc!abcdef") == 0, "puts/putchar/fputc/fwrite: \"%s\"", tl_console);
    tl_console_reset();
    /* unknown conversion is printed verbatim */
    tl_snprintf(buf, sizeof buf, "a%yb");
    check(strcmp(buf, "a%yb") == 0, "unknown conversion: \"%s\"", buf);
    end();
}

/* ---- strto* ------------------------------------------------------------------ */

static const char *int_inputs[] = {
    "0", "1", "-1", "+1", "  42", "\t\n -17x", "007", "0x1F", "0X1f", "0x", "0xg", "-0x10", "0b101",
    "z", "Zz", "", " ", "-", "+", "+-1", "123abc", "2147483647", "2147483648", "-2147483648",
    "-2147483649", "4294967295", "4294967296", "-4294967295", "9223372036854775807",
    "9223372036854775808", "-9223372036854775808", "-9223372036854775809", "18446744073709551615",
    "18446744073709551616", "99999999999999999999999", "-99999999999999999999999", "0777", "08", "0x7fffffff",
    "0xffffffffffffffff", "0x10000000000000000", "1010101", "zzzzzz", "  +0x", "1e5", "12.5", "  - 5",
};

/* macOS (FreeBSD) strto* set errno = EINVAL when no conversion was performed;
 * ISO C and glibc leave errno alone, and so do we. */
static int errno_match(int host, int ours, const char *s, const char *end)
{
    if (host == ours)
        return 1;
    return end == s && host == EINVAL && ours == 0;
}

static void test_strto_int(void)
{
    begin("strtol/strtoul/atoi");
    static const int bases[] = { 0, 2, 8, 10, 16, 36, 7 };
    for (size_t i = 0; i < sizeof int_inputs / sizeof *int_inputs; i++)
        for (size_t b = 0; b < sizeof bases / sizeof *bases; b++) {
            const char *s = int_inputs[i];
            int base = bases[b];
            char *e1, *e2;
            errno = 0; __libc_errno = 0;
            long a = strtol(s, &e1, base);
            int ea = errno;
            long c = tl_strtol(s, &e2, base);
            check(a == c && e1 == e2 && errno_match(ea, __libc_errno, s, e1), "strtol(\"%s\", %d): %ld/%td/%d vs %ld/%td/%d",
                  s, base, a, e1 - s, ea, c, e2 - s, __libc_errno);
            errno = 0; __libc_errno = 0;
            unsigned long ua = strtoul(s, &e1, base);
            ea = errno;
            unsigned long uc = tl_strtoul(s, &e2, base);
            check(ua == uc && e1 == e2 && errno_match(ea, __libc_errno, s, e1), "strtoul(\"%s\", %d): %lu/%td/%d vs %lu/%td/%d",
                  s, base, ua, e1 - s, ea, uc, e2 - s, __libc_errno);
        }
    /* random numerals in random bases */
    for (int k = 0; k < 50000; k++) {
        char s[40];
        int base = (int)rnd(37);
        if (base == 1)
            base = 10;
        int len = (int)rnd(24);
        int p = 0;
        if (rnd(4) == 0) s[p++] = ' ';
        if (rnd(3) == 0) s[p++] = rnd(2) ? '-' : '+';
        if (rnd(5) == 0) { s[p++] = '0'; s[p++] = 'x'; }
        for (int j = 0; j < len; j++)
            s[p++] = "0123456789abcdefghijklmnopqrstuvwxyzABCXYZ"[rnd(42)];
        s[p] = 0;
        char *e1, *e2;
        errno = 0; __libc_errno = 0;
        long a = strtol(s, &e1, base);
        int ea = errno;
        long c = tl_strtol(s, &e2, base);
        check(a == c && e1 == e2 && errno_match(ea, __libc_errno, s, e1), "random strtol(\"%s\", %d): %ld vs %ld", s, base, a, c);
        errno = 0; __libc_errno = 0;
        unsigned long ua = strtoul(s, &e1, base);
        ea = errno;
        unsigned long uc = tl_strtoul(s, &e2, base);
        check(ua == uc && e1 == e2 && errno_match(ea, __libc_errno, s, e1), "random strtoul(\"%s\", %d)", s, base);
    }
    /* invalid base */
    char *e;
    __libc_errno = 0;
    check(tl_strtol("123", &e, 1) == 0 && __libc_errno == EINVAL, "base 1 -> EINVAL");
    check(tl_atoi("  -123xyz") == -123 && tl_atoi("99") == 99, "atoi");
    end();
}

/* ---- sscanf -------------------------------------------------------------------- */

static void test_sscanf(void)
{
    begin("sscanf");
    struct { const char *in, *fmt; } icases[] = {
        { "42", "%d" }, { "  -17", "%d" }, { "+8", "%d" }, { "0x1F", "%x" }, { "1f", "%x" }, { "0X1f", "%X" },
        { "0x10", "%i" }, { "010", "%i" }, { "10", "%i" }, { "-0x10", "%i" }, { "777", "%o" }, { "abc", "%d" },
        { "", "%d" }, { "   ", "%d" }, { "12345", "%3d" }, { "4294967295", "%u" }, { "-1", "%u" },
        { "0x1F", " 0x%x" }, { "123", " 0x%x" }, { "0", " 0x%x" }, { "017", " 0%o" }, { "0", " 0%o" }, { "55", " %d" },
        { "x5", "x%d" }, { "y5", "x%d" }, { "%5", "%%%d" }, { "  %  5", "%%%d" }, { "2147483647", "%d" },
    };
    for (size_t i = 0; i < sizeof icases / sizeof *icases; i++) {
        int a = -999, b = -999;
        int ra = sscanf(icases[i].in, icases[i].fmt, &a);
        int rb = tl_sscanf(icases[i].in, icases[i].fmt, &b);
        check(ra == rb && (ra <= 0 || a == b), "sscanf(\"%s\", \"%s\"): host %d/%d ours %d/%d",
              icases[i].in, icases[i].fmt, ra, a, rb, b);
    }
    /* mixed conversions */
    {
        const char *in = "  12 abc x 0x7f  rest_of_line\nnext";
        int i1 = 0, i2 = 0, n1 = 0, n2 = 0;
        char s1[32] = "", s2[32] = "", s3[64] = "", s4[64] = "";
        char c1 = 0, c2 = 0;
        int ra = sscanf(in, "%d %s %c %i %n%[^\n]%n", &i1, s1, &c1, &i2, &n1, s3, &n2);
        int i3 = 0, i4 = 0, m1 = 0, m2 = 0;
        int rb = tl_sscanf(in, "%d %s %c %i %n%[^\n]%n", &i3, s2, &c2, &i4, &m1, s4, &m2);
        check(ra == rb && i1 == i3 && !strcmp(s1, s2) && c1 == c2 && i2 == i4 && n1 == m1
              && n2 == m2 && !strcmp(s3, s4), "mixed: %d vs %d (%d %s %c %d %d [%s] %d) vs (%d %s %c %d %d [%s] %d)",
              ra, rb, i1, s1, c1, i2, n1, s3, n2, i3, s2, c2, i4, m1, s4, m2);
    }
    /* widths, suppression, lengths */
    {
        short hs = 0; signed char hhs = 0; long ls = 0; long long lls = 0; size_t zs = 0;
        char cc[4] = "", ss[8] = "";
        int r = tl_sscanf("123456 -7 300 9999999999 77 xyz abcdefgh", "%3hd%*d %hhd %ld %lld %zu %3c %4s",
                          &hs, &hhs, &ls, &lls, &zs, cc, ss);
        check(r == 7 && hs == 123 && hhs == -7 && ls == 300 && lls == 9999999999LL && zs == 77
              && !memcmp(cc, "xyz", 3) && !strcmp(ss, "abcd"), "widths/lengths: r=%d hs=%d hhs=%d ls=%ld lls=%lld zs=%zu s=%s",
              r, hs, hhs, ls, lls, zs, ss);
        int hosta = 0, oura = 0;
        char hb[16] = "", ob[16] = "";
        int rh = sscanf("abc]def-x", "%[]a-c]%[^-]", hb, hb + 8);
        int ro = tl_sscanf("abc]def-x", "%[]a-c]%[^-]", ob, ob + 8);
        check(rh == ro && !strcmp(hb, ob) && !strcmp(hb + 8, ob + 8), "scansets: %d [%s][%s] vs %d [%s][%s]",
              rh, hb, hb + 8, ro, ob, ob + 8);
        (void)hosta; (void)oura;
        void *p1 = NULL, *p2 = NULL;
        sscanf("0x1234", "%p", &p1);
        tl_sscanf("0x1234", "%p", &p2);
        check(p1 == p2, "%%p: %p vs %p", p1, p2);
    }
    /* randomized: integer conversions, widths and literals vs the host */
    {
        static const char *fmts[] = { "%d", "%i", "%u", "%x", "%o", "%3d%d", "%d,%d", " %d %x", "%2i%3i",
                                      "x%dy%d", "%*d %d", "%hhd %hd", "%lld %lli", "%5x %o", "%%%d", " 0x%x", " 0%o" };
        static const char alphabet[] = "0123456789abcdefxX+- ,%y\t";
        for (int k = 0; k < 40000; k++) {
            char in[24];
            int len = (int)rnd(14);
            for (int j = 0; j < len; j++)
                in[j] = alphabet[rnd(sizeof alphabet - 1)];
            in[len] = 0;
            const char *f = fmts[rnd(sizeof fmts / sizeof *fmts)];
            long long ha = -77, hb = -77, oa = -77, ob = -77;
            int rh, ro;
            if (strstr(f, "ll")) {
                rh = sscanf(in, f, &ha, &hb);
                ro = tl_sscanf(in, f, &oa, &ob);
            } else if (strstr(f, "hh")) {
                signed char c1 = -7, c3 = -7;
                short s2 = -7, s4 = -7;
                rh = sscanf(in, f, &c1, &s2);
                ro = tl_sscanf(in, f, &c3, &s4);
                ha = c1; hb = s2; oa = c3; ob = s4;
            } else {
                int i1 = -77, i2 = -77, i3 = -77, i4 = -77;
                rh = sscanf(in, f, &i1, &i2);
                ro = tl_sscanf(in, f, &i3, &i4);
                ha = i1; hb = i2; oa = i3; ob = i4;
            }
            /* "0x" without hex digits: glibc and we scan it as 0, macOS fails the match */
            int zero_x = strstr(in, "0x") != NULL || strstr(in, "0X") != NULL;
            check(zero_x || (rh == ro && (rh < 1 || ha == oa) && (rh < 2 || hb == ob)),
                  "sscanf(\"%s\", \"%s\"): host %d (%lld %lld) ours %d (%lld %lld)", in, f, rh, ha, hb, ro, oa, ob);
        }
    }
    end();
}

/* ---- string functions ----------------------------------------------------------- */

#define SBUF 8192
static unsigned char sa[SBUF], sb[SBUF], sc[SBUF];

static void fill_random(unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        p[i] = (unsigned char)rnd64();
}

static const size_t lengths[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 15, 16, 17, 23, 31, 32, 33,
                                  63, 64, 65, 100, 127, 128, 129, 255, 256, 1000, 4096 };
#define NLEN (sizeof lengths / sizeof *lengths)

static void test_mem(void)
{
    begin("memcpy/memmove/memset/memcmp/memchr");
    for (size_t li = 0; li < NLEN; li++) {
        size_t n = lengths[li];
        for (int da = 0; da < 8; da++)
            for (int so = 0; so < 8; so++) {
                /* memcpy */
                fill_random(sa, SBUF);
                fill_random(sb, SBUF);
                memcpy(sc, sb, SBUF);
                void *r = tl_memcpy(sb + 64 + da, sa + 64 + so, n);
                memcpy(sc + 64 + da, sa + 64 + so, n);
                check(r == sb + 64 + da && memcmp(sb, sc, SBUF) == 0, "memcpy n=%zu dst+%d src+%d", n, da, so);
                /* memmove inside one buffer, both directions */
                fill_random(sa, SBUF);
                memcpy(sc, sa, SBUF);
                r = tl_memmove(sa + 128 + da, sa + 128 + so + (n / 3), n);
                memmove(sc + 128 + da, sc + 128 + so + (n / 3), n);
                check(r == sa + 128 + da && memcmp(sa, sc, SBUF) == 0, "memmove down n=%zu %d/%d", n, da, so);
                fill_random(sa, SBUF);
                memcpy(sc, sa, SBUF);
                tl_memmove(sa + 128 + da + (n / 3), sa + 128 + so, n);
                memmove(sc + 128 + da + (n / 3), sc + 128 + so, n);
                check(memcmp(sa, sc, SBUF) == 0, "memmove up n=%zu %d/%d", n, da, so);
                tl_memmove(sa + 128 + da, sa + 128 + da + 1, n);
                memmove(sc + 128 + da, sc + 128 + da + 1, n);
                tl_memmove(sa + 128 + da + 1, sa + 128 + da, n);
                memmove(sc + 128 + da + 1, sc + 128 + da, n);
                check(memcmp(sa, sc, SBUF) == 0, "memmove by one n=%zu %d", n, da);
                /* memcmp: equal, then one difference at a random position */
                fill_random(sa, SBUF);
                memcpy(sb + 64 + so, sa + 64 + da, n);
                int c1 = tl_memcmp(sa + 64 + da, sb + 64 + so, n);
                check(c1 == 0, "memcmp equal n=%zu", n);
                if (n) {
                    size_t pos = rnd((uint32_t)n);
                    sb[64 + so + pos] ^= (unsigned char)(1 + rnd(255));
                    int h = memcmp(sa + 64 + da, sb + 64 + so, n);
                    int o = tl_memcmp(sa + 64 + da, sb + 64 + so, n);
                    check((h < 0) == (o < 0) && (h > 0) == (o > 0), "memcmp diff n=%zu pos=%zu", n, pos);
                }
            }
        /* memset */
        for (int da = 0; da < 8; da++) {
            static const int vals[] = { 0, 0xA5, -1, 0x17F };
            for (int v = 0; v < 4; v++) {
                fill_random(sa, SBUF);
                memcpy(sc, sa, SBUF);
                void *r = tl_memset(sa + 32 + da, vals[v], n);
                memset(sc + 32 + da, vals[v], n);
                check(r == sa + 32 + da && memcmp(sa, sc, SBUF) == 0, "memset n=%zu +%d v=%d", n, da, vals[v]);
            }
            /* memchr: present at each position class, and absent */
            fill_random(sa, SBUF);
            for (size_t i = 0; i < n; i++)
                if (sa[32 + da + i] == 0x5A)
                    sa[32 + da + i] = 0x5B;
            check(tl_memchr(sa + 32 + da, 0x5A, n) == NULL, "memchr absent n=%zu", n);
            if (n) {
                size_t pos = rnd((uint32_t)n);
                sa[32 + da + pos] = 0x5A;
                check(tl_memchr(sa + 32 + da, 0x5A, n) == memchr(sa + 32 + da, 0x5A, n), "memchr n=%zu pos=%zu", n, pos);
                check(tl_memchr(sa + 32 + da, 0x15A, n) == memchr(sa + 32 + da, 0x5A, n), "memchr int arg");
            }
        }
    }
    end();
}

static void test_str(void)
{
    begin("str* functions");
    char *s1 = (char *)sa, *s2 = (char *)sb;
    for (size_t li = 0; li < NLEN; li++) {
        size_t n = lengths[li];
        for (int al = 0; al < 8; al++) {
            /* strlen/strnlen with non-zero garbage after the terminator */
            memset(sa, 'x', SBUF);
            for (size_t i = 0; i < n; i++)
                s1[16 + al + i] = (char)(1 + rnd(255));
            s1[16 + al + n] = 0;
            check(tl_strlen(s1 + 16 + al) == n, "strlen n=%zu al=%d", n, al);
            check(tl_strnlen(s1 + 16 + al, n / 2) == n / 2 && tl_strnlen(s1 + 16 + al, n + 5) == n, "strnlen n=%zu", n);
            /* strcmp / strncmp on a copy at another alignment */
            char *r = strcpy(s2 + 16 + (7 - al), s1 + 16 + al);
            check(tl_strcmp(s1 + 16 + al, r) == 0 && tl_strncmp(s1 + 16 + al, r, n + 3) == 0, "strcmp equal n=%zu", n);
            if (n) {
                size_t pos = rnd((uint32_t)n);
                r[pos] = (char)(1 + rnd(255));
                int h = strcmp(s1 + 16 + al, r), o = tl_strcmp(s1 + 16 + al, r);
                check((h < 0) == (o < 0) && (h > 0) == (o > 0), "strcmp diff n=%zu", n);
                size_t lim = rnd((uint32_t)n + 2);
                h = strncmp(s1 + 16 + al, r, lim);
                o = tl_strncmp(s1 + 16 + al, r, lim);
                check((h < 0) == (o < 0) && (h > 0) == (o > 0), "strncmp n=%zu lim=%zu", n, lim);
            }
            /* strchr / strrchr: pick a char from the string, and NUL */
            if (n) {
                int ch = (unsigned char)s1[16 + al + rnd((uint32_t)n)];
                check(tl_strchr(s1 + 16 + al, ch) == strchr(s1 + 16 + al, ch), "strchr n=%zu", n);
                check(tl_strrchr(s1 + 16 + al, ch) == strrchr(s1 + 16 + al, ch), "strrchr n=%zu", n);
            }
            check(tl_strchr(s1 + 16 + al, 0) == s1 + 16 + al + n, "strchr NUL");
        }
    }
    /* words: case folding, searching, spans, tokens */
    static const char *words[] = { "", "a", "A", "abc", "ABC", "abd", "ab", "abcd", "Doom1.WAD", "doom1.wad",
                                   "DOOM2.WAD", "zz", "@[`{", "hello world", "HELLO WORLD!" };
    for (size_t i = 0; i < sizeof words / sizeof *words; i++)
        for (size_t j = 0; j < sizeof words / sizeof *words; j++) {
            const char *a = words[i], *b = words[j];
            int h = strcasecmp(a, b), o = tl_strcasecmp(a, b);
            check((h < 0) == (o < 0) && (h > 0) == (o > 0), "strcasecmp(%s,%s) %d %d", a, b, h, o);
            for (size_t k = 0; k < 6; k++) {
                h = strncasecmp(a, b, k);
                o = tl_strncasecmp(a, b, k);
                check((h < 0) == (o < 0) && (h > 0) == (o > 0), "strncasecmp(%s,%s,%zu)", a, b, k);
            }
            check(tl_strstr(a, b) == strstr(a, b), "strstr(%s,%s)", a, b);
        }
    /* bounded copies */
    char nb[16], hb[16];
    memset(nb, 'Q', sizeof nb);
    memset(hb, 'Q', sizeof hb);
    tl_strncpy(nb, "abc", 8);
    strncpy(hb, "abc", 8);
    check(!memcmp(nb, hb, sizeof nb), "strncpy padding");
    tl_strncpy(nb, "abcdefghijkl", 5);
    strncpy(hb, "abcdefghijkl", 5);
    check(!memcmp(nb, hb, sizeof nb), "strncpy truncation");
    char *dup = tl_strdup("duplicate");
    check(dup && !strcmp(dup, "duplicate"), "strdup");
    tl_free(dup);
    end();
}

/* ---- ctype ---------------------------------------------------------------------- */

static void test_ctype(void)
{
    begin("ctype tables");
    /* the classes of <ctype.h> (sw/libc/include) and the host function for each */
    static const struct { unsigned mask; int (*host)(int); } cls[] = {
        { 0x01 | 0x02 | 0x04, isalnum }, { 0x01 | 0x02, isalpha }, { 0x80, isblank }, { 0x20, iscntrl },
        { 0x04, isdigit }, { 0x01 | 0x02 | 0x04 | 0x10, isgraph }, { 0x02, islower },
        { 0x01 | 0x02 | 0x04 | 0x10 | 0x100, isprint }, { 0x10, ispunct }, { 0x08, isspace },
        { 0x01, isupper }, { 0x40, isxdigit },
    };
    for (int c = -128; c < 256; c++) {
        int ascii = c >= -1 && c < 128;
        for (size_t k = 0; k < sizeof cls / sizeof *cls; k++)
            check(((__libc_ctype[c + 128] & cls[k].mask) != 0) == (ascii && cls[k].host(c) != 0), "class %zu of %d", k, c);
        check(__libc_toupper_tab[c + 128] == (ascii ? toupper(c) : c) && __libc_tolower_tab[c + 128] == (ascii ? tolower(c) : c),
              "case mapping of %d", c);
    }
    end();
}

/* ---- malloc ------------------------------------------------------------------------ */

struct blk { unsigned char *p; size_t n; uint32_t tag; };

static void blk_fill(struct blk *b)
{
    for (size_t i = 0; i < b->n; i++)
        b->p[i] = (unsigned char)(b->tag + i * 7);
}

static int blk_ok(const struct blk *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (b->p[i] != (unsigned char)(b->tag + i * 7))
            return 0;
    return 1;
}

static size_t random_size(void)
{
    uint32_t r = rnd(1000);
    if (r < 600) return rnd(64);
    if (r < 900) return rnd(1024);
    if (r < 990) return rnd(65536);
    return rnd(3u << 20);
}

static void bad_free(void *p)
{
    tl_free(p);
}

static void test_malloc(void)
{
    begin("malloc/calloc/realloc/free");
    struct tl_malloc_stats st0;
    tl_malloc_get_stats(&st0);
    check(st0.heap_size > 60u << 20 && st0.n_free >= 1, "heap set up: %zu bytes", st0.heap_size);
    check(tl_malloc_check() == 0, "initial heap consistent (%d)", tl_malloc_check());

    enum { NB = 3000 };
    static struct blk B[NB];
    memset(B, 0, sizeof B);
    unsigned long ops = 0, ok_align = 1, ok_data = 1;
    for (int it = 0; it < 300000; it++) {
        struct blk *b = &B[rnd(NB)];
        uint32_t op = rnd(10);
        if (!b->p) {
            size_t n = random_size();
            b->tag = (uint32_t)rnd64();
            b->p = op < 2 ? tl_calloc(n, 1) : tl_malloc(n);
            b->n = n;
            if (!b->p)
                continue;
            if (op < 2)
                for (size_t i = 0; i < n; i++)
                    if (b->p[i])
                        ok_data = 0;
            if ((uintptr_t)b->p & 15)
                ok_align = 0;
            blk_fill(b);
        } else if (op < 6) {
            if (!blk_ok(b, b->n))
                ok_data = 0;
            tl_free(b->p);
            b->p = NULL;
        } else {
            size_t n = op == 9 ? b->n / 2 : random_size();
            unsigned char *q = tl_realloc(b->p, n);
            if (n == 0) {
                b->p = NULL;            /* realloc(p, 0) frees */
                continue;
            }
            if (!q)
                continue;               /* out of memory: old block untouched */
            b->p = q;
            if (!blk_ok(b, n < b->n ? n : b->n))
                ok_data = 0;
            if ((uintptr_t)q & 15)
                ok_align = 0;
            b->n = n;
            blk_fill(b);
        }
        if (++ops % 5000 == 0) {
            int c = tl_malloc_check();
            check(c == 0, "heap check after %lu ops: %d", ops, c);
        }
    }
    check(ok_align, "all blocks 16-byte aligned");
    check(ok_data, "block contents intact (calloc zeroed, realloc preserved)");
    for (int i = 0; i < NB; i++)
        if (B[i].p) {
            if (!blk_ok(&B[i], B[i].n))
                ok_data = 0;
            tl_free(B[i].p);
            B[i].p = NULL;
        }
    check(ok_data, "contents intact at final free");
    struct tl_malloc_stats st;
    tl_malloc_get_stats(&st);
    check(tl_malloc_check() == 0 && st.n_alloc == 0 && st.n_free == 1 && st.largest_free == st0.largest_free,
          "everything coalesced back: %zu free chunks, largest %zu (initially %zu)", st.n_free, st.largest_free,
          st0.largest_free);

    /* exhaustion: NULL + ENOMEM, never a crash; then full recovery */
    static void *hold[100000];
    int nh = 0;
    __libc_errno = 0;
    void *big = tl_malloc(st0.largest_free + 1);
    check(big == NULL && __libc_errno == ENOMEM, "oversized request fails with ENOMEM");
    check(tl_malloc((size_t)-1) == NULL && tl_malloc((size_t)-100) == NULL, "SIZE_MAX-ish requests fail");
    check(tl_calloc((size_t)1 << 40, (size_t)1 << 40) == NULL, "calloc overflow fails");
    while (nh < 100000 && (hold[nh] = tl_malloc(1u << 20)) != NULL)
        nh++;
    check(nh > 50 && nh < 100000 && __libc_errno == ENOMEM, "filled the heap with %d MiB blocks", nh);
    void *small = tl_malloc(100);
    for (int i = 0; i < nh; i += 2)
        tl_free(hold[i]);
    void *mid = tl_malloc(3u << 20);   /* needs coalescing? no: holes are 1 MiB each */
    check(mid == NULL || tl_malloc_check() == 0, "fragmented heap still consistent");
    if (mid)
        tl_free(mid);
    for (int i = 1; i < nh; i += 2)
        tl_free(hold[i]);
    tl_free(small);
    mid = tl_malloc(6u << 20);
    check(mid != NULL, "6 MiB block after freeing everything (coalescing works)");
    tl_free(mid);
    tl_malloc_get_stats(&st);
    check(tl_malloc_check() == 0 && st.n_free == 1 && st.largest_free == st0.largest_free, "recovered after exhaustion");

    /* edge cases */
    void *z0 = tl_malloc(0), *z1 = tl_malloc(0);
    check(z0 && z1 && z0 != z1, "malloc(0) returns distinct pointers");
    tl_free(z0);
    tl_free(z1);
    tl_free(NULL);
    void *r0 = tl_realloc(NULL, 10);
    check(r0 != NULL, "realloc(NULL, n)");
    check(tl_realloc(r0, 0) == NULL, "realloc(p, 0) frees");
    /* realloc growth in place: allocate a, free the block after it, grow a */
    unsigned char *pa = tl_malloc(100), *pb = tl_malloc(100), *pc = tl_malloc(100);
    memset(pa, 0x77, 100);
    tl_free(pb);
    unsigned char *pa2 = tl_realloc(pa, 200);
    check(pa2 == pa && pa2[99] == 0x77, "realloc grows in place into a free neighbour");
    tl_free(pa2);
    tl_free(pc);
    /* misuse is detected and aborts (status 134) instead of corrupting the heap */
    int status = tl_catch_exit(bad_free, (char *)tl_malloc(32) + 8);
    check(status == 134, "free of an interior pointer aborts (%d)", status);
    char *dbl = tl_malloc(48);
    tl_free(dbl);
    status = tl_catch_exit(bad_free, dbl);
    check(status == 134, "double free aborts (%d)", status);
    check(tl_malloc_check() == 0, "heap consistent after detected misuse");
    end();
}

/* ---- FILE layer and memfiles ---------------------------------------------------------- */

static void test_files(void)
{
    begin("memfile FILE layer");
    static unsigned char wad[100000];
    for (size_t i = 0; i < sizeof wad; i++)
        wad[i] = (unsigned char)(i * 31 + (i >> 8));
    memcpy(wad, "IWAD", 4);
    check(tl_memfile_register("/some/dir/doom1.wad", wad, sizeof wad) == 0, "register");
    check(tl_memfile_register("", wad, 1) == -1 && __libc_errno == EINVAL, "empty name rejected");

    static const char *names[] = { "doom1.wad", "DOOM1.WAD", "./doom1.wad", "/x/y/Doom1.Wad", "C:\\games\\DOOM1.wad" };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        TL_FILE *f = tl_fopen(names[i], "rb");
        check(f != NULL, "fopen(\"%s\")", names[i]);
        if (f)
            tl_fclose(f);
    }
    static const char *bad[] = { "doom2.wad", "doom1.wa", "doom1.wad/", "", "dir/" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        __libc_errno = 0;
        check(tl_fopen(bad[i], "r") == NULL && __libc_errno == ENOENT, "fopen(\"%s\") fails with ENOENT", bad[i]);
    }
    static const char *wmodes[] = { "w", "wb", "a", "r+", "rb+", "w+" };
    for (size_t i = 0; i < sizeof wmodes / sizeof *wmodes; i++) {
        __libc_errno = 0;
        check(tl_fopen("doom1.wad", wmodes[i]) == NULL && __libc_errno == EROFS, "fopen mode %s fails (EROFS)", wmodes[i]);
        check(tl_fopen("newfile.cfg", wmodes[i]) == NULL, "create with mode %s fails", wmodes[i]);
    }

    TL_FILE *f = tl_fopen("doom1.wad", "rb");
    char hdr[4];
    check(tl_fread(hdr, 1, 4, f) == 4 && !memcmp(hdr, "IWAD", 4) && tl_ftell(f) == 4, "read header");
    /* chunked reads of the whole file */
    static unsigned char back[100000];
    tl_fseek(f, 0, SEEK_SET);
    size_t got = 0, chunk;
    while ((chunk = tl_fread(back + got, 1, 1 + rnd(5000), f)) > 0)
        got += chunk;
    check(got == sizeof wad && !memcmp(back, wad, sizeof wad), "chunked read of whole file");
    check(tl_fread(back, 1, 10, f) == 0, "read at EOF");
    /* element counting with partial elements */
    check(tl_fseek(f, -10, SEEK_END) == 0 && tl_fread(back, 4, 5, f) == 2 && tl_ftell(f) == (long)sizeof wad,
          "partial element count");
    /* seeks */
    check(tl_fseek(f, 1000, SEEK_SET) == 0 && tl_ftell(f) == 1000, "SEEK_SET");
    check(tl_fseek(f, -500, SEEK_CUR) == 0 && tl_ftell(f) == 500, "SEEK_CUR back");
    check(tl_fseek(f, 0, SEEK_END) == 0 && tl_ftell(f) == (long)sizeof wad, "SEEK_END");
    check(tl_fseek(f, -1, SEEK_SET) == -1 && tl_ftell(f) == (long)sizeof wad, "negative seek rejected");
    check(tl_fseek(f, 5, 99) == -1, "bad whence rejected");
    check(tl_fseek(f, 200000, SEEK_SET) == 0 && tl_fread(hdr, 1, 1, f) == 0, "seek past end then read");
    /* the M_FileLength idiom */
    tl_fseek(f, 1234, SEEK_SET);
    long save = tl_ftell(f);
    tl_fseek(f, 0, SEEK_END);
    long len = tl_ftell(f);
    tl_fseek(f, save, SEEK_SET);
    check(len == (long)sizeof wad && tl_ftell(f) == 1234, "length idiom");
    check(tl_fread(hdr, 1, 2, f) == 2 && (unsigned char)hdr[0] == wad[1234] && (unsigned char)hdr[1] == wad[1235]
          && tl_ftell(f) == 1236, "read after seek");
    check(tl_fputc('x', f) == EOF, "writing to a read-only file fails");
    check(tl_fprintf(f, "%d", 5) < 0 && tl_fwrite("ab", 1, 2, f) == 0, "fprintf/fwrite to memfile fail");
    tl_fclose(f);

    /* zero-copy lookup */
    size_t sz = 0;
    check(tl_memfile_lookup("../DOOM1.wad", &sz) == wad && sz == sizeof wad, "memfile_lookup");
    check(tl_memfile_lookup("nope", NULL) == NULL, "memfile_lookup missing");

    /* no storage */
    check(tl_remove("doom1.wad") == -1 && tl_rename("doom1.wad", "x") == -1 && tl_remove("zzz") == -1, "remove/rename fail");
    /* pool exhaustion: FOPEN_MAX streams */
    TL_FILE *fs[20];
    int opened = 0;
    for (int i = 0; i < 20; i++)
        if ((fs[i] = tl_fopen("doom1.wad", "r")) != NULL)
            opened++;
    check(opened == 16, "FOPEN_MAX streams (%d)", opened);
    for (int i = 0; i < 20; i++)
        if (fs[i])
            tl_fclose(fs[i]);
    check(tl_fopen("doom1.wad", "r") != NULL, "streams reusable after fclose");
    end();
}

/* ---- process control ------------------------------------------------------------------- */

static char order[16];
static int norder;
static void h1(void) { order[norder++] = '1'; }
static void h2(void) { order[norder++] = '2'; }
static void h3(void) { order[norder++] = '3'; }
static void do_exit(void *arg) { tl_exit((int)(intptr_t)arg); }
static void do_abort(void *arg) { (void)arg; tl_abort(); }
static void do_assert(void *arg)
{
    (void)arg;
    /* the library's assert() back end */
    tl___assert_fail("x == 1", "file.c", 12, "func");
}

static void test_process(void)
{
    begin("exit/atexit/abort/assert");
    check(tl_atexit(h1) == 0 && tl_atexit(h2) == 0 && tl_atexit(h3) == 0, "atexit");
    int st = tl_catch_exit(do_exit, (void *)(intptr_t)7);
    check(st == 7 && norder == 3 && !memcmp(order, "321", 3), "exit(7): status %d, handlers \"%.*s\"", st, norder, order);
    norder = 0;
    st = tl_catch_exit(do_exit, (void *)(intptr_t)0);
    check(st == 0 && norder == 0, "handlers run only once");
    tl_console_reset();
    st = tl_catch_exit(do_abort, NULL);
    check(st == 134 && strstr(tl_console, "abort") != NULL, "abort: %d \"%s\"", st, tl_console);
    tl_console_reset();
    st = tl_catch_exit(do_assert, NULL);
    check(st == 134 && strstr(tl_console, "Assertion failed: x == 1 (file.c:12: func)") != NULL, "assert: \"%s\"", tl_console);
    tl_console_reset();
    end();
}

/* ---- the nm check of the renaming happens in the Makefile ------------------------------- */

int main(void)
{
    printf("libc host unit tests (host libc as oracle)\n");
    test_printf_table();
    test_printf_integers();
    test_printf_floats();
    test_printf_misc();
    test_strto_int();
    test_sscanf();
    test_mem();
    test_str();
    test_ctype();
    test_malloc();
    test_files();
    test_process();
    printf("total: %lu checks, %lu failures\n", n_checks, n_fail);
    printf(n_fail ? "LIBC HOST TESTS FAILED\n" : "LIBC HOST TESTS PASSED\n");
    return n_fail ? 1 : 0;
}
