// alu: 16 independent ALU operations per iteration (base CPI).
#include "bench.h"
BENCH_BEGIN(200)
    .rept 2
    add t3, a0, a1;  xor t4, a0, a2;  sub t5, a1, a3;  or  t6, a2, a3
    and a4, a0, a3;  sll a5, a0, a3;  srl a6, a2, a3;  sltu a7, a0, a1
    .endr
BENCH_LOOP_END
BENCH_END(200, 16)
