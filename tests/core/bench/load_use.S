// load_use: 8 x (load, consumer of the loaded value) - one load-use stall each.
#include "bench.h"
BENCH_BEGIN(200)
    .rept 8
    lw   t3, 0(s0);  add t4, t3, a1
    .endr
BENCH_LOOP_END
BENCH_END(200, 16)
