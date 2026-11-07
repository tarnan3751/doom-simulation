// crt.S - start-up for the C benchmarks: stack, .bss clear, counters around main() (bench.h).
#include "bench.h"
    BENCH_START
    li    sp, 0x807ff000
    la    t0, __bss_start
    la    t1, _end
1:  bgeu  t0, t1, 2f
    sw    zero, 0(t0)
    addi  t0, t0, 4
    j     1b
2:  csrr  s2, mcycle
    csrr  s3, minstret
    call  main
    BENCH_STORE_COUNTS
    la    t2, bench_result
    sw    a0, 0(t2)
    BENCH_EXIT
    BENCH_DATA
    .globl bench_result
bench_result: .word 0
    BENCH_TOHOST
