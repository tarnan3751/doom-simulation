// branch_random: a data-dependent branch on an LFSR bit (about 50% mispredicted).
// Both paths have the same length (8 instructions per iteration + loop).
#include "bench.h"
BENCH_BEGIN(400)
    srli t3, a2, 1
    andi t4, a2, 1
    neg  t4, t4
    li   t5, 0xedb88320
    and  t4, t4, t5
    xor  a2, t3, t4          // a2 = galois LFSR step
    andi t6, a2, 1
    beqz t6, 1f
    addi a4, a4, 1
    j    2f
1:  addi a5, a5, 1
    nop
2:
BENCH_LOOP_END
BENCH_END(400, 10)
