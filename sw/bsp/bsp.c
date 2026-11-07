/*
 * bsp.c - board support: startup glue, console, clock, heap bounds, exit,
 * and the platform hooks of the C library (<libc_port.h>).
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <memfile.h>
#include <libc_port.h>
#include "bsp.h"

/* linker script symbols */
extern char __heap_start[], __heap_end[], __stack_limit[], __stack_top[];
extern void (*__preinit_array_start[])(void), (*__preinit_array_end[])(void);
extern void (*__init_array_start[])(void), (*__init_array_end[])(void);
extern void (*__fini_array_start[])(void), (*__fini_array_end[])(void);

/* The bottom 1 KB of the reserve must keep the paint.  (Paint-based: a frame
 * with a large local array the program never writes can step over it.) */
#define STACK_GUARD_WORDS 256

/* ---- console --------------------------------------------------------------- */

void bsp_putc(char c)
{
    /* UART_TX ignores a write while the transmitter is busy: wait for ready */
    while (!(mmio_read(UART_STATUS) & UART_STATUS_TX_READY))
        ;
    mmio_write(UART_TX, (uint8_t)c);
}

void bsp_write(const char *buf, size_t n)
{
    while (n--)
        bsp_putc(*buf++);
}

void bsp_puts(const char *s)
{
    while (*s)
        bsp_putc(*s++);
}

void bsp_puthex(uint32_t v)
{
    char b[10] = { '0', 'x' };
    for (int i = 0; i < 8; i++)
        b[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
    bsp_write(b, 10);
}

void bsp_putdec(uint64_t v)
{
    char b[21];
    int n = 0;
    do {
        b[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    } while (v);
    while (n)
        bsp_putc(b[--n]);
}

void bsp_uart_drain(void)
{
    while (!(mmio_read(UART_STATUS) & UART_STATUS_TX_READY))
        ;
}

void __libc_console_write(const char *buf, size_t len)
{
    bsp_write(buf, len);
}

/* ---- time ------------------------------------------------------------------- */

static uint32_t clk_hz;

uint32_t bsp_clk_hz(void)
{
    if (!clk_hz) {
        clk_hz = mmio_read(SYS_CLK_HZ);
        if (!clk_hz)
            clk_hz = 50000000u;           /* defensive: the register reads 0 if unmapped */
    }
    return clk_hz;
}

uint64_t bsp_cycles(void)
{
    return rdcycle64();
}

uint64_t bsp_instret(void)
{
    uint32_t hi, lo, hi2;
    do {
        __asm__ volatile("rdinstreth %0" : "=r"(hi));
        __asm__ volatile("rdinstret  %0" : "=r"(lo));
        __asm__ volatile("rdinstreth %0" : "=r"(hi2));
    } while (hi != hi2);
    return ((uint64_t)hi << 32) | lo;
}

uint64_t bsp_time_us(void)
{
    uint32_t hz = bsp_clk_hz();
    uint64_t c = rdcycle64();
    if (hz % 1000000u == 0)
        return c / (hz / 1000000u);
    return c / hz * 1000000u + (c % hz) * 1000000u / hz;
}

void bsp_delay_ms(uint32_t ms)
{
    uint64_t end = rdcycle64() + (uint64_t)ms * (bsp_clk_hz() / 1000u);
    while (rdcycle64() < end)
        ;
}

int bsp_is_fast_build(void)
{
    return (mmio_read(SYS_FLAGS) & SYS_FLAGS_FAST) != 0;
}

/* ---- memory layout ---------------------------------------------------------- */

uintptr_t bsp_heap_start(void)  { return (uintptr_t)__heap_start; }
uintptr_t bsp_heap_end(void)    { return (uintptr_t)__heap_end; }
uintptr_t bsp_stack_limit(void) { return (uintptr_t)__stack_limit; }
uintptr_t bsp_stack_top(void)   { return (uintptr_t)__stack_top; }

void __libc_heap_region(void **start, void **end)
{
    *start = __heap_start;
    *end = __heap_end;
}

size_t bsp_stack_used(void)
{
    const uint32_t *p = (const uint32_t *)(void *)__stack_limit;
    const uint32_t *top = (const uint32_t *)(void *)__stack_top;
    while (p < top && *p == BSP_STACK_PAINT)
        p++;
    return (size_t)((const char *)top - (const char *)p);
}

int bsp_stack_overflowed(void)
{
    const uint32_t *p = (const uint32_t *)(void *)__stack_limit;
    for (int i = 0; i < STACK_GUARD_WORDS; i++)
        if (p[i] != BSP_STACK_PAINT)
            return 1;
    return 0;
}

int bsp_stack_check(void)
{
    static int reported;
    if (reported)
        return 1;
    if (!bsp_stack_overflowed())
        return 0;
    reported = 1;
    bsp_puts("\n*** bsp: STACK OVERFLOW: the stack grew below its reserve at ");
    bsp_puthex((uint32_t)(uintptr_t)__stack_limit);
    bsp_puts(" (the top of the heap may be corrupted); raise STACK_SIZE\n");
    return 1;
}

/* ---- boot info: argv and the WAD ---------------------------------------------- */

const struct bootinfo *bsp_bootinfo(void)
{
    return BOOTINFO->magic == BOOTINFO_MAGIC ? BOOTINFO : NULL;
}

#define CMDLINE_MAX ((int)sizeof(((struct bootinfo *)0)->cmdline))
static char  cmdline_copy[CMDLINE_MAX];
static char *argv_table[CMDLINE_MAX / 2 + 2];
static char  prog_name[] = "prog";
static char  wad_name_copy[MEMFILE_NAME_MAX];

static int build_argv(const struct bootinfo *bi)
{
    int argc = 0;
    argv_table[argc++] = prog_name;
    if (bi) {
        memcpy(cmdline_copy, bi->cmdline, CMDLINE_MAX);
        cmdline_copy[CMDLINE_MAX - 1] = '\0';          /* never trust the terminator */
        char *p = cmdline_copy;
        for (;;) {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
                *p++ = '\0';
            if (!*p)
                break;
            argv_table[argc++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
                p++;
        }
    }
    argv_table[argc] = NULL;
    return argc;
}

static void register_wad(const struct bootinfo *bi)
{
    if (!bi || bi->wad_addr == 0 || bi->wad_size == 0)
        return;
    uint32_t a = bi->wad_addr, n = bi->wad_size;
    if (a < RAM_BASE || a - RAM_BASE > RAM_SIZE || n > RAM_SIZE - (a - RAM_BASE)) {
        bsp_puts("bsp: boot info: WAD outside RAM, ignored\n");
        return;
    }
    size_t len = strnlen(bi->wad_name, sizeof bi->wad_name);
    if (len == 0 || len >= sizeof wad_name_copy) {
        bsp_puts("bsp: boot info: bad WAD name, registered as doom1.wad\n");
        memcpy(wad_name_copy, "doom1.wad", 10);
    } else {
        memcpy(wad_name_copy, bi->wad_name, len);
        wad_name_copy[len] = '\0';
    }
    if (memfile_register(wad_name_copy, (const void *)(uintptr_t)a, n) != 0)
        bsp_puts("bsp: could not register the WAD\n");
}

/* Called by crt0 after .bss is cleared; returns argc and stores argv. */
int __bsp_init(char ***argvp)
{
    const struct bootinfo *bi = bsp_bootinfo();
    int argc = build_argv(bi);
    register_wad(bi);
    for (void (**f)(void) = __preinit_array_start; f < __preinit_array_end; f++)
        (*f)();
    for (void (**f)(void) = __init_array_start; f < __init_array_end; f++)
        (*f)();
    *argvp = argv_table;
    return argc;
}

/* ---- termination ------------------------------------------------------------- */

void bsp_exit(int code)
{
    bsp_uart_drain();                 /* let the last character leave the UART */
    mmio_write(SYS_EXIT, (uint32_t)code);
    for (;;)
        __asm__ volatile("wfi");
}

/* exit() lands here after the atexit handlers ran. */
void __libc_exit(int status)
{
    for (void (**f)(void) = __fini_array_end; f > __fini_array_start;)
        (*--f)();
    if (bsp_stack_check() && status == 0)
        status = BSP_EXIT_STACK_OVERFLOW;
    bsp_exit(status);
}
