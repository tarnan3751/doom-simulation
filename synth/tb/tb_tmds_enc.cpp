// Unit test of rtl/fpga/tn20k_tmds_enc.sv against the DVI 1.0 reference algorithm
// (tmds_ref.h). Checks, for every cycle, that the 10-bit output equals the
// reference bit for bit, that it decodes back to the input (data or control),
// and that all reachable (running disparity, data byte) pairs were exercised.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <random>
#include <set>
#include <utility>
#include "Vtn20k_tmds_enc.h"
#include "verilated.h"
#include "tmds_ref.h"

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    const long n_cycles = (argc > 1) ? atol(argv[1]) : 3000000;
    const unsigned seed = (argc > 2) ? unsigned(atoi(argv[2])) : 1;
    std::mt19937 rng(seed);

    Vtn20k_tmds_enc dut;
    tmds::Encoder ref;

    auto tick = [&]() { dut.clk = 1; dut.eval(); dut.clk = 0; dut.eval(); };

    dut.rst = 1; dut.de = 0; dut.d = 0; dut.c0 = 0; dut.c1 = 0; dut.clk = 0; dut.eval();
    for (int i = 0; i < 4; i++) tick();
    dut.rst = 0;

    long errors = 0, data_syms = 0, ctl_syms = 0, decode_errors = 0;
    int run_left = 0, mode = 0;
    bool de = false;
    uint8_t ramp = 0;
    long disp = 0, max_disp = 0;                 // running disparity of the transmitted bits, data periods
    std::set<std::pair<int, int>> seen;          // (reference cnt before the symbol, data byte)
    std::set<int> cnt_values;

    for (long t = 0; t < n_cycles; t++) {
        // stimulus: alternating DE runs of random length, several data pattern modes
        if (run_left == 0) {
            de = !de;
            const int r = int(rng() % 100);
            run_left = (r < 50) ? 1 + int(rng() % 16) : (r < 90) ? 1 + int(rng() % 800) : 1 + int(rng() % 5000);
            mode = int(rng() % 6);
        }
        run_left--;
        uint8_t d;
        switch (mode) {
            case 0: d = uint8_t(rng()); break;                                  // random
            case 1: d = 0x00; break;
            case 2: d = 0xFF; break;
            case 3: d = (rng() & 1) ? 0x55 : 0xAA; break;
            case 4: d = ramp++; break;                                          // ramp
            default: d = uint8_t(1u << (rng() % 8)) ^ uint8_t((rng() & 1) ? 0xFF : 0); break;  // one/seven bits
        }
        // coverage direction: in half of the data symbols, pick a byte not yet seen
        // with the current running disparity (the extreme disparities are rare)
        if (de && (rng() & 1)) {
            for (int k = 0, b0 = int(rng() & 0xFF); k < 256; k++) {
                const int b = (b0 + k) & 0xFF;
                if (!seen.count({ref.cnt, b})) { d = uint8_t(b); break; }
            }
        }
        const bool c0 = rng() & 1, c1 = rng() & 1;
        dut.de = de; dut.d = d; dut.c0 = c0; dut.c1 = c1;

        const int cnt_before = ref.cnt;
        const uint16_t exp = ref.encode(de, d, c0, c1);
        tick();
        const uint16_t got = uint16_t(dut.q);

        if (got != exp) {
            if (errors < 10)
                fprintf(stderr, "cycle %ld: de=%d d=%02x c=%d%d cnt=%d: got %03x expected %03x\n",
                        t, de, d, c1, c0, cnt_before, got, exp);
            errors++;
        }
        uint8_t dd; int ctl; bool bad;
        const bool is_data = tmds::decode(got, &dd, &ctl, &bad);
        if (de) {
            data_syms++;
            seen.insert({cnt_before, d});
            cnt_values.insert(ref.cnt);
            if (!is_data || dd != d) decode_errors++;
            int ones = 0;
            for (int i = 0; i < 10; i++) ones += (got >> i) & 1;
            disp += 2 * ones - 10;
            if (labs(disp) > max_disp) max_disp = labs(disp);
        } else {
            ctl_syms++;
            disp = 0;
            if (is_data || ctl != ((c1 ? 2 : 0) | (c0 ? 1 : 0))) decode_errors++;
        }
    }

    // every (cnt, byte) pair the reference can reach from cnt values it actually took
    long missing = 0;
    for (int c : cnt_values) for (int b = 0; b < 256; b++) if (!seen.count({c, b})) missing++;
    int cmin = *cnt_values.begin(), cmax = *cnt_values.rbegin();

    printf("tb_tmds_enc: %ld cycles (%ld data, %ld control symbols), seed %u\n", n_cycles, data_syms, ctl_syms, seed);
    printf("  mismatches vs reference: %ld, decode errors: %ld\n", errors, decode_errors);
    printf("  reference running disparity range: [%d, %d] (RTL counter is 5-bit signed)\n", cmin, cmax);
    printf("  (cnt, byte) pairs exercised: %zu, unexercised among reachable cnt values: %ld\n", seen.size(), missing);
    printf("  max |running disparity| of the sent bits within a data period: %ld\n", max_disp);
    const bool ok = errors == 0 && decode_errors == 0 && missing == 0 && cmin >= -16 && cmax <= 15;
    printf("tb_tmds_enc: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
