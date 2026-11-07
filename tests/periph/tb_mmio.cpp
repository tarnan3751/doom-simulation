// Standalone testbench for the `mmio` slave (all four peripherals).
// Built twice: TB_FAST=0 (scan-out present) and TB_FAST=1.
#include <memory>
#include <vector>
#include <string>
#include "Vmmio.h"
#include "Vmmio___024root.h"
#include "verilated.h"
#include "tb_common.h"
#include "uart_decoder.h"

#ifndef TB_FAST
#define TB_FAST 0
#endif
#ifndef TB_UART_DIV
#define TB_UART_DIV 16
#endif
#ifndef TB_CLK_HZ
#define TB_CLK_HZ 33333333
#endif

static Checker C("tb_mmio");

struct Tb {
    std::unique_ptr<Vmmio> d{new Vmmio};
    UartDecoder uart{TB_UART_DIV};
    Rng rng{12345};
    uint64_t cycle = 0;
    // contract tracking
    bool have_resp = false;
    uint32_t held = 0;
    // observation
    std::vector<uint8_t> uart_pulses;
    uint64_t flip_cycles = 0;
    uint64_t ready_low_cycles = 0;
    uint64_t accepts = 0;

    auto& root() { return *d->rootp; }

    void eval() { d->eval(); }

    // One clock cycle. Before the edge: the uart pin is sampled and rsp_* is checked against
    // combinational dependence on req_* (R5) by wiggling every request input.
    void tick() {
        uart.tick(d->uart_tx);
        {
            uint8_t rr = d->rsp_ready; uint32_t rd = d->rsp_rdata;
            uint8_t v = d->req_valid, we = d->req_we, st = d->req_wstrb;
            uint32_t a = d->req_addr, w = d->req_wdata;
            for (int i = 0; i < 2; i++) {
                d->req_valid = rng.next() & 1; d->req_we = rng.next() & 1; d->req_wstrb = rng.next() & 15;
                d->req_addr = rng.next() | (rng.next() << 16); d->req_wdata = rng.next();
                d->eval();
                CHECK_EQ(C, d->rsp_ready, rr);
                CHECK_EQ(C, d->rsp_rdata, rd);
            }
            d->req_valid = v; d->req_we = we; d->req_wstrb = st; d->req_addr = a; d->req_wdata = w;
            d->eval();
        }
        bool acc = d->req_valid && d->rsp_ready;
        if (!d->rsp_ready) ++ready_low_cycles;
        d->clk = 1; d->eval(); d->clk = 0; d->eval();
        ++cycle;
        if (acc) ++accepts;
        if (d->uart_tx_valid) uart_pulses.push_back(d->uart_tx_byte);
        if (d->vid_flip) ++flip_cycles;
        if (!d->rst) {
            if (acc) { CHECK_EQ(C, d->rsp_ready, 1);  held = d->rsp_rdata; have_resp = true; }
            else if (have_resp) { CHECK_EQ(C, d->rsp_ready, 1); CHECK_EQ(C, d->rsp_rdata, held); }
        }
    }
    void idle(int n) { d->req_valid = 0; for (int i = 0; i < n; i++) tick(); }
    // random idle gap after an access, sampling the held response (R3)
    void gap() { idle(rng.next() % 4); }

    uint32_t rd(uint32_t addr) {
        d->req_valid = 1; d->req_we = 0; d->req_addr = addr; d->req_wstrb = 0; d->req_wdata = rng.next();
        tick();
        d->req_valid = 0;
        d->eval();
        CHECK_EQ(C, d->rsp_ready, 1);
        uint32_t r = d->rsp_rdata;
        gap();
        return r;
    }
    void wr(uint32_t addr, uint32_t data, uint8_t strb = 0xF) {
        d->req_valid = 1; d->req_we = 1; d->req_addr = addr; d->req_wstrb = strb; d->req_wdata = data;
        tick();
        d->req_valid = 0;
        gap();
    }
    void reset() {
        d->rst = 1; d->req_valid = 0; d->key_valid = 0; d->key_data = 0;
        for (int i = 0; i < 4; i++) tick();
        d->rst = 0;
        have_resp = false;
        idle(1);
    }
};

// ---- shadow of palette / framebuffer ----------------------------------------
struct Shadow {
    uint8_t fb[4][16000] = {};
    uint32_t pal[256] = {};
    bool tracking = false;
};

static uint8_t fb_get(Tb& t, int lane, int idx) { return fb_byte(t.root(), lane, idx); }
static uint32_t pal_get(Tb& t, int i) { return pal_rgb(t.root(), i); }

static int compare_arrays(Tb& t, Shadow& s) {
    int bad = 0;
    for (int l = 0; l < 4; l++)
        for (int i = 0; i < 16000; i++) if (fb_get(t, l, i) != s.fb[l][i]) { if (!bad) std::printf("  fb mismatch lane %d idx %d: %02x vs %02x\n", l, i, fb_get(t, l, i), s.fb[l][i]); bad++; }
    for (int i = 0; i < 256; i++) if (pal_get(t, i) != s.pal[i]) { if (!bad) std::printf("  pal mismatch %d: %06x vs %06x\n", i, pal_get(t, i), s.pal[i]); bad++; }
    return bad;
}

static void shadow_store(Shadow& s, uint32_t addr, uint32_t data, uint8_t strb) {
    if ((addr >> 16) == 0x1001) {
        uint32_t w = (addr >> 2) & 0x3FFF;
        if (w < 16000) for (int l = 0; l < 4; l++) if (strb >> l & 1) s.fb[l][w] = data >> (8 * l);
    } else if ((addr >> 10) == (0x10003400u >> 10)) {   // 0x10003400..0x100037FF
        uint32_t i = (addr >> 2) & 0xFF;
        for (int l = 0; l < 3; l++) if (strb >> l & 1) s.pal[i] = (s.pal[i] & ~(0xFFu << (8 * l))) | (((data >> (8 * l)) & 0xFF) << (8 * l));
    }
}

// ----------------------------------------------------------------------------
static void test_reset_and_sys(Tb& t) {
    std::puts("[sys / reset / unmapped]");
    // ready is 0 during reset and 1 afterwards; line idle high
    t.d->rst = 1; t.d->req_valid = 0;
    t.d->clk = 1; t.d->eval(); t.d->clk = 0; t.d->eval();
    t.d->clk = 1; t.d->eval(); t.d->clk = 0; t.d->eval();
    CHECK_EQ(C, t.d->uart_tx, 1);
    CHECK_EQ(C, t.d->halted, 0);
    t.d->rst = 0;
    t.d->clk = 1; t.d->eval(); t.d->clk = 0; t.d->eval();
    CHECK_EQ(C, t.d->rsp_ready, 1);
    t.have_resp = false;

    CHECK_EQ(C, t.rd(SYS_CLK_HZ), TB_CLK_HZ);
    CHECK_EQ(C, t.rd(SYS_FLAGS), TB_FAST);
    t.wr(SYS_CLK_HZ, 0x1234); t.wr(SYS_FLAGS, 0xFFFFFFFF);
    CHECK_EQ(C, t.rd(SYS_CLK_HZ), TB_CLK_HZ);
    CHECK_EQ(C, t.rd(SYS_FLAGS), TB_FAST);
    CHECK_EQ(C, t.d->halted, 0);
    CHECK_EQ(C, t.rd(SYS_EXIT), 0);          // write-only
    CHECK_EQ(C, t.rd(0x1000100C), 0);
    t.wr(0x1000100C, 0xFFFFFFFF); CHECK_EQ(C, t.d->halted, 0);
    t.wr(SYS_EXIT, 0xDEAD0007);
    CHECK_EQ(C, t.d->halted, 1);
    CHECK_EQ(C, t.d->exit_code, 0xDEAD0007u);
    t.wr(SYS_EXIT, 3);                       // sticky halted, latest code wins
    CHECK_EQ(C, t.d->halted, 1);
    CHECK_EQ(C, t.d->exit_code, 3u);
    t.wr(SYS_EXIT, 0, 0x1);                  // strobes are ignored for registers
    CHECK_EQ(C, t.d->exit_code, 0u);
    CHECK_EQ(C, t.rd(SYS_EXIT), 0);

    // unmapped addresses read 0
    const uint32_t un[] = {0x10000008, 0x10000FFC, 0x10001010, 0x10001FFC, 0x10002008, 0x10002FFC,
                           0x10003008, 0x10003100, 0x10003800, 0x10003FFC, 0x10004000, 0x1000F000,
                           0x10020000, 0x1001FA00, 0x1001FFFC, 0x00000000,
                           0x7FFFFFFC, 0x20000000, 0x30001000 };
    for (uint32_t a : un) { t.wr(a, 0xFFFFFFFF); CHECK_EQ(C, t.rd(a), 0); }
    CHECK_EQ(C, t.uart_pulses.size(), 0);
    CHECK_EQ(C, t.flip_cycles, 0);
    CHECK_EQ(C, t.rd(KBD_STATUS), 0);
    CHECK_EQ(C, t.rd(VID_FLIP), 0);
    // write-only registers read 0
    CHECK_EQ(C, t.rd(UART_TX), 0);
    CHECK_EQ(C, t.rd(VID_PAL), 0);
    CHECK_EQ(C, t.rd(VID_FB), 0);
    CHECK_EQ(C, t.rd(VID_FB + 100), 0);
    CHECK_EQ(C, t.uart.count(), 0);
}

static void wait_uart_idle(Tb& t) { for (int i = 0; i < 20 * TB_UART_DIV + 50; i++) t.idle(1); }

static void test_uart(Tb& t) {
    std::puts("[uart]");
    t.uart.clear(); t.uart_pulses.clear();
    CHECK_EQ(C, t.d->uart_tx, 1);
    CHECK_EQ(C, t.rd(UART_STATUS), 1);
    CHECK_EQ(C, t.uart_pulses.size(), 0);

    // single byte; hold the response / wait a long time -> exactly one byte goes out
    t.wr(UART_TX, 0xFFFFFF55);                  // only bits [7:0] matter
    CHECK_EQ(C, t.rd(UART_STATUS), 0);          // busy right after the accept
    wait_uart_idle(t);
    CHECK_EQ(C, t.uart.count(), 1);
    if (t.uart.count() == 1) CHECK_EQ(C, t.uart.bytes()[0], 0x55);
    CHECK_EQ(C, t.uart_pulses.size(), 1);
    CHECK_EQ(C, t.rd(UART_STATUS), 1);
    CHECK_EQ(C, t.d->uart_tx, 1);

    // a write while busy is ignored (no pulse, no byte)
    t.wr(UART_TX, 0x31);
    t.wr(UART_TX, 0xAA);
    t.wr(UART_TX, 0xBB);
    CHECK_EQ(C, t.rd(UART_STATUS), 0);
    wait_uart_idle(t);
    CHECK_EQ(C, t.uart.count(), 2);
    if (t.uart.count() == 2) CHECK_EQ(C, t.uart.bytes()[1], 0x31);
    CHECK_EQ(C, t.uart_pulses.size(), 2);

    // exact frame timing: start bit begins the cycle after the accept, 10 bits long
    {
        t.uart.clear();
        t.d->req_valid = 1; t.d->req_we = 1; t.d->req_addr = UART_TX; t.d->req_wstrb = 1; t.d->req_wdata = 0xA5;
        t.tick();                               // accept edge
        t.d->req_valid = 0;
        // pin must be low from the next cycle for DIV cycles etc.
        int first_low = -1, last_low_end = -1;
        for (int i = 0; i < 12 * TB_UART_DIV; i++) {
            if (t.d->uart_tx == 0 && first_low < 0) first_low = i;
            t.tick();
            if (t.d->uart_tx == 0) last_low_end = i;
        }
        CHECK_EQ(C, first_low, 0);
        CHECK(C, last_low_end >= 0);
        CHECK_EQ(C, t.uart.count(), 1);
        if (t.uart.count()) CHECK_EQ(C, t.uart.bytes()[0], 0xA5);
    }

    // back-to-back bytes paced by tx_ready (poll), random data
    t.uart.clear(); t.uart_pulses.clear();
    std::vector<uint8_t> sent;
    uint64_t t0 = t.cycle;
    for (int i = 0; i < 40; i++) {
        uint8_t b = (i == 0) ? 0x00 : (i == 1) ? 0xFF : (t.rng.next() & 0xFF);
        int guard = 0;
        while (!(t.rd(UART_STATUS) & 1)) { if (++guard > 100000) { CHECK(C, false); break; } }
        t.wr(UART_TX, b);
        sent.push_back(b);
    }
    wait_uart_idle(t);
    CHECK_EQ(C, t.uart.count(), 40);
    CHECK_EQ(C, t.uart.bytes() == sent, 1);
    CHECK_EQ(C, t.uart_pulses == sent, 1);
    CHECK_EQ(C, t.uart.framing_errors(), 0);
    CHECK_EQ(C, t.uart.timing_errors(), 0);
    // 40 frames of 10*DIV cycles each is the lower bound on elapsed time
    CHECK(C, t.cycle - t0 >= 40ull * 10 * TB_UART_DIV);
}

static void test_kbd(Tb& t) {
    std::puts("[keyboard]");
    auto key = [&](uint32_t d) { t.d->key_valid = 1; t.d->key_data = d; t.tick(); t.d->key_valid = 0; };
    CHECK_EQ(C, t.rd(KBD_STATUS), 0);
    CHECK_EQ(C, t.rd(KBD_DATA), 0);
    CHECK_EQ(C, t.rd(KBD_DATA), 0);
    CHECK_EQ(C, t.rd(KBD_STATUS), 0);

    key(0x100 | 0x41); key(0x41); key(0x1FF);
    CHECK_EQ(C, t.rd(KBD_STATUS), 1);
    CHECK_EQ(C, t.rd(KBD_STATUS), 1);                // no side effect
    // hold one KBD_DATA response for a long time: exactly one event is popped
    t.d->req_valid = 1; t.d->req_we = 0; t.d->req_addr = KBD_DATA; t.tick(); t.d->req_valid = 0;
    uint32_t first = t.d->rsp_rdata;
    CHECK_EQ(C, first, 0x80000000u | 0x141);
    for (int i = 0; i < 300; i++) { t.idle(1); CHECK_EQ(C, t.d->rsp_rdata, first); CHECK_EQ(C, t.d->rsp_ready, 1); }
    CHECK_EQ(C, t.rd(KBD_DATA), 0x80000000u | 0x41);
    CHECK_EQ(C, t.rd(KBD_STATUS), 1);
    CHECK_EQ(C, t.rd(KBD_DATA), 0x80000000u | 0x1FF);
    CHECK_EQ(C, t.rd(KBD_STATUS), 0);
    CHECK_EQ(C, t.rd(KBD_DATA), 0);
    t.wr(KBD_DATA, 0x1234); t.wr(KBD_STATUS, 1);     // stores ignored, no pop
    key(0x155);
    t.wr(KBD_DATA, 0x1234);
    CHECK_EQ(C, t.rd(KBD_STATUS), 1);
    CHECK_EQ(C, t.rd(KBD_DATA), 0x80000155u);
    CHECK_EQ(C, t.rd(KBD_DATA), 0);

    // overflow: 20 events with no pops -> the first 16 are kept, in order
    for (int i = 0; i < 20; i++) key(0x100 | i);
    for (int i = 0; i < 16; i++) CHECK_EQ(C, t.rd(KBD_DATA), 0x80000000u | (0x100 | i));
    CHECK_EQ(C, t.rd(KBD_STATUS), 0);
    CHECK_EQ(C, t.rd(KBD_DATA), 0);
    // fill, pop one, push one more (accepted), drain
    for (int i = 0; i < 16; i++) key(i);
    CHECK_EQ(C, t.rd(KBD_DATA), 0x80000000u);
    key(0xAA);                                       // accepted: there is room again
    key(0xBB);                                       // dropped: full again
    for (int i = 1; i < 16; i++) CHECK_EQ(C, t.rd(KBD_DATA), 0x80000000u | i);
    CHECK_EQ(C, t.rd(KBD_DATA), 0x800000AAu);
    CHECK_EQ(C, t.rd(KBD_DATA), 0);
    // events in consecutive cycles with interleaved reads (one event per cycle at most)
    for (int round = 0; round < 50; round++) {
        int n = 1 + t.rng.next() % 5;
        for (int i = 0; i < n; i++) key((round * 8 + i) & 0x1FF);
        for (int i = 0; i < n; i++) CHECK_EQ(C, t.rd(KBD_DATA), 0x80000000u | ((round * 8 + i) & 0x1FF));
        CHECK_EQ(C, t.rd(KBD_STATUS), 0);
    }
    // a key event in the same cycle as a pop
    key(0x10);
    t.d->key_valid = 1; t.d->key_data = 0x11;
    t.d->req_valid = 1; t.d->req_we = 0; t.d->req_addr = KBD_DATA;
    t.tick();                                        // pop and push at the same edge
    t.d->key_valid = 0; t.d->req_valid = 0;
    CHECK_EQ(C, t.d->rsp_rdata, 0x80000010u);
    t.idle(2);
    CHECK_EQ(C, t.rd(KBD_DATA), 0x80000011u);
    CHECK_EQ(C, t.rd(KBD_DATA), 0);
}

static void test_video_regs(Tb& t) {
    std::puts("[video registers]");
    uint32_t n0 = t.rd(VID_FLIP);
    CHECK_EQ(C, n0, 0);
    uint64_t pc0 = t.flip_cycles;
    // each write: exactly one cycle of vid_flip, in the cycle after the accept; counter +1
    for (int i = 1; i <= 5; i++) {
        t.d->req_valid = 1; t.d->req_we = 1; t.d->req_addr = VID_FLIP; t.d->req_wstrb = 0xF; t.d->req_wdata = 0xCAFE0000 + i;
        t.tick();
        t.d->req_valid = 0;
        CHECK_EQ(C, t.d->vid_flip, 1);               // pulse visible right after the accept edge
        t.tick();
        CHECK_EQ(C, t.d->vid_flip, 0);
        CHECK_EQ(C, t.rd(VID_FLIP), uint32_t(i));
    }
    CHECK_EQ(C, t.flip_cycles - pc0, 5);
    // back to back writes: one pulse cycle each
    for (int i = 0; i < 3; i++) {
        t.d->req_valid = 1; t.d->req_we = 1; t.d->req_addr = VID_FLIP; t.d->req_wstrb = 1; t.d->req_wdata = 0;
        t.tick();
    }
    t.d->req_valid = 0; t.idle(3);
    CHECK_EQ(C, t.flip_cycles - pc0, 8);
    CHECK_EQ(C, t.rd(VID_FLIP), 8);
    // reads and writes to other registers do not flip
    t.wr(VID_STATUS, 0xFFFFFFFF); t.wr(VID_PAL, 0x112233); t.wr(VID_FB, 0x01020304);
    t.idle(5);
    CHECK_EQ(C, t.flip_cycles - pc0, 8);
    CHECK_EQ(C, t.rd(VID_FLIP), 8);
    uint32_t st = t.rd(VID_STATUS);
#if TB_FAST
    CHECK_EQ(C, st, 1);
    t.idle(2000);
    CHECK_EQ(C, t.rd(VID_STATUS), 1);
    CHECK_EQ(C, t.d->vid_pix_ce, 0); CHECK_EQ(C, t.d->vid_hsync, 1); CHECK_EQ(C, t.d->vid_vsync, 1);
    CHECK_EQ(C, t.d->vid_de, 0); CHECK_EQ(C, t.d->vid_r | t.d->vid_g | t.d->vid_b, 0);
#else
    CHECK_EQ(C, (st >> 1) & 0x7FFF, 0);
    CHECK_EQ(C, t.rd(VID_STATUS) & 0xFFFE, st & 0xFFFE);
#endif
}

static void test_fb_pal(Tb& t) {
    std::puts("[framebuffer / palette]");
    Shadow s;
    // snapshot current arrays (earlier tests wrote a little)
    for (int l = 0; l < 4; l++) for (int i = 0; i < 16000; i++) s.fb[l][i] = fb_get(t, l, i);
    for (int i = 0; i < 256; i++) s.pal[i] = pal_get(t, i);
    CHECK_EQ(C, compare_arrays(t, s), 0);

    // first: clear everything with word stores
    for (int i = 0; i < 16000; i++) { t.d->req_valid = 1; t.d->req_we = 1; t.d->req_addr = VID_FB + 4 * i; t.d->req_wstrb = 0xF; t.d->req_wdata = 0; t.tick(); shadow_store(s, VID_FB + 4 * i, 0, 0xF); }
    for (int i = 0; i < 256; i++) { t.d->req_addr = VID_PAL + 4 * i; t.tick(); shadow_store(s, VID_PAL + 4 * i, 0, 0xF); }
    t.d->req_valid = 0; t.idle(2);
    CHECK_EQ(C, compare_arrays(t, s), 0);

    // all 16 strobe patterns on a handful of words, FB and palette, checked immediately
    const uint32_t fbw[] = {0, 1, 79, 80, 7999, 8000, 15998, 15999};
    for (uint32_t w : fbw) for (int strb = 0; strb < 16; strb++) {
        uint32_t data = t.rng.next() * 0x01000193u ^ t.rng.next();
        t.wr(VID_FB + 4 * w, data, strb); shadow_store(s, VID_FB + 4 * w, data, strb);
        for (int l = 0; l < 4; l++) CHECK_EQ(C, fb_get(t, l, w), s.fb[l][w]);
    }
    const uint32_t pw[] = {0, 1, 127, 128, 254, 255};
    for (uint32_t w : pw) for (int strb = 0; strb < 16; strb++) {
        uint32_t data = t.rng.next() * 0x01000193u ^ t.rng.next();
        t.wr(VID_PAL + 4 * w, data, strb); shadow_store(s, VID_PAL + 4 * w, data, strb);
        CHECK_EQ(C, pal_get(t, w), s.pal[w]);
    }
    // sb / sh / sw as the core issues them, at each byte offset
    t.wr(VID_FB + 0x100, 0xAB000000 >> 0, 0x8);  shadow_store(s, VID_FB + 0x100, 0xAB000000, 0x8);
    t.wr(VID_FB + 0x100, 0x0000CDEF, 0x3);        shadow_store(s, VID_FB + 0x100, 0x0000CDEF, 0x3);
    t.wr(VID_FB + 0x104, 0x12340000, 0xC);        shadow_store(s, VID_FB + 0x104, 0x12340000, 0xC);
    CHECK_EQ(C, fb_get(t, 3, 0x40), 0xAB); CHECK_EQ(C, fb_get(t, 0, 0x40), 0xEF); CHECK_EQ(C, fb_get(t, 1, 0x40), 0xCD);
    CHECK_EQ(C, fb_get(t, 2, 0x41), 0x34); CHECK_EQ(C, fb_get(t, 3, 0x41), 0x12);

    // loads from FB/palette read 0 and have no effect
    CHECK_EQ(C, t.rd(VID_FB + 0x100), 0);
    CHECK_EQ(C, t.rd(VID_PAL + 4), 0);

    // pseudo-random stores incl. boundary and unmapped addresses; shadow follows the spec
    const uint32_t unm[] = {0x1001FA00, 0x1001FA04, 0x1001FFFC, 0x10003800, 0x10003FFC, 0x10003008,
                            0x100033FC, 0x10004400, 0x10020000, 0x1000F400, 0x1001F9FC + 0x10000 /*0x10029..*/};
    for (int i = 0; i < 30000; i++) {
        uint32_t r = t.rng.next() % 100, addr;
        if (r < 55)      addr = VID_FB + 4 * (t.rng.next() % 16000);
        else if (r < 60) addr = VID_FB + 4 * (15990 + t.rng.next() % 12);   // straddle the end (words 15990..16001)
        else if (r < 85) addr = VID_PAL + 4 * (t.rng.next() % 256);
        else             addr = unm[t.rng.next() % (sizeof unm / sizeof unm[0])];
        uint8_t strb = (t.rng.next() % 3 == 0) ? 0xF : (t.rng.next() & 0xF);
        uint32_t data = t.rng.next() ^ (t.rng.next() << 16);
        t.d->req_valid = 1; t.d->req_we = 1; t.d->req_addr = addr; t.d->req_wstrb = strb; t.d->req_wdata = data;
        t.tick();
        shadow_store(s, addr, data, strb);
        if (t.rng.next() % 8 == 0) { t.d->req_valid = 0; t.d->req_we = 0; t.d->req_addr = VID_FB; t.d->req_wstrb = 0; t.tick(); }
    }
    t.d->req_valid = 0; t.idle(2);
    CHECK_EQ(C, compare_arrays(t, s), 0);
}

static void test_contract_misc(Tb& t) {
    std::puts("[contract]");
    // ready never dropped after reset; no accepted request produced ready=0
    CHECK_EQ(C, t.ready_low_cycles <= 6, 1);   // only the reset cycles at the start
    // a load of KBD_DATA held through a sequence of other idle cycles with wiggling inputs
    // is covered per cycle by Tb::tick (R5 wiggle and R3 hold checks run on every cycle)
    CHECK(C, t.accepts > 1000);
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Tb t;
    std::printf("tb_mmio FAST=%d UART_DIV=%d CLK_HZ=%d\n", TB_FAST, TB_UART_DIV, TB_CLK_HZ);
    t.d->clk = 0; t.d->rst = 1; t.d->req_valid = 0; t.d->key_valid = 0; t.d->eval();
    test_reset_and_sys(t);
    test_uart(t);
    test_kbd(t);
    test_video_regs(t);
    test_fb_pal(t);
    test_contract_misc(t);
    return C.finish();
}
