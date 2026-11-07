// Randomised contract + data testbench for memsys (rtl/mem/README.md, "Verification").
//
// Configuration (tests/mem/Makefile): TB_FAST, TB_ICACHE_BYTES, TB_DCACHE_BYTES, TB_ROW_BITS, TB_CLK_HZ, TB_CFG.
// Run:  tb [--seeds A-B | --seed N] [--ops N] [--inject KIND] [--penalty] [-v]
//
// Per cycle (clk low, outputs settled):
//   A  sample rsp_* of both ports; check completions (latency against a
//      shadow cache model: hit -> exactly the predicted cycle, miss -> more), data
//      (D loads exact against the reference, fetches against the reference or,
//      for words the D-port stores to, against that word's value history),
//      held responses (rsp_ready stays 1, rsp_rdata stable), perf strobes
//   B  drivers present the next requests (legal per R1; random garbage on
//      the request lines when req_valid = 0)
//   C  re-evaluate and re-sample rsp_*: any change means rsp_* depend
//      combinationally on req_* (R5)
//   D  SDRAM model tick (optionally with injected pin faults)
//   E  rising edge (accepts recorded: shadow cache, reference memory), dq_i
//      update, falling edge
// At the end the D-cache is flushed through conflict loads and the whole
// backing store is compared with the reference.
#include <verilated.h>
#include <verilated_syms.h>

#include "Vmemsys.h"
#include "sdram_model.h"

#include <algorithm>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

static constexpr unsigned kColBits = 8;
static constexpr unsigned kRamAW   = 4 + TB_ROW_BITS + kColBits;
static constexpr uint32_t kRamBytes = 1u << kRamAW;
static constexpr uint32_t kOffMask  = kRamBytes - 1;
static constexpr unsigned kILines  = TB_ICACHE_BYTES / 32;
static constexpr unsigned kDLines  = TB_DCACHE_BYTES / 32;
// D-cache: a dirty miss copies its victim in the 4 cycles after the miss starts and
// completes no earlier; the first beat can arrive 2 cycles after the start with the
// ideal backend (registered line request, beat in the cycle after the grant), only
// later than the copy with SDRAM. So a dirty miss completes 0..kDirtyLead cycles
// after its first beat.
static constexpr unsigned kDirtyLead = TB_FAST ? 4 - 2 : 0;

// ---------------------------------------------------------------------------
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) {}
    uint64_t next() {   // splitmix64
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint32_t u32() { return (uint32_t)next(); }
    uint32_t below(uint32_t n) { return n ? (uint32_t)((next() >> 32) * n >> 32) : 0; }
    uint32_t range(uint32_t lo, uint32_t hi) { return lo + below(hi - lo + 1); }
    bool chance(unsigned pct) { return below(100) < pct; }
};

// ---------------------------------------------------------------------------
// address map used by the test (RAM byte offsets)
struct Region { uint32_t base, size; };
enum : int { R_CODE, R_SHARED, R_DATA, R_BANK1, R_BANK2, R_BANK3, R_HIGH, R_HICODE, R_FLUSH, R_NUM };
static Region g_reg[R_NUM];

static void init_regions() {
    const uint32_t q = kRamBytes / 4;
    g_reg[R_CODE]   = {0x00000, 0x10000};                 // I + D loads
    g_reg[R_SHARED] = {0x10000, 0x01000};                 // I + D loads/stores (value history)
    g_reg[R_DATA]   = {0x20000, 0x40000};                 // D
    g_reg[R_BANK1]  = {1 * q - 0x2000, 0x4000};           // D, across the bank boundaries
    g_reg[R_BANK2]  = {2 * q - 0x2000, 0x4000};
    g_reg[R_BANK3]  = {3 * q - 0x2000, 0x4000};
    g_reg[R_HIGH]   = {kRamBytes - 0x10000, 0x10000};     // D, top of RAM
    g_reg[R_HICODE] = {kRamBytes - 0x20000, 0x10000};     // I + D loads, high tags
    g_reg[R_FLUSH]  = {2 * q + 0x100000, TB_DCACHE_BYTES}; // only used by the final flush
}
static bool in_reg(uint32_t off, int r) { return off - g_reg[r].base < g_reg[r].size; }
static bool d_store_ok(uint32_t off) { return !in_reg(off, R_CODE) && !in_reg(off, R_HICODE) && !in_reg(off, R_FLUSH); }
static bool d_load_ok(uint32_t off) { return !in_reg(off, R_FLUSH); }

// ---------------------------------------------------------------------------
// shadow of a direct-mapped cache: predicts hit/miss and victim state
enum Cls { C_HIT, C_MISS_INV, C_MISS_CLEAN, C_MISS_DIRTY, C_NUM };
static const char* const kClsName[C_NUM] = {"hit", "miss(invalid victim)", "miss(clean victim)", "miss(dirty victim)"};

struct Shadow {
    unsigned lines, idx_bits;
    std::vector<int64_t> tag;
    std::vector<uint8_t> dirty;
    explicit Shadow(unsigned n) : lines(n), idx_bits(__builtin_ctz(n)), tag(n, -1), dirty(n, 0) {}
    unsigned idx(uint32_t off) const { return (off >> 5) & (lines - 1); }
    int64_t tg(uint32_t off) const { return off >> (5 + idx_bits); }
    Cls access(uint32_t off, bool store) {
        const unsigned i = idx(off);
        const int64_t t = tg(off);
        Cls c;
        if (tag[i] == t) c = C_HIT;
        else if (tag[i] < 0) c = C_MISS_INV;
        else c = dirty[i] ? C_MISS_DIRTY : C_MISS_CLEAN;
        if (c != C_HIT) { tag[i] = t; dirty[i] = 0; }
        if (store) dirty[i] = 1;
        return c;
    }
    unsigned dirty_lines() const {
        unsigned n = 0;
        for (unsigned i = 0; i < lines; ++i) n += (tag[i] >= 0 && dirty[i]);
        return n;
    }
};

// latency statistics
struct Lat {
    uint64_t n = 0, sum = 0, mn = ~0ull, mx = 0;
    std::vector<uint64_t> hist = std::vector<uint64_t>(256, 0);
    void add(uint64_t v) { ++n; sum += v; mn = std::min(mn, v); mx = std::max(mx, v); ++hist[std::min<uint64_t>(v, 255)]; }
    uint64_t median() const {
        uint64_t acc = 0;
        for (size_t i = 0; i < hist.size(); ++i) { acc += hist[i]; if (2 * acc >= n) return i; }
        return 0;
    }
};

struct Req {
    uint32_t addr = 0;
    bool     we = false;
    uint8_t  strb = 0;
    uint32_t wdata = 0;
};

// ---------------------------------------------------------------------------
enum GapProfile { G_BUSY, G_NORMAL, G_SPARSE };
enum DPat { DP_SEQ, DP_REUSE, DP_CONFLICT, DP_STLD, DP_RANDOM, DP_SHARED, DP_WIDE, DP_FILL, DP_NUM };
enum IPat { IP_SEQ, IP_LOOP, IP_CONFLICT, IP_SHARED, IP_RANDOM, IP_NUM };
enum Couple { CP_INDEP, CP_LOCKSTEP, CP_I_ONLY, CP_D_ONLY, CP_NUM };
static const char* const kDPatName[DP_NUM] = {"seq", "reuse", "conflict", "st-ld", "random", "shared", "wide", "fill"};

struct Port {
    const char* name;
    bool     is_d;
    bool     busy = false;          // accepted, not completed
    bool     any = false;           // something accepted since reset
    bool     seen_ready = false;
    uint32_t held = 0;
    uint64_t next_ok = 0;           // earliest cycle for the next request
    std::deque<Req> script;         // forced requests (take priority)
    // current request
    Req      cur;
    uint64_t acc_cyc = 0;
    Cls      cls = C_HIT;
    int      chk = 0;               // 0 none (store), 1 exact, 2 value history
    uint32_t expect = 0;
    uint32_t woff = 0;              // word offset
    // perf strobe expectations: ring of accepts
    uint8_t  acc_ring[4] = {0, 0, 0, 0};   // bit0 accepted, bit1 miss
    // stats
    uint64_t n = 0, n_store = 0, cls_n[C_NUM] = {}, perf_acc = 0, perf_miss = 0;
    uint64_t b2b = 0, last_acc = ~0ull;  // accepts in consecutive cycles
    Lat      lat[C_NUM][2];         // [class][store]
    // the line refilled for the most recent miss: its beats arrive one per cycle,
    // critical word first, the first one in a cycle in [fill_lo, fill_hi]
    uint32_t fill_line = ~0u, fill_w0 = 0;
    uint64_t fill_lo = 0, fill_hi = 0;
    // a store hit completed while the fill was on (first beat <= completion - 7) is
    // held in the D-cache's store buffer until the fill is over: first-beat cycle of
    // that fill under the lo / hi assumption, 0 = no store buffered
    uint64_t sb_lo = 0, sb_hi = 0;
    uint64_t hit_lo = 0, hit_hi = 0; // predicted completion cycle range of the current hit
    uint64_t n_late_hit = 0;        // hits that waited for a beat or for the fill to end
};

// ---------------------------------------------------------------------------
// Backdoor to the FAST-build RAM <memsys>.g_ideal.u_ideal.mem through Verilator's
// scope table (independent of the hierarchy and of generated class names).
#if TB_FAST
static uint32_t* find_mem_ideal(VerilatedContext* ctx) {
    if (const VerilatedScopeNameMap* scopes = ctx->scopeNameMap())
        for (const auto& kv : *scopes) {
            const std::string n = kv.first;     // "...g_ideal.u_ideal"
            if (n.size() >= 15 && n.compare(n.size() - 15, 15, "g_ideal.u_ideal") == 0)
                if (const VerilatedVar* v = kv.second->varFind("mem")) return static_cast<uint32_t*>(v->datap());
        }
    return nullptr;
}
#endif

// pin faults (--inject) and the violation the chip model must report for each
static const struct { const char* name; SdramModel::Kind kind; } kInject[] = {
    {"refresh", SdramModel::kRefresh}, {"act", SdramModel::kBankIdle},
    {"contention", SdramModel::kContention}, {"powerup", SdramModel::kPowerUp}};

// ---------------------------------------------------------------------------
class Bench {
public:
    Bench(uint64_t seed, uint64_t ops, int inject, bool verbose)
        : rng_(seed), seed_(seed), ops_target_(ops), inject_(inject), verbose_(verbose),
          ish_(kILines), dsh_(kDLines) {
        ctx_ = std::make_unique<VerilatedContext>();
        ctx_->randReset(2);
        ctx_->randSeed((int)(seed & 0x7fffffff));
        top_ = std::make_unique<Vmemsys>(ctx_.get());
#if TB_FAST
        // <...>.g_ideal.u_ideal.mem: logic [31:0] mem [0:2**23-1], word i = bytes 4i..4i+3
        store_ = reinterpret_cast<uint8_t*>(find_mem_ideal(ctx_.get()));
        if (!store_) die("mem_ideal array not found");
#else
        sdram_ = std::make_unique<SdramModel>(TB_ROW_BITS, kColBits, SdramTiming::from_ns(TB_CLK_HZ));
        store_ = sdram_->bytes();
        if (sdram_->size_bytes() != kRamBytes) die("SDRAM model size mismatch");
#endif
        ip_.name = "I"; ip_.is_d = false;
        dp_.name = "D"; dp_.is_d = true;
    }

    // ------------------------------------------------------------------ main
    bool run_random() {
        preload();
        reset();
        new_phase();
        while (ops_done_ < ops_target_ && !fatal_) {
            if (--phase_left_ <= 0 && ip_.script.empty() && dp_.script.empty()) new_phase();
            cycle();
        }
        finish();
        return report();
    }

    bool run_penalty();

private:
    // ------------------------------------------------------------------ util
    [[noreturn]] void die(const char* m) { fprintf(stderr, "FATAL: %s\n", m); exit(2); }
    void error(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        ++errors_;
        if (errors_ <= 20) {
            char buf[400];
            va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
            fprintf(stderr, "[%s seed %" PRIu64 "] cycle %" PRIu64 ": ERROR %s\n", TB_CFG, seed_, cyc_, buf);
        }
        if (errors_ > 200) fatal_ = true;
    }
    uint32_t ref_word(uint32_t off) const { uint32_t w; memcpy(&w, &ref_[off & ~3u], 4); return w; }
    uint32_t store_word(uint32_t off) const { uint32_t w; memcpy(&w, &store_[off & ~3u], 4); return w; }
    uint32_t with_high_bits(uint32_t off) {
        uint32_t a = 0x80000000u | off;
        if (rng_.chance(25)) a |= rng_.u32() & 0x7FFFFFFFu & ~kOffMask;   // ignored alias bits
        return a;
    }

    // ------------------------------------------------------------------ setup
    void preload() {
        ref_.resize(kRamBytes);
        Rng r(seed_ ^ 0xA5A5A5A5u);
        for (uint32_t i = 0; i < kRamBytes; i += 8) {
            uint64_t v = r.next();
            memcpy(&ref_[i], &v, 8);
        }
        memcpy(store_, ref_.data(), kRamBytes);
        for (uint32_t o = g_reg[R_SHARED].base; o < g_reg[R_SHARED].base + g_reg[R_SHARED].size; o += 4)
            hist_[o].push_back(ref_word(o));
    }

    // request inputs: the request, or random garbage on the lines when req_valid = 0
    void set_inputs(bool iv, const Req& irq, bool dv, const Req& drq) {
        top_->i_req_valid = iv;
        top_->i_req_addr = iv ? irq.addr : rng_.u32();
        top_->d_req_valid = dv;
        top_->d_req_addr = dv ? drq.addr : rng_.u32();
        top_->d_req_we = dv ? drq.we : (rng_.u32() & 1);
        top_->d_req_wstrb = dv ? drq.strb : (rng_.u32() & 15);
        top_->d_req_wdata = dv ? drq.wdata : rng_.u32();
    }

    void reset() {
        top_->clk = 0;
        top_->rst = 1;
        top_->sdram_dq_i = 0;
        set_inputs(false, Req(), false, Req());
        top_->eval();
        for (int i = 0; i < 8; ++i) edge();
        top_->rst = 0;
        top_->eval();
    }

    // ------------------------------------------------------------------ one clock
    SdramPins pins() const {
        SdramPins p;
        p.cke = top_->sdram_cke; p.cs_n = top_->sdram_cs_n; p.ras_n = top_->sdram_ras_n;
        p.cas_n = top_->sdram_cas_n; p.we_n = top_->sdram_we_n; p.ba = top_->sdram_ba;
        p.addr = top_->sdram_addr; p.dqm = top_->sdram_dqm; p.dq_o = top_->sdram_dq_o;
        p.dq_oe = top_->sdram_dq_oe;
        return p;
    }

    void edge() {
        uint32_t dq_next = 0;
#if !TB_FAST
        SdramPins p = pins();
        inject_pins(p);
        dq_next = sdram_->tick(p);
#endif
        top_->clk = 1;
        top_->eval();
        top_->sdram_dq_i = dq_next;
        top_->clk = 0;
        top_->eval();
        ++cyc_;
    }

    void inject_pins(SdramPins& p) {
        const bool cmd = !p.cs_n;
        const unsigned c = (p.ras_n << 2) | (p.cas_n << 1) | p.we_n;
        switch (inject_) {
            case 0:   // drop every AUTO REFRESH after initialisation
                if (cmd && c == 0b001 && sdram_->initialized()) p.cs_n = 1;
                break;
            case 1:   // drop the 200th ACTIVE: its READ/WRITE goes to an idle bank
                if (cmd && c == 0b011 && ++inj_cnt_ == 200) p.cs_n = 1;
                break;
            case 2:   // drive DQ while the chip returns the 2nd beat of the 100th read
                if (cmd && c == 0b101 && ++inj_cnt_ == 100) inj_at_ = cyc_ + 2;
                if (cyc_ == inj_at_) p.dq_oe = 1;
                break;
            case 3:   // a PRECHARGE ALL long before the power-up delay has elapsed
                if (cyc_ == 100) { p.cs_n = 0; p.ras_n = 0; p.cas_n = 1; p.we_n = 0; p.addr = 1u << 10; }
                break;
            default: break;
        }
    }

    void cycle() {
        // ---- A: sample and check
        const bool ir = top_->i_rsp_ready, dr = top_->d_rsp_ready;
        const uint32_t idat = top_->i_rsp_rdata, ddat = top_->d_rsp_rdata;
        check_perf(ip_, top_->perf_ic_access, top_->perf_ic_miss);
        check_perf(dp_, top_->perf_dc_access, top_->perf_dc_miss);
        check_port(ip_, ir, idat);
        check_port(dp_, dr, ddat);

        // ---- B: drive
        bool iv = false, dv = false;
        Req irq, drq;
        drive(ir, dr, iv, irq, dv, drq);
        set_inputs(iv, irq, dv, drq);
        top_->eval();

        // ---- C: R5 - rsp_* must not depend on req_* of the same cycle
        if (top_->i_rsp_ready != ir || top_->i_rsp_rdata != idat)
            error("I-port rsp_* changed combinationally with req_* (R5)");
        if (top_->d_rsp_ready != dr || top_->d_rsp_rdata != ddat)
            error("D-port rsp_* changed combinationally with req_* (R5)");
        if ((iv && !ir) || (dv && !dr)) die("testbench drove an illegal request");

        // ---- D/E: clock; record accepts at this edge
        edge();
        shift_ring(ip_);
        shift_ring(dp_);
        if (iv) accept(ip_, irq);
        if (dv) accept(dp_, drq);
    }

    // ------------------------------------------------------------------ checks
    void shift_ring(Port& p) {
        p.acc_ring[3] = p.acc_ring[2]; p.acc_ring[2] = p.acc_ring[1];
        p.acc_ring[1] = p.acc_ring[0]; p.acc_ring[0] = 0;
    }
    void check_perf(Port& p, bool acc, bool miss) {
        // access pulse in the cycle after the accept, miss pulse one cycle later
        const bool exp_acc = p.acc_ring[0] & 1;
        const bool exp_miss = (p.acc_ring[1] & 1) && (p.acc_ring[1] & 2);
        if (acc != exp_acc) error("%s perf access strobe %d, expected %d", p.name, acc, exp_acc);
        if (miss != exp_miss) error("%s perf miss strobe %d, expected %d", p.name, miss, exp_miss);
        p.perf_acc += acc;
        p.perf_miss += miss;
    }

    void check_port(Port& p, bool ready, uint32_t data) {
        if (ready) p.seen_ready = true;
        if (p.busy) {
            const uint64_t lat = cyc_ - p.acc_cyc;
            if (!ready) {
                if (p.cls == C_HIT && cyc_ >= p.hit_hi)
                    error("%s hit at 0x%08x not complete %" PRIu64 " cycles after accept (expected %" PRIu64 ")",
                          p.name, p.cur.addr, lat, p.hit_hi - p.acc_cyc);
                if (lat > 200000) { error("%s request 0x%08x timed out", p.name, p.cur.addr); fatal_ = true; }
                return;
            }
            // completion
            if (p.cls != C_HIT && lat == 1)
                error("%s 0x%08x completed in 1 cycle but the shadow cache predicts a miss", p.name, p.cur.addr);
            if (p.cls == C_HIT && (cyc_ < p.hit_lo || cyc_ > p.hit_hi))
                error("%s hit at 0x%08x completed after %" PRIu64 " cycles, expected %" PRIu64 "..%" PRIu64, p.name,
                      p.cur.addr, lat, p.hit_lo - p.acc_cyc, p.hit_hi - p.acc_cyc);
            if (p.cls != C_HIT) {
                // A miss completes in the cycle its first beat arrives, but a dirty
                // D miss not before its victim has been copied (see kDirtyLead).
                p.fill_line = (p.cur.addr & kOffMask) >> 5;
                p.fill_w0 = (p.cur.addr >> 2) & 7;
                p.fill_lo = p.fill_hi = cyc_;
                if (p.cls == C_MISS_DIRTY) p.fill_lo = cyc_ - kDirtyLead;
                p.sb_lo = p.sb_hi = 0;          // a miss starts only with an empty buffer
            } else if (p.cur.we) {
                p.sb_lo = cyc_ <= p.fill_lo + 7 ? p.fill_lo : 0;
                p.sb_hi = cyc_ <= p.fill_hi + 7 ? p.fill_hi : 0;
            }
            p.n_late_hit += p.cls == C_HIT && lat > 1;
            check_data(p, data);
            p.busy = false;
            p.held = data;
            p.lat[p.cls][p.cur.we].add(lat);
            p.next_ok = cyc_ + next_gap(p);
            ++ops_done_;
            return;
        }
        if (p.any) {
            if (!ready) error("%s rsp_ready dropped while holding a completed response", p.name);
            else if (data != p.held)
                error("%s rsp_rdata changed while held: 0x%08x -> 0x%08x", p.name, p.held, data);
        } else if (p.seen_ready && !ready) {
            error("%s rsp_ready dropped while idle", p.name);
        }
    }

    void check_data(Port& p, uint32_t data) {
        if (p.chk == 1) {
            if (data != p.expect)
                error("%s %s 0x%08x returned 0x%08x, expected 0x%08x (%s)", p.name, p.is_d ? "load" : "fetch",
                      p.cur.addr, data, p.expect, kClsName[p.cls]);
        } else if (p.chk == 2) {
            const auto& h = hist_[p.woff];
            if (std::find(h.begin(), h.end(), data) == h.end())
                error("%s fetch 0x%08x returned 0x%08x, never a value of that word (%s)", p.name, p.cur.addr,
                      data, kClsName[p.cls]);
        }
    }

    void accept(Port& p, const Req& r) {
        const uint32_t off = r.addr & kOffMask;
        p.busy = true;
        p.any = true;
        p.cur = r;
        p.acc_cyc = cyc_ - 1;          // the edge just taken
        p.b2b += p.acc_cyc == p.last_acc + 1;
        p.last_acc = p.acc_cyc;
        p.woff = off & ~3u;
        p.cls = (p.is_d ? dsh_ : ish_).access(off, r.we);
        // a hit completes in the cycle after the accept, except
        //   a load/fetch of a word of the line being filled: in the cycle its beat arrives
        //   a D store while the store buffer holds a store posted during the fill: in
        //   the cycle after the fill is over (the buffer is written then)
        {
            const uint64_t t1 = p.acc_cyc + 1;
            if (r.we) {
                p.hit_lo = std::max(t1, p.sb_lo ? p.sb_lo + 9 : 0);
                p.hit_hi = std::max(t1, p.sb_hi ? p.sb_hi + 9 : 0);
            } else {
                const unsigned d = (off >> 5) == p.fill_line ? (((off >> 2) & 7) - p.fill_w0) & 7 : 0;
                p.hit_lo = std::max(t1, p.fill_lo + d);
                p.hit_hi = std::max(t1, p.fill_hi + d);
            }
        }
        p.acc_ring[0] = 1 | (p.cls != C_HIT ? 2 : 0);
        ++p.n;
        ++p.cls_n[p.cls];
        if (p.is_d && r.we) {
            ++p.n_store;
            p.chk = 0;
            for (unsigned l = 0; l < 4; ++l)
                if ((r.strb >> l) & 1) ref_[p.woff + l] = (uint8_t)(r.wdata >> (8 * l));
            if (in_reg(off, R_SHARED)) hist_[p.woff].push_back(ref_word(p.woff));
        } else if (!p.is_d && in_reg(off, R_SHARED)) {
            p.chk = 2;
        } else {
            p.chk = 1;
            p.expect = ref_word(p.woff);
        }
    }

    // ------------------------------------------------------------------ drivers
    struct Phase {
        DPat dpat = DP_RANDOM;
        IPat ipat = IP_SEQ;
        Couple cp = CP_INDEP;
        GapProfile igap = G_NORMAL, dgap = G_NORMAL;
        unsigned st_pct = 40;
        // pattern state
        uint32_t d_ptr = 0, d_step = 4, d_end = 0, d_beg = 0;
        uint32_t i_ptr = 0, i_beg = 0, i_end = 0;
        std::vector<uint32_t> d_set, i_set;
    } ph_;
    int64_t phase_left_ = 0;

    unsigned sample_gap(GapProfile g) {
        const unsigned r = rng_.below(100);
        switch (g) {
            case G_BUSY: return r < 85 ? 0 : r < 97 ? rng_.range(1, 2) : rng_.range(3, 10);
            case G_NORMAL:
                return r < 55 ? 0 : r < 80 ? rng_.range(1, 3) : r < 95 ? rng_.range(4, 30)
                     : r < 99 ? rng_.range(31, 300) : rng_.range(301, 2000);
            case G_SPARSE:
            default:
                return r < 20 ? 0 : r < 50 ? rng_.range(1, 10) : r < 85 ? rng_.range(11, 200) : rng_.range(201, 3000);
        }
    }
    unsigned next_gap(Port& p) {
        if (in_penalty_) return penalty_gap_;
        if (!p.script.empty()) return 0;
        return sample_gap(p.is_d ? ph_.dgap : ph_.igap);
    }

    uint32_t pick_store_region_off() {
        static const int rs[] = {R_DATA, R_DATA, R_DATA, R_BANK1, R_BANK2, R_BANK3, R_HIGH, R_SHARED};
        const Region& g = g_reg[rs[rng_.below(8)]];
        return g.base + (rng_.below(g.size) & ~3u);
    }
    uint32_t pick_wide_off(bool store) {
        for (;;) {
            uint32_t o = rng_.u32() & kOffMask & ~3u;
            if (store ? d_store_ok(o) : d_load_ok(o)) return o;
        }
    }
    uint32_t conflict_off(uint32_t line_idx_off, bool store) {
        // same D-cache index, random tag anywhere in RAM
        for (;;) {
            uint32_t o = (rng_.u32() & kOffMask & ~(uint32_t)(TB_DCACHE_BYTES - 1)) | line_idx_off;
            if (store ? d_store_ok(o) : d_load_ok(o)) return o;
        }
    }

    void new_phase() {
        Phase& p = ph_;
        p = Phase();
        phase_left_ = rng_.range(300, 20000);
        p.dpat = (DPat)rng_.below(DP_NUM);
        p.ipat = (IPat)rng_.below(IP_NUM);
        const unsigned c = rng_.below(100);
        p.cp = c < 50 ? CP_INDEP : c < 80 ? CP_LOCKSTEP : c < 90 ? CP_I_ONLY : CP_D_ONLY;
        const unsigned g = rng_.below(100);
        p.dgap = g < 45 ? G_BUSY : g < 90 ? G_NORMAL : G_SPARSE;
        const unsigned g2 = rng_.below(100);
        p.igap = g2 < 45 ? G_BUSY : g2 < 90 ? G_NORMAL : G_SPARSE;
        static const unsigned st[] = {0, 10, 30, 50, 50, 70, 100};
        p.st_pct = st[rng_.below(7)];
        // D pattern state
        switch (p.dpat) {
            case DP_SEQ: {
                static const uint32_t steps[] = {4, 4, 4, 8, 32, 36, 64};
                p.d_step = steps[rng_.below(7)];
                static const int rs[] = {R_DATA, R_HIGH, R_BANK1, R_BANK3, R_SHARED};
                const Region& r = g_reg[rs[rng_.below(5)]];
                p.d_beg = r.base; p.d_end = r.base + r.size;
                p.d_ptr = r.base + (rng_.below(r.size) & ~3u);
                break;
            }
            case DP_REUSE: {
                const unsigned k = rng_.range(1, 16);
                for (unsigned i = 0; i < k; ++i) p.d_set.push_back(pick_store_region_off());
                break;
            }
            case DP_CONFLICT: {
                const unsigned nidx = rng_.range(1, 3), ways = rng_.range(2, 5);
                for (unsigned i = 0; i < nidx; ++i) {
                    const uint32_t lidx = rng_.below(kDLines) * 32;
                    for (unsigned w = 0; w < ways; ++w) p.d_set.push_back(conflict_off(lidx, true));
                }
                break;
            }
            default: break;
        }
        // I pattern state
        switch (p.ipat) {
            case IP_SEQ:
            case IP_LOOP: {
                const Region& r = g_reg[rng_.chance(70) ? R_CODE : R_HICODE];
                const uint32_t len = p.ipat == IP_LOOP ? rng_.range(4, 600) * 4 : r.size;
                p.i_beg = r.base + (rng_.below(r.size - len + 1) & ~3u);
                p.i_end = p.i_beg + len;
                p.i_ptr = p.i_beg;
                break;
            }
            case IP_CONFLICT: {
                const uint32_t lidx = rng_.below(kILines) * 32;
                const unsigned ways = rng_.range(2, 4);
                for (unsigned w = 0; w < ways; ++w) {
                    const Region& r = g_reg[rng_.chance(70) ? R_CODE : R_HICODE];
                    uint32_t o = r.base + ((rng_.below(r.size) & ~(uint32_t)(TB_ICACHE_BYTES - 1)) | lidx);
                    if (!in_reg(o, R_CODE) && !in_reg(o, R_HICODE)) o = lidx;
                    p.i_set.push_back(o);
                }
                break;
            }
            default: break;
        }
        if (verbose_)
            fprintf(stderr, "phase @%" PRIu64 ": D=%s I=%d couple=%d st=%u%%\n", cyc_, kDPatName[p.dpat], p.ipat,
                    p.cp, p.st_pct);
    }

    Req make_store(uint32_t off) {
        static const uint8_t strbs[7] = {1, 2, 4, 8, 3, 12, 15};
        Req r;
        r.we = true;
        r.strb = strbs[rng_.below(7)];
        const unsigned lane = __builtin_ctz(r.strb);
        r.addr = with_high_bits((off & ~3u) | lane);
        r.wdata = rng_.u32();       // all lanes random: unselected lanes must be ignored
        return r;
    }
    Req make_load(uint32_t off) {
        Req r;
        r.we = false;
        r.strb = 0;
        r.addr = with_high_bits((off & ~3u) | rng_.below(4));
        r.wdata = rng_.u32();
        return r;
    }
    Req make_d(uint32_t off) {
        return (rng_.chance(ph_.st_pct) && d_store_ok(off)) ? make_store(off) : make_load(off);
    }

    Req gen_d() {
        Phase& p = ph_;
        switch (p.dpat) {
            case DP_SEQ: {
                const uint32_t o = p.d_ptr;
                p.d_ptr += p.d_step;
                if (p.d_ptr >= p.d_end) p.d_ptr = p.d_beg + (p.d_ptr & 31 & ~3u);
                return make_d(o);
            }
            case DP_REUSE:
            case DP_CONFLICT: {
                const uint32_t o = p.d_set[rng_.below(p.d_set.size())];
                return make_d(o + (p.dpat == DP_REUSE ? 0 : (rng_.below(8) * 4)));
            }
            case DP_STLD: {
                // store then (gap 0) load of the same word; sometimes store, store, load
                const uint32_t o = rng_.chance(50) ? pick_store_region_off()
                                                   : (p.d_set.empty() ? pick_store_region_off() : p.d_set[0]);
                if (p.d_set.size() < 4) p.d_set.push_back(o);
                Req s = make_store(o);
                if (rng_.chance(30)) dp_.script.push_back(make_store(o));
                dp_.script.push_back(make_load(o));
                return s;
            }
            case DP_FILL: {
                // a (likely) miss, then back to back during its refill: a store to a word
                // of that line (posted to the store buffer; the word in, arriving or not
                // yet in), sometimes a load of another word, and a load of the stored word
                const uint32_t o = pick_store_region_off() & ~31u;
                const unsigned w = rng_.below(8);
                const uint32_t ow = o + ((w + rng_.below(8)) & 7) * 4;
                dp_.script.push_back(make_store(ow));
                if (rng_.chance(50)) dp_.script.push_back(make_load(o + ((w + rng_.below(8)) & 7) * 4));
                dp_.script.push_back(make_load(ow));
                return make_load(o + w * 4);
            }
            case DP_SHARED: {
                const Region& r = rng_.chance(80) ? g_reg[R_SHARED] : g_reg[R_CODE];
                return make_d(r.base + (rng_.below(r.size) & ~3u));
            }
            case DP_WIDE: {
                const bool st = rng_.chance(ph_.st_pct);
                const uint32_t o = pick_wide_off(st);
                return st ? make_store(o) : make_load(o);
            }
            case DP_RANDOM:
            default: {
                const bool st = rng_.chance(ph_.st_pct);
                uint32_t o;
                if (rng_.chance(15)) o = pick_wide_off(st);
                else if (!st && rng_.chance(25)) o = g_reg[rng_.chance(50) ? R_CODE : R_HICODE].base + (rng_.below(0x10000) & ~3u);
                else o = pick_store_region_off();
                return st ? make_store(o) : make_load(o);
            }
        }
    }

    Req gen_i() {
        Phase& p = ph_;
        uint32_t o = 0;
        switch (p.ipat) {
            case IP_SEQ:
            case IP_LOOP:
                o = p.i_ptr;
                p.i_ptr += 4;
                if (p.ipat == IP_SEQ && rng_.chance(8)) {   // taken branch
                    const int32_t d = (int32_t)rng_.range(0, 4096) - 2048;
                    p.i_ptr = (uint32_t)std::clamp<int64_t>((int64_t)p.i_ptr + d * 4, p.i_beg, p.i_end - 4);
                }
                if (p.i_ptr >= p.i_end) p.i_ptr = p.i_beg;
                break;
            case IP_CONFLICT:
                o = p.i_set[rng_.below(p.i_set.size())] + rng_.below(8) * 4;
                break;
            case IP_SHARED: {
                const Region& r = rng_.chance(70) ? g_reg[R_SHARED] : g_reg[R_CODE];
                o = r.base + (rng_.below(r.size) & ~3u);
                break;
            }
            case IP_RANDOM:
            default: {
                const int rr[] = {R_CODE, R_CODE, R_HICODE, R_SHARED};
                const Region& r = g_reg[rr[rng_.below(4)]];
                o = r.base + (rng_.below(r.size) & ~3u);
                break;
            }
        }
        Req r;
        r.addr = with_high_bits(o & ~3u);
        return r;
    }

    void drive(bool ir, bool dr, bool& iv, Req& irq, bool& dv, Req& drq) {
        const bool i_can = ir && ip_.seen_ready && cyc_ >= ip_.next_ok && !draining_;
        const bool d_can = dr && dp_.seen_ready && cyc_ >= dp_.next_ok && (!draining_ || !dp_.script.empty());
        if (in_penalty_ || draining_) {
            if (i_can && !ip_.script.empty()) { iv = true; irq = ip_.script.front(); ip_.script.pop_front(); }
            if (d_can && !dp_.script.empty()) { dv = true; drq = dp_.script.front(); dp_.script.pop_front(); }
            return;
        }
        auto take_d = [&]() {
            if (!dp_.script.empty()) { drq = dp_.script.front(); dp_.script.pop_front(); }
            else drq = gen_d();
            dv = true;
        };
        switch (ph_.cp) {
            case CP_INDEP:
                if (i_can) { iv = true; irq = gen_i(); }
                if (d_can) take_d();
                break;
            case CP_LOCKSTEP:
                // CPU-like global stall: issue only when both ports are ready
                if (ir && dr && i_can && cyc_ >= dp_.next_ok) {
                    iv = true; irq = gen_i();
                    if (!dp_.script.empty() || rng_.chance(45)) take_d();
                }
                break;
            case CP_I_ONLY:
                if (i_can) { iv = true; irq = gen_i(); }
                if (d_can && !dp_.script.empty()) take_d();
                break;
            case CP_D_ONLY:
                if (d_can) take_d();
                break;
            default: break;
        }
    }

    // ------------------------------------------------------------------ end
    bool pending() const { return (ip_.busy || dp_.busy || !ip_.script.empty() || !dp_.script.empty()) && !fatal_; }
    static void clear_lat(Port& p) { for (auto& l : p.lat) for (auto& x : l) x = Lat(); }
    void drain() {
        draining_ = true;
        uint64_t guard = 0;
        while (pending()) {
            cycle();
            if (++guard > 5'000'000) { error("drain timed out"); break; }
        }
    }

    void finish() {
        drain();
        // flush every D-cache line by conflict loads from a region never used before
        for (unsigned i = 0; i < kDLines; ++i)
            dp_.script.push_back(make_load(g_reg[R_FLUSH].base + i * 32 + rng_.below(8) * 4));
        drain();
        for (int i = 0; i < 200; ++i) cycle();      // last write-back
        if (dsh_.dirty_lines()) error("shadow cache still has %u dirty lines after flush", dsh_.dirty_lines());
        // whole backing store
        uint64_t bad = 0;
        for (uint32_t o = 0; o < kRamBytes; o += 4) {
            const uint32_t s = store_word(o), r = ref_word(o);
            if (s != r) {
                if (++bad <= 8) error("backing store word 0x%07x = 0x%08x, reference 0x%08x", o, s, r);
            }
        }
        if (ip_.perf_acc != ip_.n || dp_.perf_acc != dp_.n)
            error("perf access pulse count mismatch");
    }

    bool report() {
        const uint64_t viol = violations();
        const bool inj = inject_ >= 0;
        bool pass = errors_ == 0 && viol == 0;
        if (inj) pass = inject_check();
        printf("[%s] seed %-4" PRIu64 " ops %8" PRIu64 " (I %" PRIu64 ", D %" PRIu64 " ld / %" PRIu64 " st)  cycles %9" PRIu64
               "  I miss %" PRIu64 "  D miss %" PRIu64 " (%" PRIu64 " dirty)  late hits %" PRIu64 "/%" PRIu64
               "  b2b %" PRIu64 "/%" PRIu64 "  viol %" PRIu64 "  err %" PRIu64 "  %s\n",
               TB_CFG, seed_, ip_.n + dp_.n, ip_.n, dp_.n - dp_.n_store, dp_.n_store, cyc_,
               ip_.n - ip_.cls_n[C_HIT], dp_.n - dp_.cls_n[C_HIT], dp_.cls_n[C_MISS_DIRTY], ip_.n_late_hit, dp_.n_late_hit,
               ip_.b2b, dp_.b2b, viol, errors_,
               pass ? "PASS" : "FAIL");
#if !TB_FAST
        if (viol && (!pass || inj || verbose_)) sdram_->print_violations(stdout, inj ? 6 : 20);
#endif
        return pass;
    }

    bool inject_check() {
#if TB_FAST
        return false;
#else
        const SdramModel::Kind k = kInject[inject_].kind;
        const bool ok = sdram_->violations(k) > 0;
        printf("[%s] inject %-10s -> %" PRIu64 " violation(s) of kind %s (%s), %" PRIu64 " data/contract error(s): %s\n",
               TB_CFG, kInject[inject_].name, sdram_->violations(k), SdramModel::kind_name(k),
               ok ? "detected" : "NOT detected", errors_, ok ? "PASS" : "FAIL");
        return ok;
#endif
    }

public:
    // ------------------------------------------------------------------ results
    uint64_t errors_ = 0, ops_done_ = 0, cyc_ = 0;
    uint64_t violations() const {
#if TB_FAST
        return 0;
#else
        return sdram_->violations();
#endif
    }
    uint64_t ops() const { return ip_.n + dp_.n; }

private:
    Rng rng_;
    uint64_t seed_, ops_target_;
    int inject_;                    // index into kInject, -1 = none
    bool verbose_;
    std::unique_ptr<VerilatedContext> ctx_;
    std::unique_ptr<Vmemsys> top_;
    std::unique_ptr<SdramModel> sdram_;
    uint8_t* store_ = nullptr;
    std::vector<uint8_t> ref_;
    std::unordered_map<uint32_t, std::vector<uint32_t>> hist_;
    Shadow ish_, dsh_;
    Port ip_, dp_;
    bool fatal_ = false, draining_ = false, in_penalty_ = false;
    unsigned penalty_gap_ = 0;
    uint64_t inj_cnt_ = 0, inj_at_ = ~0ull;
};

// ---------------------------------------------------------------------------
// Miss-penalty measurement: one port at a time, long gaps (idle backend).
bool Bench::run_penalty() {
    preload();
    reset();
    in_penalty_ = true;
    ph_.cp = CP_INDEP;
    // wait for initialisation (SDRAM power-up) with one warm-up miss per port
    penalty_gap_ = 0;
    dp_.script.push_back(make_load(g_reg[R_DATA].base));
    while (!dp_.any || dp_.busy) cycle();
    for (int i = 0; i < 100; ++i) cycle();
    clear_lat(ip_);
    clear_lat(dp_);

    const unsigned N = 200;
    penalty_gap_ = 60;
    auto run_scripts = [&]() {
        while (pending()) cycle();
        for (int i = 0; i < 80; ++i) cycle();
    };
    // I-cache misses (all different lines, invalid or clean victims)
    for (unsigned i = 0; i < N; ++i) {
        Req r; r.addr = 0x80000000u | (g_reg[R_CODE].base + i * 32 * 3 % 0x10000 + (i % 8) * 4);
        ip_.script.push_back(r);
    }
    run_scripts();
    // I-cache hits
    for (unsigned i = 0; i < N; ++i) { Req r; r.addr = 0x80000000u | (i * 4 % 0x100); ip_.script.push_back(r); }
    run_scripts();
    // a miss followed by the other 7 words of its line, back to back (hit under fill)
    auto stream = [&](bool d, uint32_t base) {
        penalty_gap_ = 0;
        const unsigned L = 50;
        uint64_t total = 0;
        for (unsigned i = 0; i < L; ++i) {
            const uint64_t c0 = cyc_;
            for (unsigned w = 0; w < 8; ++w) {
                const uint32_t off = base + i * 64 + ((w + 3) & 7) * 4;
                if (d) dp_.script.push_back(make_load(off));
                else { Req r; r.addr = 0x80000000u | off; ip_.script.push_back(r); }
            }
            while (pending()) cycle();
            total += cyc_ - c0;
            for (int k = 0; k < 80; ++k) cycle();
        }
        penalty_gap_ = 60;
        return (double)total / L;
    };
    const double i_stream = stream(false, g_reg[R_CODE].base + 0x8000);
    const double d_stream = stream(true, g_reg[R_DATA].base + 0x30000);
    // D: sets of lines with identical indices and distinct tags, in turn: fill (clean), clean victims,
    // store misses with clean victims, load misses with dirty victims, store misses (clean victims),
    // store misses with dirty victims, hits
    const uint32_t D = TB_DCACHE_BYTES;
    auto line = [&](unsigned set, unsigned i) { return g_reg[R_DATA].base + set * D + (i * 32) % D + (i % 8) * 4; };
    const unsigned M = std::min<unsigned>(N, kDLines);
    static const struct { unsigned set; bool store; } dsets[] = {{0, 0}, {1, 0}, {2, 1}, {3, 0}, {4, 1}, {5, 1}, {5, 0}};
    for (auto& ds : dsets) {
        for (unsigned i = 0; i < M; ++i) dp_.script.push_back(ds.store ? make_store(line(ds.set, i)) : make_load(line(ds.set, i)));
        run_scripts();
    }

    printf("[%s] miss penalties (latency = cycles from accept to completion; penalty = latency - 1)\n", TB_CFG);
    auto show = [&](const char* what, const Lat& l) {
        if (!l.n) return;
        printf("  %-38s n=%4" PRIu64 "  latency min %3" PRIu64 "  median %3" PRIu64 "  max %3" PRIu64
               "  -> penalty %" PRIu64 " cycles\n", what, l.n, l.mn, l.median(), l.mx, l.median() - 1);
    };
    show("I fetch hit", ip_.lat[C_HIT][0]);
    show("I fetch miss (invalid victim)", ip_.lat[C_MISS_INV][0]);
    show("I fetch miss (valid victim)", ip_.lat[C_MISS_CLEAN][0]);
    show("D load hit", dp_.lat[C_HIT][0]);
    show("D load miss, invalid victim", dp_.lat[C_MISS_INV][0]);
    show("D load miss, clean victim", dp_.lat[C_MISS_CLEAN][0]);
    show("D load miss, dirty victim (write-back)", dp_.lat[C_MISS_DIRTY][0]);
    show("D store miss, clean victim", dp_.lat[C_MISS_CLEAN][1]);
    show("D store miss, dirty victim (write-back)", dp_.lat[C_MISS_DIRTY][1]);
    printf("  a miss and the other 7 words of its line, back to back: I %.1f cycles, D loads %.1f cycles\n",
           i_stream, d_stream);

    // saturated stream of clean misses: refresh effect on throughput
    penalty_gap_ = 0;
    clear_lat(dp_);
#if !TB_FAST
    const uint64_t c0 = cyc_;
    const uint64_t r0 = sdram_->stats().refresh;
#endif
    const unsigned S = 40000;
    for (unsigned i = 0; i < S; ++i) dp_.script.push_back(make_load(g_reg[R_DATA].base + (i * 32) % 0x40000));
    run_scripts();
    const Lat& l = dp_.lat[C_MISS_CLEAN][0];
    printf("  saturated clean-miss stream: %" PRIu64 " misses, latency min %" PRIu64 " avg %.3f max %" PRIu64,
           l.n, l.mn, l.n ? (double)l.sum / l.n : 0.0, l.mx);
#if !TB_FAST
    const uint64_t refs = sdram_->stats().refresh - r0, cycles = cyc_ - c0;
    printf("; %" PRIu64 " refreshes in %" PRIu64 " cycles (one per %.1f), avg latency +%.2f%% over min\n", refs,
           cycles, refs ? (double)cycles / refs : 0.0, l.n ? 100.0 * ((double)l.sum / l.n - l.mn) / l.mn : 0.0);
#else
    printf("\n");
#endif
    finish();
    const bool pass = errors_ == 0 && violations() == 0;
    printf("[%s] penalty run: errors %" PRIu64 ", violations %" PRIu64 ": %s\n", TB_CFG, errors_, violations(),
           pass ? "PASS" : "FAIL");
    return pass;
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    uint64_t s0 = 1, s1 = 1, ops = 200000;
    int inject = -1;
    bool verbose = false, penalty = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> std::string { if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", a.c_str()); exit(2); } return argv[++i]; };
        if (a == "--seed") { s0 = s1 = strtoull(val().c_str(), nullptr, 0); }
        else if (a == "--seeds") {
            std::string v = val();
            const size_t d = v.find('-');
            s0 = strtoull(v.substr(0, d).c_str(), nullptr, 0);
            s1 = d == std::string::npos ? s0 : strtoull(v.substr(d + 1).c_str(), nullptr, 0);
        }
        else if (a == "--ops") ops = strtoull(val().c_str(), nullptr, 0);
        else if (a == "--inject") {
            const std::string k = val();
            for (int j = 0; j < 4; ++j) if (k == kInject[j].name) inject = j;
            if (inject < 0) { fprintf(stderr, "unknown --inject %s\n", k.c_str()); return 2; }
        }
        else if (a == "--penalty") penalty = true;
        else if (a == "-v") verbose = true;
        else if (a.rfind("+verilator", 0) == 0) {}
        else { fprintf(stderr, "usage: %s [--seeds A-B] [--ops N] [--inject refresh|act|contention|powerup] [--penalty] [-v]\n", argv[0]); return 2; }
    }
    init_regions();
    if (penalty) {
        Bench b(s0, 0, -1, verbose);
        return b.run_penalty() ? 0 : 1;
    }
    unsigned fails = 0, runs = 0;
    uint64_t tot_ops = 0, tot_cyc = 0, tot_err = 0, tot_viol = 0;
    for (uint64_t s = s0; s <= s1; ++s) {
        Bench b(s, ops, inject, verbose);
        const bool ok = b.run_random();
        ++runs;
        fails += !ok;
        tot_ops += b.ops();
        tot_cyc += b.cyc_;
        tot_err += b.errors_;
        tot_viol += b.violations();
    }
    if (inject < 0)
        printf("[%s] TOTAL %u seed(s), %" PRIu64 " ops checked, %" PRIu64 " cycles, %" PRIu64 " SDRAM violations, %" PRIu64
               " errors: %s\n", TB_CFG, runs, tot_ops, tot_cyc, tot_viol, tot_err, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
