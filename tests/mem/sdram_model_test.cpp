// Directed unit test of the SDRAM chip model (sim/models/sdram_model.*).
//
// 1. Legal traffic must produce zero violations and correct data: init
//    sequence, bursts with/without auto-precharge, wrap order, interleaved
//    order, DQM write/read masking, open-page reads, a long run with periodic
//    refresh, read latency exactly CL, backdoor visibility.
// 2. Every checked rule is violated once on a fresh, initialised model and
//    the violation must be reported under the right kind (proves the
//    checker; the RTL bench additionally injects pin faults end to end).
#include "sdram_model.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace {

enum Cmd : unsigned { MRS = 0, REF = 1, PRE = 2, ACT = 3, WR = 4, RD = 5, BST = 6, NOP = 7 };

struct Drv {
    SdramModel m;
    uint64_t t = 0;
    bool cke = true;
    explicit Drv(const SdramTiming& tim, unsigned row = 13, unsigned col = 8) : m(row, col, tim) {}

    uint32_t tick(unsigned cmd = NOP, unsigned ba = 0, unsigned addr = 0, uint32_t dq = 0, bool oe = false,
                  unsigned dqm = 0) {
        SdramPins p{};
        p.cke = cke;
        p.cs_n = false;
        p.ras_n = (cmd >> 2) & 1;
        p.cas_n = (cmd >> 1) & 1;
        p.we_n = cmd & 1;
        p.ba = (uint8_t)ba;
        p.addr = (uint16_t)addr;
        p.dqm = (uint8_t)dqm;
        p.dq_o = dq;
        p.dq_oe = oe;
        ++t;
        return m.tick(p);
    }
    void nop(uint64_t n) { for (uint64_t i = 0; i < n; ++i) tick(); }
    void wait_after(uint32_t limit) { if (limit > 1) nop(limit - 1); }

    // legal init: power-up, PRE ALL, 2 x REF, MRS (BL8 seq CL2 unless given)
    void init(unsigned mode = 0x023) {
        const SdramTiming& tm = m.timing();
        nop(tm.power_up);
        tick(PRE, 0, 1u << 10); wait_after(tm.tRP);
        tick(REF);              wait_after(tm.tRFC);
        tick(REF);              wait_after(tm.tRFC);
        tick(MRS, 0, mode);     wait_after(tm.tMRD);
    }
};

int g_checks = 0, g_fail = 0;
void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) { ++g_fail; printf("  FAIL: %s\n", what.c_str()); }
}

SdramTiming slow_timing() {
    // larger limits so that 1-cycle violations are possible
    SdramTiming t;
    t.power_up = 100;
    t.tRP = 3; t.tRCD = 3; t.tRAS = 6; t.tRC = 10; t.tRFC = 8; t.tRRD = 2; t.tWR = 2; t.tMRD = 2;
    t.tREFI = 1000;
    t.refresh_slack = 8;
    return t;
}

// ------------------------------------------------------------------ legal traffic
void legal_traffic() {
    {   // from_ns matches the defaults / the table
        const SdramTiming a = SdramTiming::from_ns(50'000'000), b;
        check(a.power_up == 10000 && a.tRP == 1 && a.tRCD == 1 && a.tRAS == 3 && a.tRC == 3 && a.tRFC == 3 &&
              a.tWR == 2 && a.tMRD == 2 && a.tREFI == 781 && a.expect_cl == 2 && a.expect_bl == 8,
              "SdramTiming::from_ns(50 MHz) equals the section 6 table");
        check(a.power_up == b.power_up && a.tRP == b.tRP && a.tRAS == b.tRAS && a.tREFI == b.tREFI,
              "default-constructed SdramTiming equals from_ns(50 MHz)");
        const SdramTiming c = SdramTiming::from_ns(100'000'000);
        check(c.tRP == 2 && c.tRCD == 2 && c.tRAS == 5 && c.tRC == 6 && c.tREFI == 1562, "from_ns(100 MHz)");
    }
    Drv d{SdramTiming()};
    d.init();
    check(d.m.initialized() && d.m.violations() == 0, "init sequence accepted");

    // WRITE burst with auto precharge: bank 1, row 0x123, column 0x40 (line aligned)
    const unsigned row = 0x123, col = 0x40, ba = 1;
    uint32_t data[8];
    for (int i = 0; i < 8; ++i) data[i] = 0x11111111u * (i + 1) ^ 0xA5000000u;
    d.tick(ACT, ba, row);
    d.tick(WR, ba, (1u << 10) | col, data[0], true);
    for (int i = 1; i < 8; ++i) d.tick(NOP, 0, 0, data[i], true);
    d.nop(3);   // tWR + tRP after the last beat
    // backdoor: word index {ba, row, col}
    const uint32_t widx = (ba << 21) | (row << 8) | col;
    bool bd = true;
    for (int i = 0; i < 8; ++i) { uint32_t w; memcpy(&w, d.m.bytes() + (size_t)(widx + i) * 4, 4); bd &= w == data[i]; }
    check(bd, "write burst visible through the backdoor at {ba,row,col}");

    // READ with AP, starting at column +5: wrap order 5,6,7,0,1,2,3,4; CL = 2
    d.tick(ACT, ba, row);
    const uint32_t r0 = d.tick(RD, ba, (1u << 10) | (col + 5));   // k
    std::vector<uint32_t> got;
    got.push_back(r0);
    for (int i = 0; i < 10; ++i) got.push_back(d.tick());
    check(got[0] == SdramModel::kUndriven, "nothing driven at the READ edge");
    bool wrap = true;
    for (int n = 0; n < 8; ++n) wrap &= got[1 + n] == data[(5 + n) & 7];
    check(wrap, "read beats arrive CL-1+n ticks after READ, in sequential wrap order");
    check(got[9] == SdramModel::kUndriven, "bus released after the burst");

    // DQM write masking (latency 0) and read masking (latency 2), open page, explicit precharge
    d.tick(ACT, 2, 7);
    d.tick(WR, 2, 0x80, 0xCAFEF00Du, true, 0b0101);   // lanes 0 and 2 masked
    for (int i = 1; i < 8; ++i) d.tick(NOP, 0, 0, 0x01020304u * i, true);
    d.nop(1);
    d.tick(PRE, 2, 0);   // tWR = 2 after the last beat, tRAS satisfied
    d.nop(0);
    uint32_t w0;
    memcpy(&w0, d.m.bytes() + (size_t)((2u << 21) | (7u << 8) | 0x80) * 4, 4);
    check((w0 & 0xFF00FF00u) == (0xCAFEF00Du & 0xFF00FF00u), "DQM masks write lanes");
    d.tick(ACT, 2, 7);
    d.tick(RD, 2, 0x80, 0, false, 0b0000);
    const uint32_t m0 = d.tick(NOP, 0, 0, 0, false, 0b1000);   // beat 0 (mask from previous edge = 0)
    const uint32_t m1 = d.tick();                               // beat 1, masked lane 3
    const uint32_t m2 = d.tick(RD, 2, 0x82);                    // read-to-read, open page
    d.nop(1);
    const uint32_t m4 = d.tick();
    check(m0 == w0, "DQM low: beat 0 unmasked");
    check((m1 >> 24) == (SdramModel::kUndriven >> 24) && (m1 & 0xFFFFFF) == (0x01020304u & 0xFFFFFF),
          "DQM read mask has a latency of 2");
    check(m2 == 0x01020304u * 2, "beat 2 of the first burst");
    check(m4 == 0x01020304u * 3, "read-to-read interrupt: the new burst (from column 0x82) takes over");
    d.nop(8);
    d.tick(PRE, 0, 1u << 10);
    d.nop(1);
    check(d.m.violations() == 0, "open-page traffic, read/write with DQM, explicit precharge: no violations");

    // a long run: refresh every ~780 cycles with random line traffic in between
    uint32_t seed = 12345;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    uint64_t next_ref = d.t + 700;
    for (int i = 0; i < 4000; ++i) {
        if (d.t >= next_ref) { d.tick(REF); d.nop(2); next_ref += 781; }
        const unsigned b = rnd() & 3, r = rnd() & 0x1FFF, c = rnd() & 0xF8;
        d.tick(ACT, b, r);
        if (rnd() & 1) {
            d.tick(WR, b, (1u << 10) | c, rnd(), true);
            for (int k = 1; k < 8; ++k) d.tick(NOP, 0, 0, rnd(), true);
            d.nop(3);
        } else {
            d.tick(RD, b, (1u << 10) | c);
            d.nop(9);
        }
    }
    check(d.m.violations() == 0, "4000 random line transactions with periodic refresh: no violations");
    if (d.m.violations()) d.m.print_violations(stdout, 5);
    check(d.m.stats().refresh > 60, "refreshes counted");

    // interleaved burst order
    Drv e{SdramTiming()};
    e.init(0x02B);   // BL8, interleaved, CL2
    e.tick(ACT, 0, 1);
    e.tick(WR, 0, (1u << 10) | 0, 0, true);
    for (int i = 1; i < 8; ++i) e.tick(NOP, 0, 0, (uint32_t)i, true);
    e.nop(3);
    e.tick(ACT, 0, 1);
    e.tick(RD, 0, (1u << 10) | 5);
    bool il = true;
    for (int n = 0; n < 8; ++n) il &= e.tick() == (uint32_t)(5 ^ n);
    check(il && e.m.violations() == 0, "interleaved burst order (start 5: 5,4,7,6,1,0,3,2)");

    // CAS latency 3 and burst length 4 (mode-register driven; expectations off)
    SdramTiming any = SdramTiming();
    any.expect_cl = 0;
    any.expect_bl = 0;
    Drv g{any};
    g.init(0x032);   // BL4, sequential, CL3
    g.tick(ACT, 3, 9);
    g.tick(WR, 3, (1u << 10) | 0x10, 100, true);
    for (int i = 1; i < 4; ++i) g.tick(NOP, 0, 0, 100 + i, true);
    g.nop(3);
    g.tick(ACT, 3, 9);
    g.tick(RD, 3, (1u << 10) | 0x12);
    const uint32_t c1 = g.tick();               // CL-1 = 2 ticks after READ: still nothing
    bool cl3 = c1 == SdramModel::kUndriven;
    for (int n = 0; n < 4; ++n) cl3 &= g.tick() == 100u + ((2 + n) & 3);
    cl3 &= g.tick() == SdramModel::kUndriven;   // only 4 beats
    check(cl3 && g.m.violations() == 0 && g.m.cas_latency() == 3 && g.m.burst_length() == 4,
          "CL 3 / BL 4: beats 2,3,0,1 two ticks later than CL 2, burst of 4");
}

// ------------------------------------------------------------------ violations
struct VCase {
    const char* name;
    SdramModel::Kind kind;
    bool slow;       // use slow_timing()
    bool init;       // run the legal init first
    std::function<void(Drv&)> body;
};

void violations() {
    const unsigned AP = 1u << 10;
    std::vector<VCase> cases = {
        {"command during power-up", SdramModel::kPowerUp, false, false,
         [](Drv& d) { d.nop(10); d.tick(PRE, 0, 1u << 10); }},
        {"AUTO REFRESH before PRECHARGE ALL", SdramModel::kInitSeq, true, false,
         [](Drv& d) { d.nop(100); d.tick(REF); }},
        {"MRS after only one AUTO REFRESH", SdramModel::kInitSeq, true, false,
         [](Drv& d) { d.nop(100); d.tick(PRE, 0, 1u << 10); d.nop(2); d.tick(REF); d.nop(7); d.tick(MRS, 0, 0x023); }},
        {"ACTIVE before MRS", SdramModel::kNotInit, true, false,
         [](Drv& d) { d.nop(100); d.tick(PRE, 0, 1u << 10); d.nop(2); d.tick(ACT, 0, 0); }},
        {"CKE low after initialisation", SdramModel::kCkeLow, true, true,
         [](Drv& d) { d.cke = false; d.nop(3); d.cke = true; d.nop(1); }},
        {"tRP: PRECHARGE -> ACTIVE", SdramModel::kTRP, true, true,
         [](Drv& d) { d.tick(ACT, 0, 1); d.nop(8); d.tick(PRE, 0, 0); d.nop(1); d.tick(ACT, 0, 2); }},
        {"tRCD: ACTIVE -> READ", SdramModel::kTRCD, true, true,
         [=](Drv& d) { d.tick(ACT, 0, 1); d.nop(1); d.tick(RD, 0, 0); d.nop(12); }},
        {"tRAS: ACTIVE -> PRECHARGE", SdramModel::kTRAS, true, true,
         [](Drv& d) { d.tick(ACT, 0, 1); d.nop(3); d.tick(PRE, 0, 0); }},
        {"tRC: ACTIVE -> ACTIVE same bank", SdramModel::kTRC, true, true,
         [](Drv& d) { d.tick(ACT, 0, 1); d.nop(5); d.tick(PRE, 0, 0); d.nop(2); d.tick(ACT, 0, 2); }},
        {"tRFC: REFRESH -> REFRESH", SdramModel::kTRFC, true, true,
         [](Drv& d) { d.tick(REF); d.nop(3); d.tick(REF); }},
        {"tRRD: ACTIVE -> ACTIVE other bank", SdramModel::kTRRD, true, true,
         [](Drv& d) { d.tick(ACT, 0, 1); d.tick(ACT, 1, 1); }},
        {"tWR: last write data -> PRECHARGE", SdramModel::kTWR, true, true,
         [](Drv& d) {
             d.tick(ACT, 0, 1); d.nop(2);
             d.tick(WR, 0, 0, 1, true);
             for (int i = 1; i < 8; ++i) d.tick(NOP, 0, 0, i, true);
             d.tick(PRE, 0, 0);
         }},
        {"tMRD: MRS -> ACTIVE", SdramModel::kTMRD, true, true,
         [](Drv& d) { d.tick(MRS, 0, 0x023); d.tick(ACT, 0, 0); }},
        {"ACTIVE to an open bank", SdramModel::kBankActive, false, true,
         [](Drv& d) { d.tick(ACT, 3, 1); d.nop(4); d.tick(ACT, 3, 2); }},
        {"READ to a bank without an active row", SdramModel::kBankIdle, false, true,
         [](Drv& d) { d.tick(RD, 2, 0); d.nop(10); }},
        {"WRITE after the auto-precharge closed the row", SdramModel::kBankIdle, false, true,
         [=](Drv& d) { d.tick(ACT, 0, 1); d.tick(RD, 0, AP); d.nop(12); d.tick(WR, 0, 0, 0, true); d.nop(8); }},
        {"AUTO REFRESH with an open bank", SdramModel::kBanksOpen, false, true,
         [](Drv& d) { d.tick(ACT, 1, 1); d.nop(4); d.tick(REF); }},
        {"AUTO REFRESH before the auto-precharge finished", SdramModel::kBanksOpen, false, true,
         [=](Drv& d) { d.tick(ACT, 0, 1); d.tick(RD, 0, AP); d.nop(6); d.tick(REF); d.nop(4); }},
        {"PRECHARGE while auto-precharge pending", SdramModel::kAutoPre, false, true,
         [=](Drv& d) { d.tick(ACT, 0, 1); d.tick(RD, 0, AP); d.nop(2); d.tick(PRE, 0, 0); d.nop(8); }},
        {"READ interrupting an auto-precharge burst", SdramModel::kBurstInterrupt, false, true,
         [=](Drv& d) { d.tick(ACT, 0, 1); d.tick(ACT, 1, 1); d.tick(RD, 0, AP); d.nop(2); d.tick(RD, 1, 0); d.nop(10); }},
        {"PRECHARGE in the middle of a burst", SdramModel::kBurstInterrupt, false, true,
         [](Drv& d) { d.tick(ACT, 0, 1); d.nop(3); d.tick(RD, 0, 0); d.nop(2); d.tick(PRE, 0, 0); d.nop(10); }},
        {"DQ driven while the chip outputs read data", SdramModel::kContention, false, true,
         [=](Drv& d) { d.tick(ACT, 0, 1); d.tick(RD, 0, AP); d.nop(2); d.tick(NOP, 0, 0, 0, true); d.nop(10); }},
        {"write beat with DQ not driven", SdramModel::kWriteNoData, false, true,
         [=](Drv& d) {
             d.tick(ACT, 0, 1);
             d.tick(WR, 0, AP, 1, true);
             for (int i = 1; i < 8; ++i) d.tick(NOP, 0, 0, i, i != 4);
             d.nop(4);
         }},
        {"MRS with CAS latency 1", SdramModel::kModeReg, false, true,
         [](Drv& d) { d.tick(MRS, 0, 0x013); d.nop(2); }},
        {"MRS with burst length 4 (expected 8)", SdramModel::kModeReg, false, true,
         [](Drv& d) { d.tick(MRS, 0, 0x022); d.nop(2); }},
        {"refresh starvation", SdramModel::kRefresh, false, true,
         [](Drv& d) { d.nop(781ull * 12); }},
    };
    for (auto& c : cases) {
        Drv d{c.slow ? slow_timing() : SdramTiming()};
        if (c.init) d.init();
        const uint64_t before = d.m.violations();
        c.body(d);
        const uint64_t k = d.m.violations(c.kind);
        const bool ok = before == 0 && k >= 1;
        check(ok, std::string("violation detected: ") + c.name);
        printf("  %-48s -> %s %s\n", c.name, ok ? "detected" : "MISSED  ",
               d.m.violation_log().empty() ? "" : d.m.violation_log().front().c_str());
    }
}

// fast-path cost (informational)
void speed() {
    Drv d{SdramTiming()};
    d.init();
    SdramPins p{};
    p.cke = true; p.cs_n = true; p.ras_n = p.cas_n = p.we_n = true;
    const uint64_t n = 200'000'000;
    uint32_t acc = 0;
    const auto t0 = std::clock();
    for (uint64_t i = 0; i < n; ++i) {
        if ((i & 511) == 0) { p.cs_n = false; p.ras_n = false; p.cas_n = false; p.we_n = true; }   // REF
        acc ^= d.m.tick(p);
        p.cs_n = true; p.ras_n = p.cas_n = p.we_n = true;
    }
    const double s = double(std::clock() - t0) / CLOCKS_PER_SEC;
    printf("  idle tick() cost: %.2f ns/call (%llu calls, refresh every 512) [%x]\n", 1e9 * s / n,
           (unsigned long long)n, acc & 1);
    check(d.m.violations() == 0, "idle run with refresh: no violations");
}

}  // namespace

int main() {
    printf("sdram_model_test: legal traffic\n");
    legal_traffic();
    printf("sdram_model_test: injected violations\n");
    violations();
    speed();
    printf("sdram_model_test: %d checks, %d failed: %s\n", g_checks, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
