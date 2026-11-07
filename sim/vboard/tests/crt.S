# Minimal start-up for the vboard test programs: stack, main(), then SYS_EXIT with its return value.
    .section .text.init
    .globl _start
_start:
    la   sp, 0x807FF000
    call main
    li   t0, 0x10001004
    sw   a0, 0(t0)
1:  j    1b
