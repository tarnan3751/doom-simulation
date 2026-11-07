// mul: 16 independent multiplies per iteration.
#include "bench.h"
BENCH_BEGIN(200)
    .rept 4
    mul t3, a0, a1;  mulh t4, a2, a3;  mulhu t5, a0, a2;  mul t6, a1, a3
    .endr
BENCH_LOOP_END
BENCH_END(200, 16)
