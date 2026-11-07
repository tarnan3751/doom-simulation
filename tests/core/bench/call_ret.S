// call_ret: 4 calls per iteration to small functions (jal + ret via the RAS).
#include "bench.h"
BENCH_BEGIN(200)
    jal  ra, f1
    jal  ra, f2
    jal  ra, f1
    jal  ra, f3
BENCH_LOOP_END
BENCH_END(200, 15)
    .text
f1: addi t3, t3, 1
    ret
f2: addi t4, t4, 1
    addi t5, t5, 1
    ret
f3: jal  t0, leaf           // nested call through the alternate link register
    ret
leaf:
    addi t6, t6, 1
    jr   t0
