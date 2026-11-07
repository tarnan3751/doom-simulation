/*
 * core_portme.c - CoreMark port for the RV32IM SoC.
 *
 * The upstream sources in third_party/coremark are compiled unmodified; this
 * file only provides timing, seeds and the start/end hooks.  At the end it
 * adds two lines to the standard report: CoreMark/MHz and cycles/iteration,
 * and the process exit status is non-zero if CoreMark found a CRC error.
 */
#include <stdlib.h>
#include <stddef.h>
#include <inttypes.h>
#include "coremark.h"
#include <bsp.h>

#ifndef ITERATIONS
#define ITERATIONS 0
#endif

#if VALIDATION_RUN
volatile ee_s32 seed1_volatile = 0x3415;
volatile ee_s32 seed2_volatile = 0x3415;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PERFORMANCE_RUN
volatile ee_s32 seed1_volatile = 0x0;
volatile ee_s32 seed2_volatile = 0x0;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PROFILE_RUN
volatile ee_s32 seed1_volatile = 0x8;
volatile ee_s32 seed2_volatile = 0x8;
volatile ee_s32 seed3_volatile = 0x8;
#endif
volatile ee_s32 seed4_volatile = ITERATIONS;
volatile ee_s32 seed5_volatile = 0;

ee_u32 default_num_contexts = 1;

static uint64_t start_cycles, stop_cycles;
static uint64_t start_instret, stop_instret;

void start_time(void)
{
    start_instret = bsp_instret();
    start_cycles = bsp_cycles();
}

void stop_time(void)
{
    stop_cycles = bsp_cycles();
    stop_instret = bsp_instret();
}

CORE_TICKS get_time(void)
{
    return stop_cycles - start_cycles;
}

secs_ret time_in_secs(CORE_TICKS ticks)
{
    return (secs_ret)ticks / (secs_ret)bsp_clk_hz();
}

void portable_init(core_portable *p, int *argc, char *argv[])
{
    (void)argc;
    (void)argv;
    if (sizeof(ee_ptr_int) != sizeof(ee_u8 *))
        ee_printf("ERROR! Please define ee_ptr_int to a type that holds a pointer!\n");
    if (sizeof(ee_u32) != 4)
        ee_printf("ERROR! Please define ee_u32 to a 32b unsigned type!\n");
    ee_printf("CoreMark on the RV32IM SoC: %" PRIu32 " Hz (%s build), %d iterations requested\n",
              bsp_clk_hz(), bsp_is_fast_build() ? "FAST" : "accurate", (int)seed4_volatile);
    p->portable_id = 1;
}

void portable_fini(core_portable *p)
{
    /* p is results[0].port; the iteration count and error tally sit in the
     * same core_results */
    core_results *r = (core_results *)(void *)((char *)p - offsetof(core_results, port));
    uint64_t cycles = stop_cycles - start_cycles;
    uint64_t insns = stop_instret - start_instret;
    if (cycles && r->iterations) {
        double per_mhz = (double)r->iterations * 1e6 / (double)cycles;
        ee_printf("CoreMark/MHz     : %.4f\n", per_mhz);
        ee_printf("Cycles/iteration : %" PRIu64 "   (instructions/iteration %" PRIu64 ", IPC %.3f)\n",
                  cycles / r->iterations, insns / r->iterations, (double)insns / (double)cycles);
    }
    p->portable_id = 0;
    if (r->err)
        exit(1);
}
