// soc_top with behavioural stub core / memsys: data-port split, response ownership,
// rsp_ready combination, and the 64-bit event counters.
#include <memory>
#include <vector>
#include "Vsoc.h"
#include "Vsoc___024root.h"
#include "verilated.h"
#include "tb_common.h"
#include "uart_decoder.h"

#ifndef TB_FAST
#define TB_FAST 0
#endif
#define UART_DIV_T 16

static Checker C("tb_soc");

struct Tb {
    std::unique_ptr<Vsoc> d{new Vsoc};
    Vsoc___024root& r = *d->rootp;
    UartDecoder uart{UART_DIV_T};
    uint64_t cyc = 0, edges_run = 0;
    // master-side contract tracking
    bool outstanding = false, have_comp = false;
    uint32_t comp_rdata = 0;
    uint64_t flip_seen = 0;

#define CORE(n) r.soc_top__DOT__u_core__DOT__##n
#define MEM(n) r.soc_top__DOT__u_mem__DOT__##n

    void tick() {
        uart.tick(d->uart_tx);
        d->eval();
        bool acc = CORE(mon_valid);
        if (acc) CHECK_EQ(C, CORE(mon_ready), 1);          // request only when ready (master rule)
        d->clk = 1; d->eval(); d->clk = 0; d->eval();
        ++cyc;
        if (!d->rst) ++edges_run;
        if (d->vid_flip) ++flip_seen;
        if (d->rst) { outstanding = have_comp = false; return; }
        if (acc) { outstanding = true; have_comp = false; }
        if (outstanding && CORE(mon_ready) && !have_comp) { have_comp = true; comp_rdata = CORE(mon_rdata); }
        else if (have_comp) {                              // R3: stable until the next accept
            CHECK_EQ(C, CORE(mon_ready), 1);
            CHECK_EQ(C, CORE(mon_rdata), comp_rdata);
        }
    }
    void idle(int n) { for (int i = 0; i < n; i++) tick(); }

    // One data access. Returns rsp_rdata of the completion; `lows` = cycles rsp_ready was 0 after the accept.
    uint32_t access(uint32_t addr, bool we, uint32_t data, uint8_t strb, int* lows = nullptr) {
        CORE(cmd_addr) = addr; CORE(cmd_we) = we; CORE(cmd_wdata) = data; CORE(cmd_wstrb) = strb;
        CORE(cmd_pending) = 1;
        int guard = 0;
        while (CORE(cmd_pending)) { tick(); if (++guard > 1000) { CHECK(C, false); break; } }
        int low = 0;
        while (!CORE(mon_ready)) { ++low; tick(); if (low > 1000) { CHECK(C, false); break; } }
        if (lows) *lows = low;
        return CORE(mon_rdata);
    }
    uint32_t rd(uint32_t a, int* lows = nullptr) { return access(a, false, 0, 0, lows); }
    void wr(uint32_t a, uint32_t v, uint8_t strb = 0xF, int* lows = nullptr) { access(a, true, v, strb, lows); }

    void reset(int init_delay = 0) {
        MEM(init_delay) = init_delay;
        d->rst = 1;
        for (int i = 0; i < 3; i++) tick();
        d->rst = 0;
        edges_run = 0;
    }
};

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Tb t;
    std::printf("tb_soc FAST=%d\n", TB_FAST);
    t.d->clk = 0; t.d->key_valid = 0; t.d->sdram_dq_i = 0;
    for (int i = 0; i < 1024; i++) t.MEM(mem)[i] = 0x1000 + i;
    t.MEM(lat) = 0;

    // ---- reset with a slow memory subsystem: the core must wait for BOTH slaves ------------
    std::puts("[reset / init delay]");
    t.reset(25);
    {
        int low_cycles = 0;
        // present an MMIO read immediately; it may only be accepted once memsys is ready
        t.CORE(cmd_addr) = 0x10001000; t.CORE(cmd_we) = 0; t.CORE(cmd_pending) = 1;
        while (t.CORE(cmd_pending)) { t.tick(); if (++low_cycles > 200) break; }
        CHECK(C, low_cycles >= 24);                   // waited for memsys although mmio itself is ready
        CHECK(C, low_cycles < 200);
        CHECK_EQ(C, t.MEM(d_req_seen_when_not_ready), 0);
        t.tick();
        CHECK_EQ(C, t.CORE(mon_rdata), 50000000u);    // SYS_CLK_HZ default
    }
    t.reset(0);

    t.uart = UartDecoder(UART_DIV_T);           // forget pin glitches from before the first reset
    // ---- routing --------------------------------------------------------------------------
    std::puts("[routing: RAM vs MMIO]");
    // RAM, zero latency: completion in the cycle after the accept
    int lows = -1;
    t.wr(RAM + 0x10, 0xDEADBEEF, 0xF, &lows);   CHECK_EQ(C, lows, 0);
    CHECK_EQ(C, t.rd(RAM + 0x10, &lows), 0xDEADBEEFu); CHECK_EQ(C, lows, 0);
    CHECK_EQ(C, t.MEM(d_acc_count), 2);
    // sub-word strobes pass through unchanged
    t.wr(RAM + 0x14, 0x11223344, 0xF);
    t.wr(RAM + 0x14, 0xAA00BB00, 0xA);
    CHECK_EQ(C, t.rd(RAM + 0x14), 0xAA22BB44u);
    CHECK_EQ(C, t.MEM(d_acc_count), 5);
    // MMIO never reaches memsys; the response comes from mmio
    uint32_t acc0 = t.MEM(d_acc_count);
    CHECK_EQ(C, t.rd(SYS_CLK_HZ), 50000000u);
    CHECK_EQ(C, t.rd(SYS_FLAGS), TB_FAST);
    CHECK_EQ(C, t.rd(UART_STATUS), 1);
    CHECK_EQ(C, t.rd(0x7FFFFFFC), 0);             // unmapped, bit 31 clear -> MMIO side
    CHECK_EQ(C, t.rd(0x00000000), 0);
    CHECK_EQ(C, t.MEM(d_acc_count), acc0);
    // RAM again; the held MMIO response must not leak into it
    CHECK_EQ(C, t.rd(RAM + 0x10), 0xDEADBEEFu);
    CHECK_EQ(C, t.rd(0xFFFFFFFC), t.MEM(mem)[0x3FF]);      // top of the address space is RAM too
    t.idle(30);

    // wait states: ready low for exactly `lat` cycles after the accept, data correct, held
    std::puts("[RAM latency, ownership of the held response]");
    for (int lat : {1, 2, 5, 17}) {
        t.MEM(lat) = lat;
        t.wr(RAM + 0x40, 0xC0DE0000 + lat, 0xF, &lows);   CHECK_EQ(C, lows, lat);
        CHECK_EQ(C, t.rd(RAM + 0x40, &lows), 0xC0DE0000u + lat); CHECK_EQ(C, lows, lat);
        // MMIO right after: ready must be back to 1 immediately, data from MMIO
        CHECK_EQ(C, t.rd(SYS_CLK_HZ, &lows), 50000000u);   CHECK_EQ(C, lows, 0);
        t.idle(40);                                         // tick() checks rdata stays stable
        CHECK_EQ(C, t.rd(RAM + 0x40, &lows), 0xC0DE0000u + lat);
        t.idle(40);
        CHECK_EQ(C, t.rd(UART_STATUS, &lows), 1);          CHECK_EQ(C, lows, 0);
    }
    // while a RAM request is outstanding the core sees ready=0 (the idle MMIO side does not mask it)
    t.MEM(lat) = 9;
    uint32_t pre = t.MEM(d_acc_count);
    t.rd(RAM + 0x44, &lows);
    CHECK_EQ(C, lows, 9);
    CHECK_EQ(C, t.MEM(d_acc_count), pre + 1);
    t.MEM(lat) = 0;

    // exactly-once side effects through the whole path
    std::puts("[MMIO side effects through soc_top]");
    t.uart.clear();
    t.wr(UART_TX, 'D');
    t.idle(11 * UART_DIV_T);
    CHECK_EQ(C, t.uart.count(), 1);
    if (t.uart.count()) CHECK_EQ(C, t.uart.bytes()[0], 'D');
    CHECK_EQ(C, t.uart.framing_errors(), 0);
    // uart_tx_valid pulse seen at soc_top
    t.wr(UART_TX, 'o');
    CHECK_EQ(C, t.d->uart_tx_valid, 1);
    CHECK_EQ(C, t.d->uart_tx_byte, 'o');
    t.idle(11 * UART_DIV_T);
    CHECK_EQ(C, t.uart.count(), 2);
    // keyboard: pop exactly once even though the response is held for a long time
    t.d->key_valid = 1; t.d->key_data = 0x141; t.tick();
    t.d->key_data = 0x042; t.tick();
    t.d->key_valid = 0;
    CHECK_EQ(C, t.rd(KBD_DATA), 0x80000141u);
    t.idle(100);
    CHECK_EQ(C, t.rd(KBD_STATUS), 1);
    CHECK_EQ(C, t.rd(KBD_DATA), 0x80000042u);
    CHECK_EQ(C, t.rd(KBD_DATA), 0);
    // video control + framebuffer reach mmio
    uint64_t fl0 = t.flip_seen;
    t.wr(VID_FLIP, 0);
    t.wr(VID_FLIP, 0);
    CHECK_EQ(C, t.rd(VID_FLIP), 2);
    CHECK_EQ(C, t.flip_seen - fl0, 2);
    t.wr(VID_PAL + 8, 0x00AABBCC);
    t.wr(VID_FB + 4, 0x04030201);
    CHECK_EQ(C, t.r.soc_top__DOT__u_mmio__DOT__u_video__DOT__palette[2], 0xAABBCCu);
    CHECK_EQ(C, t.r.soc_top__DOT__u_mmio__DOT__u_video__DOT__fb0[1], 0x01);
    CHECK_EQ(C, t.r.soc_top__DOT__u_mmio__DOT__u_video__DOT__fb1[1], 0x02);
    CHECK_EQ(C, t.r.soc_top__DOT__u_mmio__DOT__u_video__DOT__fb2[1], 0x03);
    CHECK_EQ(C, t.r.soc_top__DOT__u_mmio__DOT__u_video__DOT__fb3[1], 0x04);
    t.wr(SYS_EXIT, 42);
    CHECK_EQ(C, t.d->halted, 1);
    CHECK_EQ(C, t.d->exit_code, 42u);
    // video pins: idle in FAST, alive otherwise
#if TB_FAST
    t.idle(3000);
    CHECK_EQ(C, t.d->vid_pix_ce, 0); CHECK_EQ(C, t.d->vid_hsync, 1); CHECK_EQ(C, t.d->vid_vsync, 1); CHECK_EQ(C, t.d->vid_de, 0);
#else
    { int ce = 0; for (int i = 0; i < 3000; i++) { t.tick(); ce += t.d->vid_pix_ce; } CHECK(C, ce >= 1400 && ce <= 1500); }
#endif

    // ---- counters ----------------------------------------------------------------------------
    std::puts("[event counters]");
    t.reset(0);
    t.MEM(lat) = 0;
    CHECK_EQ(C, t.d->cnt_cycle, 0u);
    CHECK_EQ(C, t.d->cnt_instret | t.d->cnt_ic_access | t.d->cnt_ic_miss | t.d->cnt_dc_access | t.d->cnt_dc_miss |
                t.d->cnt_branch | t.d->cnt_branch_miss | t.d->cnt_jump | t.d->cnt_jump_miss | t.d->cnt_flip, 0u);
    struct { uint64_t instret = 0, ic = 0, icm = 0, dc = 0, dcm = 0, br = 0, brm = 0, j = 0, jm = 0, fl = 0; } e;
    Rng rng(99);
    for (int i = 0; i < 400; i++) {
        uint32_t v = rng.next();
        int tv = v & 1, trap = (v >> 1) & 1, br = (v >> 2) & 1, brm = (v >> 3) & 1, j = (v >> 4) & 1,
            jm = (v >> 5) & 1, icm = (v >> 6) & 1, itch = (v >> 7) & 1;
        t.CORE(cmd_trace_valid) = tv; t.CORE(cmd_trace_trap) = trap; t.CORE(cmd_branch) = br;
        t.CORE(cmd_branch_miss) = brm; t.CORE(cmd_jump) = j; t.CORE(cmd_jump_miss) = jm;
        t.CORE(cmd_ifetch) = itch; t.MEM(ctl_ic_miss) = icm;
        t.tick();
        t.CORE(cmd_trace_valid) = t.CORE(cmd_trace_trap) = t.CORE(cmd_branch) = t.CORE(cmd_branch_miss) = 0;
        t.CORE(cmd_jump) = t.CORE(cmd_jump_miss) = 0; t.CORE(cmd_ifetch) = 0; t.MEM(ctl_ic_miss) = 0;
        e.instret += tv && !trap; e.br += br; e.brm += brm; e.j += j; e.jm += jm; e.ic += itch; e.icm += icm;
        if (i % 40 == 0) {                           // interleave bus traffic
            t.MEM(lat) = (i / 40) % 3 == 0 ? 0 : 3;
            t.rd(RAM + 4 * (i % 64)); e.dc++; if (t.MEM(lat)) e.dcm++;
            t.wr(RAM + 4 * (i % 64), i); e.dc++; if (t.MEM(lat)) e.dcm++;
            t.rd(SYS_CLK_HZ);                        // MMIO: not counted
            t.wr(VID_FLIP, 0); e.fl++;
            t.wr(VID_PAL, 0);
        }
    }
    t.MEM(lat) = 0;
    t.idle(5);
    CHECK_EQ(C, t.d->cnt_cycle, t.edges_run);
    CHECK_EQ(C, t.d->cnt_instret, e.instret);
    CHECK_EQ(C, t.d->cnt_ic_access, e.ic);
    CHECK_EQ(C, t.d->cnt_ic_miss, e.icm);
    CHECK_EQ(C, t.d->cnt_dc_access, e.dc);
    CHECK_EQ(C, t.d->cnt_dc_miss, e.dcm);
    CHECK_EQ(C, t.d->cnt_branch, e.br);
    CHECK_EQ(C, t.d->cnt_branch_miss, e.brm);
    CHECK_EQ(C, t.d->cnt_jump, e.j);
    CHECK_EQ(C, t.d->cnt_jump_miss, e.jm);
    CHECK_EQ(C, t.d->cnt_flip, e.fl);
    std::printf("  events: instret %llu ic %llu/%llu dc %llu/%llu br %llu/%llu jmp %llu/%llu flips %llu cycles %llu\n",
        (unsigned long long)e.instret, (unsigned long long)e.ic, (unsigned long long)e.icm, (unsigned long long)e.dc,
        (unsigned long long)e.dcm, (unsigned long long)e.br, (unsigned long long)e.brm, (unsigned long long)e.j,
        (unsigned long long)e.jm, (unsigned long long)e.fl, (unsigned long long)t.d->cnt_cycle);
    // 64-bit carry
    t.r.soc_top__DOT__cnt_cycle = 0xFFFFFFFEull; t.r.soc_top__DOT__cnt_instret = 0xFFFFFFFFull;   // internal storage (--public-flat-rw)
    t.CORE(cmd_trace_valid) = 1; t.CORE(cmd_trace_trap) = 0;
    t.tick();
    t.CORE(cmd_trace_valid) = 0;
    CHECK_EQ(C, t.d->cnt_cycle, 0xFFFFFFFFull);
    CHECK_EQ(C, t.d->cnt_instret, 0x100000000ull);
    t.tick();
    CHECK_EQ(C, t.d->cnt_cycle, 0x100000000ull);
    CHECK_EQ(C, t.d->cnt_instret, 0x100000000ull);
    return C.finish();
}
