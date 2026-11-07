// Timing corners for the model lockstep (--engine accurate --cosim=model): counter CSR writes, a UART write
// while busy (dropped) and to lane 1, the keyboard FIFO overflowing, VID_STATUS / VID_FLIP reads, write-only and
// unmapped registers, divides of every length. Prints one checksum; the run is only meaningful in lockstep.
#include "t.h"
#define CSRW(c, v) __asm__ volatile("csrw " #c ", %0" ::"r"(v))
#define CSRR(c) ({ uint32_t v_; __asm__ volatile("csrr %0, " #c : "=r"(v_)); v_; })
#define CSRRS(c, v) ({ uint32_t v_; __asm__ volatile("csrrs %0, " #c ", %1" : "=r"(v_) : "r"(v)); v_; })
#define CSRRCI(c, v) ({ uint32_t v_; __asm__ volatile("csrrci %0, " #c ", " #v : "=r"(v_)); v_; })

static uint32_t acc;
static void mix(uint32_t v) { acc = (acc ^ v) * 16777619u; }

int main(void) {
    // keyboard: the board sends 20 presses at cycle 2000000; 16 fit, the rest are dropped
    while (CSRR(cycle) < 2100000u) ;
    uint32_t keys = 0;
    for (uint32_t e; (e = R32(KBD_DATA)) != 0; keys++) mix(e);
    mix(R32(KBD_STATUS));
    // UART: the second write lands while the first byte is on the line and is dropped
    R32(UART_TX) = 'A'; R32(UART_TX) = 'B';
    uint32_t polls = 0;
    while (!(R32(UART_STATUS) & 1)) polls++;
    mix(polls);
    *(volatile uint8_t *)(UART_TX + 1) = 'C';                        // byte store to lane 1: a frame with 0x00
    while (!(R32(UART_STATUS) & 1)) ;
    // video: wait for two frame-counter steps, flip twice, read the counter
    for (uint32_t f0 = R32(VID_STATUS) >> 16; (R32(VID_STATUS) >> 16) < f0 + 2; ) ;
    mix(R32(VID_STATUS));
    R32(VID_FLIP) = 1; R32(VID_FLIP) = 1;
    mix(R32(VID_FLIP));
    // write-only and unmapped registers read 0 on the hardware
    VID_FB[5] = 9; VID_PALETTE[3] = 0x123456;
    mix(R32(VID_FB_BASE + 4)); mix(R32(VID_PALETTE_BASE + 12)); mix(R32(UART_BASE + 8)); mix(R32(SYS_FLAGS));
    mix(R32(SYS_CLK_HZ)); mix(R32(KBD_BASE + 8)); mix(R32(VID_BASE + 8)); mix(R32(SYS_EXIT));
    // divides and multiplies: every quotient length, signs, by zero, INT_MIN / -1
    volatile int32_t a = 0x7FFFFFFF, m = (int32_t)0x80000000u, n = -1, z = 0;
    volatile uint32_t ub = 1;
    for (int i = 0; i < 33; i++, ub = ub * 2 + 1) {
        int32_t sb = (int32_t)ub;
        mix((uint32_t)(a / sb)); mix((uint32_t)(-a % sb)); mix((uint32_t)a / ub); mix((uint32_t)a * ub);
    }
    mix((uint32_t)(m / n)); mix((uint32_t)(m % n)); mix((uint32_t)(a / z)); mix((uint32_t)(a % z));
    // counters: writes replace the increment, reads see them (last: mcycle no longer counts from reset)
    CSRW(mcycle, 0x12345678u); mix(CSRR(mcycle)); mix(CSRR(cycle));
    CSRW(mcycleh, 7u); mix(CSRR(mcycleh)); mix(CSRR(mcycle));
    CSRW(minstret, 100u); mix(CSRR(minstret)); mix(CSRR(instret));
    CSRW(minstreth, 3u); mix(CSRR(minstreth)); mix(CSRRS(minstret, 0x10u)); mix(CSRRCI(mcycle, 5));
    CSRW(mcycle, 0xFFFFFFF0u); for (int i = 0; i < 8; i++) mix(CSRR(mcycleh) + CSRR(mcycle));   // carry
    puts_(" keys "); puthex(keys, 2); puts_(" polls "); puthex(polls, 4); puts_(" sum "); puthex(acc, 8);
    putc_('\n');
    return 0;
}
