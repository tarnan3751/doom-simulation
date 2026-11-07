// branch_taken: 8 always-taken forward branches per iteration (BTB-predicted: 0 bubbles).
#include "bench.h"
BENCH_BEGIN(200)
    .rept 8
    beq  a0, a0, 1f
    nop
1:  add  t3, a0, a1
    .endr
BENCH_LOOP_END
BENCH_END(200, 16)
