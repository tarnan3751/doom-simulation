/*
 * hello.c - first light: serial output, printf, the cycle counter, argv.
 * Exits 0 when everything it can check by itself is right.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <memfile.h>
#include <bsp.h>

/* `hello trap-<kind>` deliberately faults, to show the BSP trap report. */
static __attribute__((noinline)) int deliberate_fault(const char *kind)
{
    volatile uint32_t word = 0x12345678;
    uintptr_t base = (uintptr_t)&word;
    /* explicit instructions: GCC would split a visibly misaligned access into bytes */
    if (!strcmp(kind, "trap-load")) {
        uint32_t v;
        __asm__ volatile("lw %0, 1(%1)" : "=r"(v) : "r"(base));  /* misaligned load  */
        return (int)v;
    }
    if (!strcmp(kind, "trap-store")) {
        __asm__ volatile("sh %0, 1(%1)" : : "r"(1), "r"(base) : "memory");  /* misaligned store */
        return 0;
    }
    if (!strcmp(kind, "trap-null")) {
        uint32_t v;
        __asm__ volatile("lw %0, 2(%1)" : "=r"(v) : "r"(0));     /* misaligned, near NULL */
        return (int)v;
    }
    if (!strcmp(kind, "trap-illegal")) {
        __asm__ volatile(".word 0x00000000");                    /* illegal (all zero) */
        return 0;
    }
    if (!strcmp(kind, "trap-ebreak")) {
        __asm__ volatile("ebreak");
        return 0;
    }
    if (!strcmp(kind, "trap-ecall")) {
        __asm__ volatile("ecall");
        return 0;
    }
    if (!strcmp(kind, "trap-jump")) {
        void (*volatile fn)(void) = (void (*)(void))(base | 2);  /* misaligned target */
        fn();
        return 0;
    }
    if (!strcmp(kind, "abort"))
        abort();
    printf("unknown fault kind %s\n", kind);
    return 1;
}

/* recursion with 1 KB frames: `hello stack-overflow` goes 300 KB deep */
static __attribute__((noinline)) unsigned deep(unsigned n)
{
    volatile unsigned char frame[1024];
    for (int i = 0; i < 1024; i++)        /* a local buffer that is actually used */
        frame[i] = (unsigned char)(n + i);
    return n ? deep(n - 1) + frame[0] + frame[1023] : 0;
}

int main(int argc, char **argv)
{
    int bad = 0;
    if (argc > 1 && !strcmp(argv[1], "stack-overflow")) {
        printf("hello: recursing 300 KB deep (the stack reserve is %u bytes)\n",
               (unsigned)(bsp_stack_top() - bsp_stack_limit()));
        unsigned r = deep(300);
        printf("hello: back, result %u; deepest stack use %zu bytes\n", r, bsp_stack_used());
        return 0;                         /* the BSP turns this into 0xDF */
    }
    if (argc > 1 && (!strncmp(argv[1], "trap-", 5) || !strcmp(argv[1], "abort"))) {
        printf("hello: causing %s on purpose\n", argv[1]);
        return deliberate_fault(argv[1]);
    }
    printf("hello\n");
    printf("hello from RV32IM: SYS_CLK_HZ = %" PRIu32 " Hz, %s build\n",
           bsp_clk_hz(), bsp_is_fast_build() ? "FAST" : "accurate");

    /* printf self-check: a few conversions with known output */
    char buf[96];
    snprintf(buf, sizeof buf, "%d|%5u|%-4x|%#o|%c|%.3s|%08.3f|%lld|%e",
             -42, 7u, 0xabu, 8, 'Z', "abcdef", 3.14159, -1234567890123LL, 6.25e-3);
    const char *want = "-42|    7|ab  |010|Z|abc|0003.142|-1234567890123|6.250000e-03";
    printf("printf: %s  [%s]\n", buf, strcmp(buf, want) ? "MISMATCH" : "ok");
    bad |= strcmp(buf, want) != 0;

    /* the cycle counter must advance, and agree with instret on the ISS */
    uint64_t c0 = bsp_cycles(), i0 = bsp_instret();
    volatile uint32_t sink = 0;
    for (uint32_t k = 0; k < 1000; k++)
        sink += k;
    uint64_t c1 = bsp_cycles(), i1 = bsp_instret();
    (void)sink;
    printf("cycles: %" PRIu64 " -> %" PRIu64 " (+%" PRIu64 "), instret +%" PRIu64 ", time %" PRIu64 " us since reset\n",
           c0, c1, c1 - c0, i1 - i0, bsp_time_us());
    if (c1 <= c0) {
        printf("cycle counter does not advance\n");
        bad = 1;
    }

    /* argv from the boot-info page */
    printf("argc = %d\n", argc);
    for (int i = 0; i < argc; i++)
        printf("argv[%d] = \"%s\"\n", i, argv[i]);
    if (argc < 1 || strcmp(argv[0], "prog") != 0 || argv[argc] != NULL) {
        printf("argv is malformed\n");
        bad = 1;
    }

    const struct bootinfo *bi = bsp_bootinfo();
    if (bi && bi->wad_size) {
        size_t n = 0;
        const unsigned char *w = memfile_lookup(bi->wad_name, &n);
        printf("wad: \"%s\" %zu bytes at %p, starts \"%.4s\"\n", bi->wad_name, n, (const void *)w,
               w ? (const char *)w : "");
    } else {
        printf("boot info: %s, no WAD\n", bi ? "valid" : "absent");
    }

    /* heap */
    char *p = malloc(1000);
    char *q = malloc(100000);
    printf("malloc: %p %p (heap %#" PRIxPTR "..%#" PRIxPTR ")\n", (void *)p, (void *)q,
           bsp_heap_start(), bsp_heap_end());
    if (!p || !q || ((uintptr_t)p & 15)) {
        printf("malloc failed\n");
        bad = 1;
    }
    free(q);
    free(p);
    printf("stack used so far: %zu bytes\n", bsp_stack_used());
    printf("%s\n", bad ? "hello: FAILED" : "hello: done");
    return bad;
}
