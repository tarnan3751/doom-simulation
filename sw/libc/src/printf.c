/*
 * printf.c - the one formatter behind every printf variant.
 *
 * Conversions: d i u o x X c s p n % f F e E g G (a/A print as e/E).
 * Flags - + space # 0, field width and precision (also as *), length
 * modifiers hh h l ll q j z t (no long double).  64-bit values use 32-bit division
 * whenever the value fits.  Floating point is converted exactly
 * (fpconv.c) and rounded half-to-even at the last printed digit.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <errno.h>
#include <libc_port.h>
#include "libc_impl.h"

#define FL_MINUS 0x01
#define FL_PLUS  0x02
#define FL_SPACE 0x04
#define FL_HASH  0x08
#define FL_ZERO  0x10

enum { LEN_NONE, LEN_HH, LEN_H, LEN_L, LEN_LL, LEN_J, LEN_Z, LEN_T };

typedef struct {
    libc_sink_fn fn;
    void        *ctx;
    size_t       count;
} out_t;

typedef struct {
    unsigned flags;
    int      width;
    int      prec;      /* -1: not given */
} spec_t;

static inline void put(out_t *o, const char *s, size_t n)
{
    if (n) {
        o->fn(o->ctx, s, n);
        o->count += n;
    }
}

static void pad(out_t *o, char c, int n)
{
    static const char spaces[] = "                ";   /* 16 + NUL */
    static const char zeros[]  = "0000000000000000";
    const char *src = c == '0' ? zeros : spaces;
    while (n > 0) {
        int k = n < 16 ? n : 16;
        put(o, src, (size_t)k);
        n -= k;
    }
}

/* Left/right padding around a body of `len` characters. */
static inline void pad_left(out_t *o, const spec_t *sp, int len)
{
    if (!(sp->flags & FL_MINUS) && sp->width > len)
        pad(o, ' ', sp->width - len);
}

static inline void pad_right(out_t *o, const spec_t *sp, int len)
{
    if ((sp->flags & FL_MINUS) && sp->width > len)
        pad(o, ' ', sp->width - len);
}

/* ---- integers ------------------------------------------------------------ */

static void fmt_int(out_t *o, const spec_t *sp, unsigned long long u, int neg,
                    int base, int upper, int is_ptr)
{
    const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[24];
    int n = 0;
    if (u <= 0xFFFFFFFFull) {
        uint32_t v = (uint32_t)u;
        if (base == 10) {
            while (v) { buf[n++] = (char)('0' + v % 10); v /= 10; }
        } else {
            unsigned shift = base == 16 ? 4 : 3, mask = (unsigned)base - 1;
            while (v) { buf[n++] = digs[v & mask]; v >>= shift; }
        }
    } else {
        if (base == 10) {
            while (u) { buf[n++] = (char)('0' + (int)(u % 10)); u /= 10; }
        } else {
            unsigned shift = base == 16 ? 4 : 3, mask = (unsigned)base - 1;
            while (u) { buf[n++] = digs[u & mask]; u >>= shift; }
        }
    }
    int was_zero = (n == 0);
    if (was_zero && sp->prec != 0)
        buf[n++] = '0';                 /* "0", unless precision 0 asks for nothing */

    int zeros = sp->prec > n ? sp->prec - n : 0;
    if (base == 8 && (sp->flags & FL_HASH) && zeros == 0 && (n == 0 || buf[n - 1] != '0'))
        zeros = 1;                      /* '#' with o: first digit must be 0 */

    char prefix[2];
    int plen = 0;
    if (neg)
        prefix[plen++] = '-';
    else if (sp->flags & FL_PLUS)
        prefix[plen++] = '+';
    else if (sp->flags & FL_SPACE)
        prefix[plen++] = ' ';
    if (base == 16 && (is_ptr || ((sp->flags & FL_HASH) && !was_zero))) {
        prefix[plen++] = '0';
        prefix[plen++] = upper ? 'X' : 'x';
    }

    int len = plen + zeros + n;
    if ((sp->flags & (FL_ZERO | FL_MINUS)) == FL_ZERO && sp->prec < 0 && sp->width > len) {
        zeros += sp->width - len;
        len = sp->width;
    }
    pad_left(o, sp, len);
    put(o, prefix, (size_t)plen);
    pad(o, '0', zeros);
    while (n > 0) {                     /* digits are reversed in buf */
        char rev[24];
        int k = 0;
        while (n > 0)
            rev[k++] = buf[--n];
        put(o, rev, (size_t)k);
    }
    pad_right(o, sp, len);
}

/* ---- floating point ------------------------------------------------------- */

/* Rounds the digit string d[0..n) (value 0.d x 10^E) half-to-even so that
 * at most `keep` digits remain; returns the new length (trailing zeros
 * removed) and bumps *E on a carry out of the first digit. */
static int round_digits(char *d, int n, int *E, int keep)
{
    if (keep >= n)
        return n;
    if (keep < 0)
        return 0;                       /* value < 0.1 unit of the cut: rounds to 0 */
    int up;
    if (d[keep] > '5')
        up = 1;
    else if (d[keep] < '5')
        up = 0;
    else if (keep + 1 < n)
        up = 1;                         /* digits are trimmed: something nonzero follows */
    else
        up = keep > 0 ? ((d[keep - 1] - '0') & 1) : 0;   /* exact tie: to even */
    n = keep;
    if (up) {
        int i = keep - 1;
        while (i >= 0 && d[i] == '9')
            i--;
        if (i < 0) {
            d[0] = '1';
            n = 1;
            (*E)++;
        } else {
            d[i]++;
            n = i + 1;
        }
    }
    while (n > 0 && d[n - 1] == '0')
        n--;
    return n;
}

/* Emits digits d[from .. from+count), treating indices outside [0, n) as '0'. */
static void put_digits(out_t *o, const char *d, int n, int from, int count)
{
    while (count > 0) {
        if (from < 0) {
            int z = -from < count ? -from : count;
            pad(o, '0', z);
            from += z;
            count -= z;
        } else if (from < n) {
            int k = n - from < count ? n - from : count;
            put(o, d + from, (size_t)k);
            from += k;
            count -= k;
        } else {
            pad(o, '0', count);
            count = 0;
        }
    }
}

static void fmt_float(out_t *o, const spec_t *sp, double v, char conv)
{
    int upper = conv >= 'A' && conv <= 'Z';
    char c = (char)(upper ? conv + 32 : conv);
    if (c == 'a')
        c = 'e';
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    int neg = (int)(bits >> 63);
    char sign = neg ? '-' : (sp->flags & FL_PLUS) ? '+' : (sp->flags & FL_SPACE) ? ' ' : 0;
    int slen = sign ? 1 : 0;

    if (((bits >> 52) & 0x7FF) == 0x7FF) {
        const char *t = (bits & 0xFFFFFFFFFFFFFull) ? (upper ? "NAN" : "nan")
                                                     : (upper ? "INF" : "inf");
        int len = slen + 3;
        pad_left(o, sp, len);
        if (sign)
            put(o, &sign, 1);
        put(o, t, 3);
        pad_right(o, sp, len);
        return;
    }

    int prec = sp->prec < 0 ? 6 : sp->prec;
    int alt = (sp->flags & FL_HASH) != 0;
    char d[LIBC_DTOA_MAX];
    int E;
    int n = __libc_dtoa_exact(v, d, &E);
    char style = c;
    int strip = 0;
    if (c == 'g') {
        int P = prec == 0 ? 1 : prec;
        int X = 0;
        if (n) {
            n = round_digits(d, n, &E, P);
            X = E - 1;
        }
        if (P > X && X >= -4) {
            style = 'f';
            prec = P - 1 - X;
        } else {
            style = 'e';
            prec = P - 1;
        }
        strip = !alt;
    }

    if (style == 'f') {
        if (n)
            n = round_digits(d, n, &E, E + prec);
        if (n == 0)
            E = 1;                      /* zero: integer part is "0" */
        if (strip) {
            int sig = n - E;
            if (sig < 0)
                sig = 0;
            if (prec > sig)
                prec = sig;
        }
        int intlen = E > 0 ? E : 1;
        int point = prec > 0 || alt;
        int len = slen + intlen + point + prec;
        int zeros = 0;
        if ((sp->flags & (FL_ZERO | FL_MINUS)) == FL_ZERO && sp->width > len) {
            zeros = sp->width - len;
            len = sp->width;
        }
        pad_left(o, sp, len);
        if (sign)
            put(o, &sign, 1);
        pad(o, '0', zeros);
        if (E > 0)
            put_digits(o, d, n, 0, E);
        else
            put(o, "0", 1);
        if (point)
            put(o, ".", 1);
        put_digits(o, d, n, E, prec);
        pad_right(o, sp, len);
    } else {
        if (n)
            n = round_digits(d, n, &E, prec + 1);
        if (strip && prec > (n > 0 ? n - 1 : 0))
            prec = n > 0 ? n - 1 : 0;
        int X = n ? E - 1 : 0;
        unsigned ax = (unsigned)(X < 0 ? -X : X);
        char ebuf[8];
        int el = 0;
        do {
            ebuf[el++] = (char)('0' + ax % 10);
            ax /= 10;
        } while (ax);
        if (el < 2)
            ebuf[el++] = '0';
        int point = prec > 0 || alt;
        int len = slen + 1 + point + prec + 2 + el;
        int zeros = 0;
        if ((sp->flags & (FL_ZERO | FL_MINUS)) == FL_ZERO && sp->width > len) {
            zeros = sp->width - len;
            len = sp->width;
        }
        pad_left(o, sp, len);
        if (sign)
            put(o, &sign, 1);
        pad(o, '0', zeros);
        put_digits(o, d, n, 0, 1);
        if (point)
            put(o, ".", 1);
        put_digits(o, d, n, 1, prec);
        char ex[2] = { upper ? 'E' : 'e', X < 0 ? '-' : '+' };
        put(o, ex, 2);
        while (el > 0)
            put(o, &ebuf[--el], 1);
        pad_right(o, sp, len);
    }
}

/* ---- the formatter --------------------------------------------------------- */

int __libc_vformat(libc_sink_fn fn, void *ctx, const char *fmt, va_list ap)
{
    out_t o = { fn, ctx, 0 };

    for (;;) {
        const char *lit = fmt;
        while (*fmt && *fmt != '%')
            fmt++;
        put(&o, lit, (size_t)(fmt - lit));
        if (!*fmt)
            break;
        const char *start = fmt++;

        spec_t sp = { 0, 0, -1 };
        for (;; fmt++) {
            if (*fmt == '-') sp.flags |= FL_MINUS;
            else if (*fmt == '+') sp.flags |= FL_PLUS;
            else if (*fmt == ' ') sp.flags |= FL_SPACE;
            else if (*fmt == '#') sp.flags |= FL_HASH;
            else if (*fmt == '0') sp.flags |= FL_ZERO;
            else break;
        }
        if (*fmt == '*') {
            int w = va_arg(ap, int);
            fmt++;
            if (w < 0) {
                sp.flags |= FL_MINUS;
                w = w == INT_MIN ? INT_MAX : -w;
            }
            sp.width = w;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                if (sp.width < INT_MAX / 10)
                    sp.width = sp.width * 10 + (*fmt - '0');
                fmt++;
            }
        }
        if (*fmt == '.') {
            fmt++;
            if (*fmt == '*') {
                int p = va_arg(ap, int);
                fmt++;
                sp.prec = p < 0 ? -1 : p;
            } else {
                sp.prec = 0;
                while (*fmt >= '0' && *fmt <= '9') {
                    if (sp.prec < INT_MAX / 10)
                        sp.prec = sp.prec * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }
        int len = LEN_NONE;
        switch (*fmt) {
        case 'h':
            fmt++;
            if (*fmt == 'h') { fmt++; len = LEN_HH; } else len = LEN_H;
            break;
        case 'l':
            fmt++;
            if (*fmt == 'l') { fmt++; len = LEN_LL; } else len = LEN_L;
            break;
        case 'q': fmt++; len = LEN_LL; break;
        case 'j': fmt++; len = LEN_J; break;
        case 'z': fmt++; len = LEN_Z; break;
        case 't': fmt++; len = LEN_T; break;
        default: break;
        }

        char conv = *fmt;
        if (conv)
            fmt++;
        switch (conv) {
        case 'd':
        case 'i': {
            long long v;
            switch (len) {
            case LEN_HH: v = (signed char)va_arg(ap, int); break;
            case LEN_H:  v = (short)va_arg(ap, int); break;
            case LEN_L:  v = va_arg(ap, long); break;
            case LEN_LL: v = va_arg(ap, long long); break;
            case LEN_J:  v = va_arg(ap, intmax_t); break;
            case LEN_Z:  v = (long long)va_arg(ap, ptrdiff_t); break;  /* signed size_t */
            case LEN_T:  v = va_arg(ap, ptrdiff_t); break;
            default:     v = va_arg(ap, int); break;
            }
            unsigned long long u = v < 0 ? 0ull - (unsigned long long)v : (unsigned long long)v;
            fmt_int(&o, &sp, u, v < 0, 10, 0, 0);
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            unsigned long long u;
            switch (len) {
            case LEN_HH: u = (unsigned char)va_arg(ap, unsigned); break;
            case LEN_H:  u = (unsigned short)va_arg(ap, unsigned); break;
            case LEN_L:  u = va_arg(ap, unsigned long); break;
            case LEN_LL: u = va_arg(ap, unsigned long long); break;
            case LEN_J:  u = va_arg(ap, uintmax_t); break;
            case LEN_Z:  u = va_arg(ap, size_t); break;
            case LEN_T:  u = (unsigned long long)(uintptr_t)va_arg(ap, ptrdiff_t); break;
            default:     u = va_arg(ap, unsigned); break;
            }
            sp.flags &= ~(unsigned)(FL_PLUS | FL_SPACE);
            int base = conv == 'u' ? 10 : conv == 'o' ? 8 : 16;
            fmt_int(&o, &sp, u, 0, base, conv == 'X', 0);
            break;
        }
        case 'p': {
            uintptr_t u = (uintptr_t)va_arg(ap, void *);
            sp.flags &= ~(unsigned)(FL_PLUS | FL_SPACE);
            fmt_int(&o, &sp, u, 0, 16, 0, 1);
            break;
        }
        case 'c': {
            char ch = (char)va_arg(ap, int);
            pad_left(&o, &sp, 1);
            put(&o, &ch, 1);
            pad_right(&o, &sp, 1);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            size_t n = sp.prec >= 0 ? strnlen(s, (size_t)sp.prec) : strlen(s);
            int ln = n > INT_MAX ? INT_MAX : (int)n;
            pad_left(&o, &sp, ln);
            put(&o, s, n);
            pad_right(&o, &sp, ln);
            break;
        }
        case 'f': case 'F': case 'e': case 'E':
        case 'g': case 'G': case 'a': case 'A': {
            double v = va_arg(ap, double);
            fmt_float(&o, &sp, v, conv);
            break;
        }
        case 'n': {
            size_t c = o.count;
            switch (len) {
            case LEN_HH: *va_arg(ap, signed char *) = (signed char)c; break;
            case LEN_H:  *va_arg(ap, short *) = (short)c; break;
            case LEN_L:  *va_arg(ap, long *) = (long)c; break;
            case LEN_LL: *va_arg(ap, long long *) = (long long)c; break;
            case LEN_J:  *va_arg(ap, intmax_t *) = (intmax_t)c; break;
            case LEN_Z:  *va_arg(ap, size_t *) = c; break;
            case LEN_T:  *va_arg(ap, ptrdiff_t *) = (ptrdiff_t)c; break;
            default:     *va_arg(ap, int *) = (int)c; break;
            }
            break;
        }
        case '%':
            put(&o, "%", 1);
            break;
        default:
            /* unknown conversion (or '%' at the end): print it verbatim */
            put(&o, start, (size_t)(fmt - start));
            break;
        }
    }
    if (o.count > INT_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return (int)o.count;
}

/* ---- sinks ------------------------------------------------------------------- */

typedef struct {
    char  *buf;
    size_t cap;      /* bytes available for characters (excluding the NUL) */
    size_t len;
} strsink;

static void str_sink(void *ctx, const char *s, size_t n)
{
    strsink *ss = ctx;
    if (ss->len < ss->cap) {
        size_t room = ss->cap - ss->len;
        size_t k = n < room ? n : room;
        memcpy(ss->buf + ss->len, s, k);
    }
    ss->len += n;
}

int vsnprintf(char *restrict s, size_t n, const char *restrict fmt, va_list ap)
{
    strsink ss = { s, n ? n - 1 : 0, 0 };
    int r = __libc_vformat(str_sink, &ss, fmt, ap);
    if (n)
        s[ss.len < ss.cap ? ss.len : ss.cap] = '\0';
    return r;
}

int snprintf(char *restrict s, size_t n, const char *restrict fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(s, n, fmt, ap);
    va_end(ap);
    return r;
}

/* Console streams: format into a small buffer, hand it over in chunks. */
typedef struct {
    size_t len;
    char   buf[128];
} consink;

static void con_sink(void *ctx, const char *s, size_t n)
{
    consink *cs = ctx;
    if (cs->len + n > sizeof cs->buf) {
        if (cs->len) {
            __libc_console_write(cs->buf, cs->len);
            cs->len = 0;
        }
        if (n >= sizeof cs->buf) {
            __libc_console_write(s, n);
            return;
        }
    }
    memcpy(cs->buf + cs->len, s, n);
    cs->len += n;
}

static void null_sink(void *ctx, const char *s, size_t n)
{
    (void)ctx; (void)s; (void)n;
}

int vfprintf(FILE *restrict f, const char *restrict fmt, va_list ap)
{
    if (!(f->flags & LIBC_F_CONSOLE)) {
        /* nothing else is writable; still consume the arguments */
        __libc_vformat(null_sink, NULL, fmt, ap);
        errno = EBADF;
        return -1;
    }
    consink cs;
    cs.len = 0;
    int r = __libc_vformat(con_sink, &cs, fmt, ap);
    if (cs.len)
        __libc_console_write(cs.buf, cs.len);
    return r;
}

int fprintf(FILE *restrict f, const char *restrict fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char *restrict fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}
