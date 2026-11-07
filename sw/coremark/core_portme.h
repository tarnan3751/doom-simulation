/*
 * core_portme.h - CoreMark port for the RV32IM SoC (bare metal, our libc).
 *
 * Timing: the 64-bit cycle counter (rdcycle/rdcycleh); seconds = cycles /
 * SYS_CLK_HZ.  Seeds come from volatile variables (performance-run seeds
 * 0, 0, 0x66), the iteration count from ITERATIONS at build time, the data
 * block lives on the stack.  printf is the C library's.
 */
#ifndef CORE_PORTME_H
#define CORE_PORTME_H

#include <stddef.h>
#include <stdint.h>

#define HAS_FLOAT   1
#define HAS_TIME_H  0
#define USE_CLOCK   0
#define HAS_STDIO   1
#define HAS_PRINTF  1

#ifndef COMPILER_VERSION
#define COMPILER_VERSION "GCC" __VERSION__
#endif
#ifndef COMPILER_FLAGS
#define COMPILER_FLAGS FLAGS_STR
#endif
#ifndef MEM_LOCATION
#define MEM_LOCATION "STACK"
#endif

typedef signed short   ee_s16;
typedef unsigned short ee_u16;
typedef signed int     ee_s32;
typedef double         ee_f32;
typedef unsigned char  ee_u8;
typedef unsigned int   ee_u32;
typedef uintptr_t      ee_ptr_int;
typedef size_t         ee_size_t;

#define align_mem(x) (void *)(4 + (((ee_ptr_int)(x)-1) & ~3))

/* cycles; 64 bits so long runs cannot wrap ("Total ticks" prints the low 32) */
#define CORETIMETYPE uint64_t
typedef uint64_t CORE_TICKS;

#define SEED_METHOD SEED_VOLATILE
#define MEM_METHOD  MEM_STACK

#define MULTITHREAD     1
#define USE_PTHREAD     0
#define USE_FORK        0
#define USE_SOCKET      0
#define MAIN_HAS_NOARGC   0
#define MAIN_HAS_NORETURN 0

extern ee_u32 default_num_contexts;

typedef struct CORE_PORTABLE_S
{
    ee_u8 portable_id;
} core_portable;

void portable_init(core_portable *p, int *argc, char *argv[]);
void portable_fini(core_portable *p);

#if !defined(PROFILE_RUN) && !defined(PERFORMANCE_RUN) && !defined(VALIDATION_RUN)
#if (TOTAL_DATA_SIZE == 1200)
#define PROFILE_RUN 1
#elif (TOTAL_DATA_SIZE == 2000)
#define PERFORMANCE_RUN 1
#else
#define VALIDATION_RUN 1
#endif
#endif

#include <stdio.h>
int ee_printf(const char *fmt, ...);   /* = printf (HAS_PRINTF) */

#endif /* CORE_PORTME_H */
