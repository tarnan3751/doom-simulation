/* bench.h - harness for the tests/core microbenchmarks (crt.S uses its parts for the C kernel).
 *
 *   BENCH_BEGIN(iters)      set up, read mcycle/minstret, start the loop
 *   BENCH_LOOP_END          close the loop (2 instructions: addi + bnez)
 *   BENCH_END(iters, body)  read the counters again, store the deltas to
 *                           bench_cycles / bench_instret, then tohost = 1
 *
 * The kernel region therefore executes iters * (body + 2) instructions.
 * Registers: s0 = data area, s1 = loop counter, s2/s3 = start counters.
 * All registers are zeroed first: the register file is not reset. A trap ends the run with tohost = 3.
 */
#define BENCH_START                   \
    .section .text.init;              \
    .globl _start;                    \
_start:                               \
    .irp r, 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31; \
    li    x\r, 0;                     \
    .endr;                            \
    la    t0, bench_trap;             \
    csrw  mtvec, t0

#define BENCH_STORE_COUNTS            \
    csrr  t0, mcycle;                 \
    csrr  t1, minstret;               \
    sub   t0, t0, s2;                 \
    sub   t1, t1, s3;                 \
    la    t2, bench_cycles;           \
    sw    t0, 0(t2);                  \
    sw    t1, 4(t2)

#define BENCH_EXIT                    \
    li    t0, 1;                      \
    la    t2, tohost;                 \
    sw    t0, 0(t2);                  \
1:  j     1b;                         \
bench_trap:                           \
    li    t0, 3;                      \
    la    t2, tohost;                 \
    sw    t0, 0(t2);                  \
2:  j     2b

#define BENCH_DATA                    \
    .data;                            \
    .align 4;                         \
    .globl bench_cycles;              \
bench_cycles:  .word 0;               \
    .globl bench_instret;             \
bench_instret: .word 0

#define BENCH_TOHOST                  \
    .section .tohost, "aw", @progbits; \
    .align 6;                         \
    .globl tohost;                    \
tohost: .dword 0

#define BENCH_BEGIN(iters)            \
    BENCH_START;                      \
    la    s0, bench_data;             \
    li    s1, iters;                  \
    li    a0, 12345;                  \
    li    a1, 678;                    \
    li    a2, 0x9e3779b9;             \
    li    a3, 7;                      \
    csrr  s2, mcycle;                 \
    csrr  s3, minstret;               \
    .align 4;                         \
bench_loop:

#define BENCH_LOOP_END                \
    addi  s1, s1, -1;                 \
    bnez  s1, bench_loop

#define BENCH_END(iters, body)        \
    BENCH_STORE_COUNTS;               \
    BENCH_EXIT;                       \
    BENCH_DATA;                       \
    .globl bench_iters;               \
bench_iters:   .word iters;           \
    .globl bench_body;                \
bench_body:    .word body;            \
    .align 6;                         \
bench_data:    .fill 1024, 4, 0x01020304; \
    BENCH_TOHOST
