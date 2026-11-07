// div: 16 independent divides / remainders per iteration.
#include "bench.h"
BENCH_BEGIN(50)
    .rept 4
    div t3, a2, a3;  divu t4, a2, a1;  rem t5, a0, a3;  remu t6, a2, a1
    .endr
BENCH_LOOP_END
BENCH_END(50, 16)
