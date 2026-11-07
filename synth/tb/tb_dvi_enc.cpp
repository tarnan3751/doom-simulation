// End-to-end test of the DVI pixel path rtl/fpga/tn20k_dvi_enc.sv (clock-domain
// crossing FIFO + three TMDS encoders), with two independent clocks.
//
// System side: a raster generator that behaves like soc_top's video pins
// (800 x 525, 640 x 480 active, syncs active low, one pixel per two system
// cycles, vid_pix_ce in the first cycle of each pixel, random colours).
// Pixel side: the three symbol streams are decoded with an independent TMDS
// decoder (tmds_ref.h) and compared, pixel by pixel and in order, with what was
// written; control symbols must carry the sync levels (channel 0) / 00.
//
// Runs: clk_pix = clk_sys / 2 exactly with several phase offsets (must never
// slip, no pixel lost, duplicated or corrupted, over two frames), and a +-0.5 %
// pixel clock error (the FIFO must detect the drift and set `slip`).
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <random>
#include "Vtn20k_dvi_enc.h"
#include "verilated.h"
#include "tmds_ref.h"

struct Pixel { bool de, hs, vs; uint8_t r, g, b; };

struct Result { long checked = 0, errors = 0; bool slip = false; long first_valid_ps = -1; };

static Result run(long sys_ps, long pix_ps, long phase_ps, long n_pixels, unsigned seed, bool verbose) {
    Vtn20k_dvi_enc dut;
    std::mt19937 rng(seed);
    Result res;

    // raster generator state (system domain)
    int hcnt = 0, vcnt = 0, half = 0;
    Pixel cur{};
    std::deque<Pixel> written;
    long sys_cycles = 0, pix_cycles = 0;
    const long rst_sys_cycles = 8, rst_pix_cycles = 5;

    auto gen_next = [&]() {
        Pixel p;
        p.de = hcnt < 640 && vcnt < 480;
        p.hs = !(hcnt >= 656 && hcnt < 752);
        p.vs = !(vcnt >= 490 && vcnt < 492);
        p.r = p.de ? uint8_t(rng()) : 0;
        p.g = p.de ? uint8_t(rng()) : 0;
        p.b = p.de ? uint8_t(rng()) : 0;
        if (++hcnt == 800) { hcnt = 0; if (++vcnt == 525) vcnt = 0; }
        return p;
    };

    dut.clk_sys = 0; dut.clk_pix = 0; dut.rst_sys = 1; dut.rst_pix = 1;
    dut.vid_pix_ce = 0; dut.vid_hsync = 1; dut.vid_vsync = 1; dut.vid_de = 0;
    dut.vid_r = dut.vid_g = dut.vid_b = 0;
    dut.eval();

    // event times (ps) of the next rising edge of each clock; falling edges at +period/2
    long t_sys = sys_ps, t_pix = pix_ps + phase_ps;
    long t_sys_f = sys_ps / 2, t_pix_f = phase_ps + pix_ps / 2;
    long expect_idx = 0;  // index into `written` of the next pixel expected on the output
    bool started = false;

    while (res.checked < n_pixels) {
        long t = std::min(std::min(t_sys, t_pix), std::min(t_sys_f, t_pix_f));
        bool rs = (t == t_sys), rp = (t == t_pix);
        if (t == t_sys_f) { dut.clk_sys = 0; t_sys_f += sys_ps; }
        if (t == t_pix_f) { dut.clk_pix = 0; t_pix_f += pix_ps; }
        if (rs) dut.clk_sys = 1;
        if (rp) dut.clk_pix = 1;
        dut.eval();
        if (rs) {
            t_sys += sys_ps;
            sys_cycles++;
            // outputs of soc_top change right after its clock edge
            if (sys_cycles == rst_sys_cycles) dut.rst_sys = 0;
            if (!dut.rst_sys) {
                if (half == 0) { cur = gen_next(); written.push_back(cur); }
                dut.vid_pix_ce = (half == 0);
                dut.vid_de = cur.de; dut.vid_hsync = cur.hs; dut.vid_vsync = cur.vs;
                dut.vid_r = cur.r; dut.vid_g = cur.g; dut.vid_b = cur.b;
                half ^= 1;
            }
            dut.eval();
        }
        if (rp) {
            t_pix += pix_ps;
            pix_cycles++;
            if (pix_cycles == rst_pix_cycles) { dut.rst_pix = 0; dut.eval(); continue; }
            if (dut.slip) { res.slip = true; break; }
            if (!dut.sym_valid) {
                if (started) { res.errors++; if (verbose) fprintf(stderr, "symbol stream stopped\n"); break; }
                continue;
            }
            if (!started) { started = true; res.first_valid_ps = t; }
            if (expect_idx >= long(written.size())) { res.errors++; break; }
            const Pixel& e = written[size_t(expect_idx++)];
            uint8_t d0, d1, d2; int c0, c1, c2; bool b0, b1, b2;
            const bool x0 = tmds::decode(uint16_t(dut.sym0), &d0, &c0, &b0);
            const bool x1 = tmds::decode(uint16_t(dut.sym1), &d1, &c1, &b1);
            const bool x2 = tmds::decode(uint16_t(dut.sym2), &d2, &c2, &b2);
            bool ok;
            if (e.de) ok = x0 && x1 && x2 && d0 == e.b && d1 == e.g && d2 == e.r;
            else      ok = !x0 && !x1 && !x2 && c0 == ((e.vs ? 2 : 0) | (e.hs ? 1 : 0)) && c1 == 0 && c2 == 0;
            if (!ok) {
                if (verbose && res.errors < 5)
                    fprintf(stderr, "pixel %ld: expected de=%d hs=%d vs=%d rgb=%02x%02x%02x, got sym %03x %03x %03x\n",
                            expect_idx - 1, e.de, e.hs, e.vs, e.r, e.g, e.b,
                            unsigned(dut.sym0), unsigned(dut.sym1), unsigned(dut.sym2));
                res.errors++;
            }
            res.checked++;
            // keep the history bounded
            while (expect_idx > 64) { written.pop_front(); expect_idx--; }
        }
    }
    return res;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    const long frame = 800L * 525L;
    const long sys_ps = 20000, pix_ps = 40000;     // exact 2:1 (absolute period irrelevant for RTL)
    bool all_ok = true;

    // locked clocks, several phase offsets (including coincident edges)
    const long phases[] = {0, 1, 5000, 9999, 10000, 19999, 20000, 20001, 30000, 39999};
    unsigned seed = 1;
    for (long ph : phases) {
        Result r = run(sys_ps, pix_ps, ph, 2 * frame, seed++, true);
        const bool ok = r.errors == 0 && !r.slip && r.checked == 2 * frame;
        printf("  locked, pixel-clock phase %5ld ps: %ld pixels checked, %ld errors, slip %d -> %s\n",
               ph, r.checked, r.errors, r.slip, ok ? "ok" : "FAIL");
        all_ok &= ok;
    }
    // frequency error: the FIFO must slip (and report it) instead of silently corrupting
    for (long dp : {200L, -200L}) {
        Result r = run(sys_ps, pix_ps + dp, 12345, 2 * frame, seed++, false);
        const bool ok = r.slip && r.errors == 0;
        printf("  pixel clock %+.2f %%: slip after %ld good pixels, %ld errors -> %s (slip expected)\n",
               100.0 * double(dp) / double(pix_ps), r.checked, r.errors, ok ? "ok" : "FAIL");
        all_ok &= ok;
    }
    printf("tb_dvi_enc: %s\n", all_ok ? "PASS" : "FAIL");
    return all_ok ? 0 : 1;
}
