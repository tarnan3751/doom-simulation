// load_nouse: 8 x (load, independent ALU op) - loads without a load-use hazard.
#include "bench.h"
BENCH_BEGIN(200)
    .rept 8
    lw   t3, 0(s0);  add t4, a0, a1
    .endr
BENCH_LOOP_END
BENCH_END(200, 16)
