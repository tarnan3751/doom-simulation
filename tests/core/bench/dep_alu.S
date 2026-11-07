// dep_alu: 16 back-to-back dependent ALU operations (EX<-MEM forwarding every cycle).
#include "bench.h"
BENCH_BEGIN(200)
    .rept 4
    add  a0, a0, a1;  xor a0, a0, a2;  addi a0, a0, 3;  sub a0, a0, a3
    .endr
BENCH_LOOP_END
BENCH_END(200, 16)
