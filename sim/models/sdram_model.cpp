// SDRAM chip model - see sdram_model.h for the contract.
#include "sdram_model.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <stdexcept>

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "sdram_model assumes a little-endian host (storage is little endian)"
#endif

// ---------------------------------------------------------------------------
// timing helpers

static uint32_t ps_to_cycles_ceil(double ns, uint64_t clk_hz) {
    // integer arithmetic on picoseconds so the result matches the RTL exactly
    const uint64_t ps = (uint64_t)std::llround(ns * 1000.0);
    const uint64_t num = ps * clk_hz;
    return (uint32_t)((num + 999'999'999'999ull) / 1'000'000'000'000ull);
}

SdramTiming SdramTiming::from_ns(uint64_t clk_hz, const SdramTimingNs& ns) {
    SdramTiming t;
    auto atleast1 = [](uint32_t v) { return v ? v : 1u; };
    t.power_up = ps_to_cycles_ceil(ns.power_up_us * 1000.0, clk_hz);
    t.tRP      = atleast1(ps_to_cycles_ceil(ns.tRP_ns, clk_hz));
    t.tRCD     = atleast1(ps_to_cycles_ceil(ns.tRCD_ns, clk_hz));
    t.tRAS     = atleast1(ps_to_cycles_ceil(ns.tRAS_ns, clk_hz));
    t.tRC      = atleast1(ps_to_cycles_ceil(ns.tRC_ns, clk_hz));
    t.tRFC     = atleast1(ps_to_cycles_ceil(ns.tRFC_ns, clk_hz));
    t.tRRD     = ns.tRRD_ns > 0 ? ps_to_cycles_ceil(ns.tRRD_ns, clk_hz) : 0;
    t.tWR      = ns.tWR_clk;
    t.tMRD     = ns.tMRD_clk;
    // average refresh interval, rounded down (a controller meeting it is safe)
    t.tREFI = (uint32_t)((double)clk_hz * ns.refresh_period_ms / 1000.0 / ns.refresh_count);
    t.expect_cl = ns.cas_latency;
    t.expect_bl = ns.burst_length;
    return t;
}

const char* SdramModel::kind_name(unsigned k) {
    static const char* const names[] = {
        "power-up", "init-sequence", "not-initialized", "cke-low", "tRP", "tRCD", "tRAS",
        "tRC", "tRFC", "tRRD", "tWR", "tMRD", "bank-active", "bank-idle", "banks-open", "auto-precharge",
        "burst-interrupt", "dq-contention", "write-no-data", "mode-register", "refresh-rate"};
    static_assert(sizeof names / sizeof names[0] == kNumKinds, "kind names out of sync");
    return k < kNumKinds ? names[k] : "?";
}

// ---------------------------------------------------------------------------

SdramModel::SdramModel(unsigned row_bits, unsigned col_bits, const SdramTiming& t)
    : row_bits_(row_bits), col_bits_(col_bits), tim_(t) {
    if (row_bits < 11 || row_bits > 16 || col_bits < 3 || col_bits > 10)
        throw std::invalid_argument("SdramModel: unsupported geometry");
    mem_.assign((size_t(4) << (row_bits + col_bits)) * 4, 0);
    next_event_ = 0;   // the first tick goes through the slow path
}

void SdramModel::violation(Kind k, uint64_t t, const char* fmt, ...) {
    ++violations_;
    ++kind_count_[k];
    if (log_.size() < log_limit_) {
        char buf[256];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        char line[320];
        snprintf(line, sizeof line, "cycle %llu: [%s] %s", (unsigned long long)t, kind_name(k), buf);
        log_.emplace_back(line);
    }
}

void SdramModel::print_violations(FILE* f, size_t max_lines) const {
    fprintf(f, "SDRAM model: %llu violation(s)", (unsigned long long)violations_);
    if (violations_) {
        fprintf(f, " [");
        bool first = true;
        for (unsigned k = 0; k < kNumKinds; ++k)
            if (kind_count_[k]) {
                fprintf(f, "%s%s=%llu", first ? "" : " ", kind_name(k), (unsigned long long)kind_count_[k]);
                first = false;
            }
        fprintf(f, "]");
    }
    fprintf(f, "\n");
    for (size_t i = 0; i < log_.size() && i < max_lines; ++i) fprintf(f, "  %s\n", log_[i].c_str());
}

void SdramModel::settle(Bank& b, int64_t t) {
    if (b.ap && t >= b.t_ap) {
        b.ap = false;
        b.active = false;
        b.t_pre = b.t_ap;
    }
}

void SdramModel::check_since(Kind k, int64_t t, int64_t since, uint32_t lim, const char* what) {
    if (lim && t - since < (int64_t)lim)
        violation(k, (uint64_t)t, "%s after %lld cycle(s), need %u", what, (long long)(t - since), lim);
}

uint32_t SdramModel::burst_col(uint32_t start, unsigned n) const {
    if (bl_ == 1) return start;
    const uint32_t m = bl_ - 1;
    const uint32_t low = interleave_ ? ((start ^ n) & m) : ((start + n) & m);
    return (start & ~m) | low;
}

void SdramModel::cancel_outputs_from(uint64_t c) {
    for (auto& s : out_)
        if (s.cycle != ~0ull && s.cycle >= c) s.cycle = ~0ull;
}

// ---------------------------------------------------------------------------
// commands

void SdramModel::cmd_activate(const SdramPins& p, int64_t t) {
    const unsigned b = p.ba & 3;
    Bank& bk = bank_[b];
    settle(bk, t);
    ++stats_.act;
    if (init_ != Init::Ready) violation(kNotInit, t, "ACTIVE bank %u before MODE REGISTER SET", b);
    if (bk.active) violation(kBankActive, t, "ACTIVE to bank %u while row %u is open", b, bk.row);
    check_since(kTRP, t, bk.t_pre, tim_.tRP, "PRECHARGE -> ACTIVE");
    check_since(kTRC, t, bk.t_act, tim_.tRC, "ACTIVE -> ACTIVE (same bank)");
    check_since(kTRFC, t, t_ref_, tim_.tRFC, "REFRESH -> ACTIVE");
    if (tim_.tRRD) check_since(kTRRD, t, t_act_any_, tim_.tRRD, "ACTIVE -> ACTIVE (other bank)");
    bk.active = true;
    bk.ap = false;
    bk.row = p.addr & ((1u << row_bits_) - 1);
    bk.t_act = t;
    bk.t_wr = kLongAgo;
    t_act_any_ = t;
}

void SdramModel::cmd_read_write(const SdramPins& p, int64_t t, bool write) {
    const unsigned b = p.ba & 3;
    Bank& bk = bank_[b];
    settle(bk, t);
    const char* name = write ? "WRITE" : "READ";
    if (write) ++stats_.write; else ++stats_.read;
    if (init_ != Init::Ready) violation(kNotInit, t, "%s before MODE REGISTER SET", name);
    if (!bk.active)
        violation(kBankIdle, t, "%s to bank %u without an active row", name, b);
    else
        check_since(kTRCD, t, bk.t_act, tim_.tRCD, "ACTIVE -> READ/WRITE");
    if (bk.ap) violation(kAutoPre, t, "%s to bank %u while its auto-precharge is pending", name, b);
    if (burst_ && t < burst_k_ + (int64_t)burst_len_ && burst_ap_)
        violation(kBurstInterrupt, t, "%s interrupts a burst with auto-precharge", name);

    const uint32_t col = p.addr & ((1u << col_bits_) - 1);
    const bool ap = (p.addr >> 10) & 1;
    const unsigned len = (write && single_write_) ? 1 : bl_;

    if (write) cancel_outputs_from((uint64_t)t);   // a WRITE stops read data output

    burst_ = true;
    burst_write_ = write;
    burst_ap_ = ap;
    burst_bank_ = b;
    burst_row_ = bk.row;
    burst_start_ = col;
    burst_len_ = len;
    burst_k_ = t;

    if (!write) {
        for (unsigned n = 0; n < len; ++n) {
            const uint64_t c = (uint64_t)t + cl_ - 1 + n;
            out_[c & 15].cycle = c;
            out_[c & 15].word = word_index(b, bk.row, burst_col(col, n));
        }
        const uint64_t last = (uint64_t)t + cl_ - 1 + len - 1;
        out_until_ = out_any_ ? std::max(out_until_, last) : last;
        out_any_ = true;
    }
    if (ap && bk.active) {
        bk.ap = true;
        const int64_t ras_ok = bk.t_act + tim_.tRAS;
        bk.t_ap = write ? std::max<int64_t>(t + len - 1 + tim_.tWR, ras_ok)
                        : std::max<int64_t>(t + len, ras_ok);
    }
}

void SdramModel::cmd_precharge(const SdramPins& p, int64_t t) {
    const bool all = (p.addr >> 10) & 1;
    ++stats_.precharge;
    if (init_ == Init::WaitPrecharge) {
        if (all) init_ = Init::WaitRefresh;
        else violation(kInitSeq, t, "initialisation must start with PRECHARGE ALL (got bank %u)", p.ba & 3u);
    }
    for (unsigned b = 0; b < 4; ++b) {
        if (!all && b != (p.ba & 3u)) continue;
        Bank& bk = bank_[b];
        settle(bk, t);
        if (bk.ap) {
            violation(kAutoPre, t, "PRECHARGE to bank %u while its auto-precharge is pending", b);
            continue;
        }
        if (!bk.active) continue;   // precharging an idle bank is a NOP
        check_since(kTRAS, t, bk.t_act, tim_.tRAS, "ACTIVE -> PRECHARGE");
        check_since(kTWR, t, bk.t_wr, tim_.tWR, "last write data -> PRECHARGE");
        if (burst_ && burst_bank_ == b && t < burst_k_ + (int64_t)burst_len_)
            violation(kBurstInterrupt, t, "PRECHARGE interrupts a burst to bank %u", b);
        bk.active = false;
        bk.t_pre = t;
    }
}

void SdramModel::cmd_refresh(int64_t t) {
    ++stats_.refresh;
    if (init_ == Init::WaitPrecharge) violation(kInitSeq, t, "AUTO REFRESH before PRECHARGE ALL");
    for (unsigned b = 0; b < 4; ++b) {
        Bank& bk = bank_[b];
        settle(bk, t);
        if (bk.active) violation(kBanksOpen, t, "AUTO REFRESH while bank %u is open", b);
        check_since(kTRP, t, bk.t_pre, tim_.tRP, "PRECHARGE -> REFRESH");
        check_since(kTRC, t, bk.t_act, tim_.tRC, "ACTIVE -> REFRESH");
    }
    check_since(kTRFC, t, t_ref_, tim_.tRFC, "REFRESH -> REFRESH");
    if (burst_ && t < burst_k_ + (int64_t)burst_len_)
        violation(kBurstInterrupt, t, "AUTO REFRESH during a burst");
    t_ref_ = t;
    if (init_ == Init::WaitRefresh) ++init_refs_;
    if (ref_track_ && tim_.tREFI) {
        const uint64_t required = ((uint64_t)t - ref_t0_) / tim_.tREFI;
        ref_done_ = std::min<uint64_t>(ref_done_ + 1, required + tim_.refresh_slack);
        update_refresh_deadline();
    }
}

void SdramModel::cmd_mode(const SdramPins& p, int64_t t) {
    ++stats_.mrs;
    if (init_ == Init::WaitPrecharge)
        violation(kInitSeq, t, "MODE REGISTER SET before PRECHARGE ALL");
    else if (init_ == Init::WaitRefresh && init_refs_ < tim_.init_refreshes)
        violation(kInitSeq, t, "MODE REGISTER SET after %u AUTO REFRESH, need %u", init_refs_, tim_.init_refreshes);
    for (unsigned b = 0; b < 4; ++b) {
        Bank& bk = bank_[b];
        settle(bk, t);
        if (bk.active) violation(kBanksOpen, t, "MODE REGISTER SET while bank %u is open", b);
        check_since(kTRP, t, bk.t_pre, tim_.tRP, "PRECHARGE -> MRS");
    }
    check_since(kTRFC, t, t_ref_, tim_.tRFC, "REFRESH -> MRS");
    if (burst_ && t < burst_k_ + (int64_t)burst_len_)
        violation(kBurstInterrupt, t, "MODE REGISTER SET during a burst");

    const unsigned a = p.addr;
    const unsigned blc = a & 7, clc = (a >> 4) & 7, op = (a >> 7) & 3;
    bool ok = true;
    unsigned bl = 0, cl = 0;
    if (blc <= 3) bl = 1u << blc;
    else { violation(kModeReg, t, "unsupported burst length code %u", blc); ok = false; }
    if (clc == 2 || clc == 3) cl = clc;
    else { violation(kModeReg, t, "unsupported CAS latency code %u", clc); ok = false; }
    if (op != 0) { violation(kModeReg, t, "reserved operating mode %u", op); ok = false; }
    if (ok && tim_.expect_cl && cl != tim_.expect_cl)
        violation(kModeReg, t, "CAS latency %u programmed, expected %u", cl, tim_.expect_cl);
    if (ok && tim_.expect_bl && bl != tim_.expect_bl)
        violation(kModeReg, t, "burst length %u programmed, expected %u", bl, tim_.expect_bl);
    if (ok) {
        bl_ = bl;
        cl_ = cl;
        interleave_ = (a >> 3) & 1;
        single_write_ = (a >> 9) & 1;
    }
    t_mrs_ = t;
    if (init_ != Init::Ready) {
        init_ = Init::Ready;
        ref_track_ = true;
        ref_t0_ = (uint64_t)t;
        ref_done_ = 0;
        update_refresh_deadline();
    }
}

void SdramModel::cmd_burst_stop(int64_t t) {
    ++stats_.bst;
    if (!(burst_ && t < burst_k_ + (int64_t)burst_len_)) return;
    if (burst_ap_) violation(kBurstInterrupt, t, "BURST TERMINATE of a burst with auto-precharge");
    if (!burst_write_) cancel_outputs_from((uint64_t)t + cl_ - 1);
    burst_ = false;
}

// ---------------------------------------------------------------------------
// refresh rate

void SdramModel::update_refresh_deadline() {
    if (!ref_track_ || !tim_.tREFI) { ref_deadline_ = ~0ull; return; }
    ref_deadline_ = ref_t0_ + (ref_done_ + tim_.refresh_slack + 1) * (uint64_t)tim_.tREFI;
}

void SdramModel::refresh_deadline_hit(uint64_t t) {
    while (t >= ref_deadline_) {
        const uint64_t required = (t - ref_t0_) / tim_.tREFI;
        violation(kRefresh, t, "refresh starvation: %llu refresh(es) owed (limit %u, interval %u cycles)",
                  (unsigned long long)(required - ref_done_), tim_.refresh_slack, tim_.tREFI);
        ++ref_done_;   // forgive one so a continuous starvation counts once per interval
        update_refresh_deadline();
    }
}

void SdramModel::update_next_event(uint64_t t) {
    const bool wr_burst = burst_ && burst_write_ && (int64_t)t + 1 < burst_k_ + (int64_t)burst_len_;
    if (out_any_ && out_until_ <= t) out_any_ = false;
    if (wr_burst || out_any_ || drove_last_) next_event_ = t + 1;
    else next_event_ = ref_deadline_;
}

// ---------------------------------------------------------------------------

uint32_t SdramModel::tick_slow(const SdramPins& p, uint64_t tu) {
    const int64_t t = (int64_t)tu;

    if (tu >= ref_deadline_) refresh_deadline_hit(tu);

    // the controller drives DQ during a cycle in which the chip outputs data
    if (p.dq_oe && drove_last_)
        violation(kContention, tu, "DQ driven by the controller while the chip outputs read data");

    bool idle_cmd = p.cs_n || (p.ras_n && p.cas_n && p.we_n);
    if (!p.cke) {
        // power-down / self-refresh are not modelled; CKE may only be low
        // before the first command (power-up)
        if (any_command_ && !cke_low_reported_) {
            violation(kCkeLow, tu, "CKE low after initialisation started (power-down/self-refresh unsupported)");
            cke_low_reported_ = true;
        }
        idle_cmd = true;
    } else {
        cke_low_reported_ = false;
    }

    if (!idle_cmd) {
        const unsigned c = (p.ras_n << 2) | (p.cas_n << 1) | p.we_n;
        static const char* const cname[8] = {"MRS", "REF", "PRE", "ACT", "WRITE", "READ", "BST", "NOP"};
        if (tu < tim_.power_up && !any_command_)
            violation(kPowerUp, tu, "%s issued %llu cycle(s) after power-up, need %u of NOP", cname[c],
                      (unsigned long long)tu, tim_.power_up);
        any_command_ = true;
        if (t - t_mrs_ < (int64_t)tim_.tMRD)
            violation(kTMRD, tu, "%s %lld cycle(s) after MODE REGISTER SET, need %u", cname[c],
                      (long long)(t - t_mrs_), tim_.tMRD);
        switch (c) {
            case 0b011: cmd_activate(p, t); break;
            case 0b101: cmd_read_write(p, t, false); break;
            case 0b100: cmd_read_write(p, t, true); break;
            case 0b010: cmd_precharge(p, t); break;
            case 0b001: cmd_refresh(t); break;
            case 0b000: cmd_mode(p, t); break;
            case 0b110: cmd_burst_stop(t); break;
            default: break;
        }
    }

    // write beat sampled at this edge
    if (burst_ && burst_write_) {
        const int64_t n = t - burst_k_;
        if (n >= 0 && n < (int64_t)burst_len_) {
            const uint32_t w = word_index(burst_bank_, burst_row_, burst_col(burst_start_, (unsigned)n));
            if (!p.dq_oe) {
                violation(kWriteNoData, tu, "write beat %lld with DQ not driven", (long long)n);
            } else {
                uint8_t* dst = &mem_[(size_t)w * 4];
                for (unsigned l = 0; l < 4; ++l)
                    if (!((p.dqm >> l) & 1)) dst[l] = (uint8_t)(p.dq_o >> (8 * l));
            }
            ++stats_.beats_written;
            bank_[burst_bank_].t_wr = t;
            if (n == (int64_t)burst_len_ - 1) burst_ = false;
        } else {
            burst_ = false;
        }
    } else if (burst_ && t >= burst_k_ + (int64_t)burst_len_ - 1) {
        burst_ = false;   // internal read burst done (data may still be in the output pipe)
    }

    // read data for the cycle after this edge
    uint32_t ret = kUndriven;
    bool drove = false;
    if (out_any_) {
        const OutSlot& s = out_[tu & 15];
        if (s.cycle == tu) {
            uint32_t w;
            memcpy(&w, &mem_[(size_t)s.word * 4], 4);
            const unsigned mask = dqm_prev_ & 0xF;
            if (mask) {
                for (unsigned l = 0; l < 4; ++l)
                    if ((mask >> l) & 1) {
                        const uint32_t lm = 0xFFu << (8 * l);
                        w = (w & ~lm) | (kUndriven & lm);
                    }
            }
            drove = mask != 0xF;
            ret = w;
            ++stats_.beats_read;
        }
    }
    drove_last_ = drove;
    dqm_prev_ = p.dqm;
    update_next_event(tu);
    return ret;
}
