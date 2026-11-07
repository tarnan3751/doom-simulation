/*
 * bsp.h - board support API for programs running on the SoC.
 *
 * Startup (crt0.S): gp/sp, trap vector, .bss cleared, stack reserve painted,
 * argc/argv from the boot-info page (argv[0] = "prog"), the WAD registered
 * as a read-only file (see <memfile.h>), constructors, main(), exit().
 *
 * Exit codes the BSP itself produces (the board reports the value written
 * to SYS_EXIT; a host process exit status keeps the low 8 bits):
 *   0xE0 + mcause   unexpected exception (0xE2 illegal instruction,
 *                   0xE3 ebreak, 0xE4 misaligned load, 0xE6 misaligned
 *                   store, 0xEB ecall, 0xE0 misaligned jump target)
 *   0xDE            trap while handling a trap
 *   0xDF            stack overflow detected at exit (program returned 0)
 *   134             abort() / failed assert()
 */
#ifndef BSP_H
#define BSP_H

#include <stddef.h>
#include <stdint.h>
#include <soc.h>

#define BSP_EXIT_TRAP_BASE      0xE0
#define BSP_EXIT_TRAP_NESTED    0xDE
#define BSP_EXIT_STACK_OVERFLOW 0xDF
#define BSP_EXIT_ABORT          134

#define BSP_STACK_PAINT         0x57ACC0DEu   /* fill pattern of the unused stack */

/* ---- time (the cycle counter; SYS_CLK_HZ cycles per second) ---- */
uint32_t bsp_clk_hz(void);             /* SYS_CLK_HZ (50 MHz nominal)              */
uint64_t bsp_cycles(void);             /* cycles since reset                       */
uint64_t bsp_instret(void);            /* instructions retired since reset         */
uint64_t bsp_time_us(void);            /* microseconds since reset                 */
void     bsp_delay_ms(uint32_t ms);     /* busy-wait                                */
int      bsp_is_fast_build(void);      /* SYS_FLAGS bit 0: FAST=1 simulation build */

/* ---- console: raw UART output (no libc involved; safe in a crash) ---- */
void bsp_putc(char c);
void bsp_write(const char *buf, size_t n);
void bsp_puts(const char *s);          /* no newline added                         */
void bsp_puthex(uint32_t v);           /* "0x%08x"                                 */
void bsp_putdec(uint64_t v);
void bsp_uart_drain(void);             /* wait until the last byte has left        */

/* ---- program termination ---- */
/* Writes `code` to SYS_EXIT after the UART drained, then spins.  Does not run
 * atexit handlers (exit() does, then ends up here). */
__attribute__((noreturn)) void bsp_exit(int code);

/* ---- boot information and memory layout ---- */
const struct bootinfo *bsp_bootinfo(void);   /* NULL unless the magic is valid  */
uintptr_t bsp_heap_start(void);              /* malloc's region [start, end)    */
uintptr_t bsp_heap_end(void);
uintptr_t bsp_stack_limit(void);             /* lowest valid stack address      */
uintptr_t bsp_stack_top(void);               /* STACK_TOP                        */
size_t    bsp_stack_used(void);              /* deepest stack use so far (bytes) */
int       bsp_stack_overflowed(void);        /* the guard words were overwritten */
/* For programs that never exit (Doom): call now and then (e.g. once per
 * frame).  Prints a warning on the console the first time an overflow is
 * seen and returns 1 from then on. */
int       bsp_stack_check(void);

#endif /* BSP_H */
