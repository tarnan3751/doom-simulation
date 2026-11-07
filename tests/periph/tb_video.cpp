// Scan-out testbench: mmio (FAST=0) + VideoMonitor. Compares complete 640x480 frames pixel
// by pixel against an independently computed expected image, checks the timing and the
// VID_STATUS view of the scan-out.
#include <algorithm>
#include <memory>
#include <vector>
#include "Vmmio.h"
#include "Vmmio___024root.h"
#include "verilated.h"
#include "tb_common.h"
#include "video_monitor.h"

static const uint64_t FRAME_CYC = 800ull * 525 * 2, LINE_CYC = 800ull * 2;

static Checker C("tb_video");

struct Vt {
    std::unique_ptr<Vmmio> d{new Vmmio};
    VideoMonitor mon;
    uint64_t cyc = 0;
    std::vector<uint64_t> vs_falls, hs_falls;
    bool pvs = true, phs = true;
    uint64_t ce_count = 0;

    void tick() {
        d->clk = 1; d->eval(); d->clk = 0; d->eval();
        ++cyc;
        mon.tick(d->vid_pix_ce, d->vid_hsync, d->vid_vsync, d->vid_de, d->vid_r, d->vid_g, d->vid_b);
        if (d->vid_pix_ce) ++ce_count;
        if (pvs && !d->vid_vsync) vs_falls.push_back(cyc);
        if (phs && !d->vid_hsync) hs_falls.push_back(cyc);
        pvs = d->vid_vsync; phs = d->vid_hsync;
    }
    void run_until(uint64_t c) { d->req_valid = 0; while (cyc < c) tick(); }
    uint32_t rd(uint32_t a) {
        d->req_valid = 1; d->req_we = 0; d->req_addr = a; d->req_wstrb = 0; d->req_wdata = 0;
        tick(); d->req_valid = 0;
        return d->rsp_rdata;
    }
    void wr(uint32_t a, uint32_t v, uint8_t strb = 0xF) {
        d->req_valid = 1; d->req_we = 1; d->req_addr = a; d->req_wstrb = strb; d->req_wdata = v;
        tick(); d->req_valid = 0;
    }
};

struct Image { uint8_t fb[320 * 200]; uint32_t pal[256]; };

static void program(Vt& t, const Image& im, bool fb, bool pal, uint32_t variant) {
    if (pal) for (int i = 0; i < 256; i++) t.wr(VID_PAL + 4 * i, im.pal[i]);
    if (fb) {
        for (int w = 0; w < 16000; w++) {
            const uint8_t* p = &im.fb[4 * w];
            uint32_t v = p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24;
            uint32_t a = VID_FB + 4 * w;
            if (variant && w % 7 == 0)        for (int l = 0; l < 4; l++) t.wr(a + l, v & (0xFFu << (8 * l)), 1 << l);  // sb
            else if (variant && w % 11 == 0) { t.wr(a, v & 0xFFFF, 0x3); t.wr(a + 2, v & 0xFFFF0000u, 0xC); }          // sh
            else                              t.wr(a, v);
        }
    }
}

static uint32_t expect_px(const Image& im, int x, int y) {
    if (y < 40 || y >= 440) return 0;
    return im.pal[im.fb[((y - 40) / 2) * 320 + x / 2]] & 0xFFFFFF;
}

static void check_frame(Vt& t, const Image& im, const char* what) {
    // wait for a frame that started after all writes: two more complete frames
    uint64_t f0 = t.mon.frames();
    while (t.mon.frames() < f0 + 2) t.tick();
    const auto& fr = t.mon.frame();
    uint64_t bad = 0;
    for (int y = 0; y < 480; y++) for (int x = 0; x < 640; x++) {
        uint32_t e = expect_px(im, x, y), g = fr[size_t(y) * 640 + x];
        if (e != g) { if (bad < 5) std::printf("  %s: pixel (%d,%d) got %06x want %06x\n", what, x, y, g, e); bad++; }
    }
    std::printf("  %s: %d pixels compared, %llu mismatches\n", what, 640 * 480, (unsigned long long)bad);
    C.checks += 640 * 480; C.fails += bad;
    // the public arrays hold exactly what we wrote
    uint64_t abad = 0;
    auto& r = *t.d->rootp;
    for (int w = 0; w < 16000; w++) for (int l = 0; l < 4; l++) if (fb_byte(r, l, w) != im.fb[4 * w + l]) abad++;
    for (int i = 0; i < 256; i++) if (pal_rgb(r, i) != (im.pal[i] & 0xFFFFFF)) abad++;
    CHECK_EQ(C, abad, 0);
}

static Image make_random(Rng& rng) {
    Image im;
    for (int i = 0; i < 256; i++) im.pal[i] = (rng.next() & 0xFFFFFF);
    im.pal[0] = 0x123456;                      // non-black index 0: the letterbox must still be black
    for (int i = 0; i < 320 * 200; i++) im.fb[i] = rng.next() & 0xFF;
    // distinct corners, edges and bands
    im.fb[0] = 1; im.fb[319] = 2; im.fb[199 * 320] = 3; im.fb[199 * 320 + 319] = 4;
    for (int x = 1; x < 319; x++) { im.fb[x] = 10 + (x & 3); im.fb[199 * 320 + x] = 20 + (x & 3); }
    for (int y = 1; y < 199; y++) { im.fb[y * 320] = 30 + (y & 3); im.fb[y * 320 + 319] = 40 + (y & 3); }
    im.pal[1] = 0xFF0000; im.pal[2] = 0x00FF00; im.pal[3] = 0x0000FF; im.pal[4] = 0xFFFFFF;
    return im;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vt t;
    Rng rng(777);
    t.d->clk = 0; t.d->rst = 1; t.d->req_valid = 0; t.d->key_valid = 0; t.d->eval();
    for (int i = 0; i < 4; i++) t.tick();
    t.d->rst = 0;
    t.tick();

    // ---- before anything is programmed the screen must be well-formed already --------------
    std::puts("[scenario A: random fill, doubled + letterboxed]");
    Image A = make_random(rng);
    program(t, A, true, true, 1);                // mixes sb / sh / sw stores
    check_frame(t, A, "A");

    std::puts("[scenario B: palette change only]");
    Image B = A;
    for (int i = 0; i < 256; i++) B.pal[i] = (rng.next() & 0xFFFFFF);
    B.pal[0] = 0xABCDEF;
    program(t, B, false, true, 0);
    check_frame(t, B, "B");

    std::puts("[scenario C: framebuffer change only (corners + a band + sub-word stores)]");
    Image Cc = B;
    for (int i = 0; i < 320 * 200; i++) if ((i / 320) >= 90 && (i / 320) < 110) Cc.fb[i] = uint8_t(i * 7 + 3);
    Cc.fb[0] = 9; Cc.fb[319] = 8; Cc.fb[199 * 320] = 7; Cc.fb[199 * 320 + 319] = 6;
    program(t, Cc, true, false, 1);
    check_frame(t, Cc, "C");

    std::puts("[scenario D: everything black / all-same index]");
    Image D;
    for (int i = 0; i < 256; i++) D.pal[i] = 0x010101 * i;
    for (int i = 0; i < 320 * 200; i++) D.fb[i] = 255;
    program(t, D, true, true, 0);
    check_frame(t, D, "D");

    // ---- timing ----------------------------------------------------------------------------
    std::puts("[timing]");
    const auto& m = t.mon.measured();
    std::printf("  measured: ce %u clk; h: total %u active %u fp %u sync %u bp %u; v: total %u active %u fp %u sync %u bp %u\n",
                m.ce_period, m.h_total, m.h_active, m.h_fp, m.h_sync, m.h_bp, m.v_total, m.v_active, m.v_fp, m.v_sync, m.v_bp);
    CHECK_EQ(C, m.ce_period, 2);
    CHECK_EQ(C, m.h_total, 800); CHECK_EQ(C, m.h_active, 640); CHECK_EQ(C, m.h_fp, 16);
    CHECK_EQ(C, m.h_sync, 96);   CHECK_EQ(C, m.h_bp, 48);
    CHECK_EQ(C, m.v_total, 525); CHECK_EQ(C, m.v_active, 480); CHECK_EQ(C, m.v_fp, 10);
    CHECK_EQ(C, m.v_sync, 2);    CHECK_EQ(C, m.v_bp, 33);
    for (const auto& e : t.mon.error_log()) std::printf("  monitor: %s\n", e.c_str());
    CHECK_EQ(C, t.mon.errors(), 0);
    // independent cycle-level checks from the raw pins
    bool vs_ok = t.vs_falls.size() > 3, hs_ok = t.hs_falls.size() > 100;
    for (size_t i = 1; i < t.vs_falls.size(); i++) vs_ok &= (t.vs_falls[i] - t.vs_falls[i - 1] == FRAME_CYC);
    for (size_t i = 1; i < t.hs_falls.size(); i++) hs_ok &= (t.hs_falls[i] - t.hs_falls[i - 1] == LINE_CYC);
    CHECK(C, vs_ok);
    CHECK(C, hs_ok);
    CHECK(C, t.ce_count * 2 + 16 >= t.cyc && t.ce_count * 2 <= t.cyc);   // a pixel enable every other clock
    std::printf("  %llu frames, %llu cycles, %zu vsyncs, %zu hsyncs\n", (unsigned long long)t.mon.frames(),
                (unsigned long long)t.cyc, t.vs_falls.size(), t.hs_falls.size());

    // ---- VID_STATUS ------------------------------------------------------------------------
    std::puts("[VID_STATUS vs the monitor's view]");
    {
        // P = a vsync falling edge seen on the pins; the RTL frame counter ticks 5 cycles earlier
        size_t k = t.vs_falls.size() - 1;
        uint64_t P = t.vs_falls[k] + 2 * FRAME_CYC;
        t.run_until(P - 6 - 20);
        // frame counter (RTL counts vsync starts since reset: vs_falls[k] is the (k+1)th)
        uint64_t n = k + 3;                         // vs_falls[k] is the (k+1)th vsync; P is the (k+3)th
        t.run_until(P - 6);
        uint32_t s0 = t.rd(VID_STATUS);             // presented in cycle P-6: before the increment
        uint32_t s1 = t.rd(VID_STATUS);             // presented in cycle P-5: after
        CHECK_EQ(C, s0 >> 16, n - 1);
        CHECK_EQ(C, s1 >> 16, n);
        CHECK_EQ(C, s0 & 0xFFFE, 0); CHECK_EQ(C, s1 & 0xFFFE, 0);
        // the monitor's frame count has caught up 5 cycles later (it publishes at the vsync edge)
        uint64_t f_before = t.mon.frames();
        t.run_until(P - 1); CHECK_EQ(C, t.mon.frames(), f_before);
        t.run_until(P + 1); CHECK_EQ(C, t.mon.frames(), f_before + 1);
        CHECK_EQ(C, (t.mon.frames() + 1) & 0xFFFF, n);      // frames() excludes the very first (partial) one

        // bit0: 0 exactly while the scan-out counters are inside lines 40..439 (5 cycles ahead of the pins)
        auto pin_line_start = [&](int r) { return P + uint64_t((r + 525 - 490) % 525) * LINE_CYC + 2 * FRAME_CYC; };
        struct Pt { int line; int off; uint32_t want; };
        // off in cycles relative to the first cycle the pins show that raster line (S0 is 5 cycles ahead)
        const Pt pts[] = {
            {0, 800, 1}, {39, 800, 1}, {40, -6, 1}, {40, -5, 0}, {40, 800, 0}, {41, 800, 0}, {200, 800, 0},
            {439, 800, 0}, {440, -6, 0}, {440, -5, 1}, {441, 800, 1}, {479, 800, 1}, {489, 800, 1},
            {490, 800, 1}, {524, 800, 1}, {1, 3, 1}, {100, 0, 0}, {300, 1599, 0},
        };
        // run in time order (lines increase from 490 around): sort by absolute cycle
        std::vector<std::pair<uint64_t, uint32_t>> evs;
        for (const auto& p : pts) evs.push_back({pin_line_start(p.line) + p.off - 0, p.want});
        std::sort(evs.begin(), evs.end());
        uint64_t base = t.cyc;
        for (auto& e : evs) {
            uint64_t c = e.first;
            while (c < base) c += FRAME_CYC;       // next occurrence
            t.run_until(c);
            uint32_t s = t.rd(VID_STATUS);
            CHECK_EQ(C, s & 1, e.second);
            base = t.cyc;
        }
    }

    // ---- FIFO of flips does not disturb scan-out ----------------------------------------------
    t.wr(VID_FLIP, 1);
    t.run_until(t.cyc + 5000);
    CHECK_EQ(C, t.rd(VID_FLIP), 1);
    CHECK_EQ(C, t.mon.errors(), 0);
    return C.finish();
}
