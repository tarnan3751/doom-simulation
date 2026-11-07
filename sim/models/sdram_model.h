// Cycle-accurate behavioural model of a single-data-rate SDRAM chip
// (32-bit bus, 4 banks, row_bits x col_bits) with protocol / timing checks.
//
// Contract: docs/ARCHITECTURE.md section 6. One tick() per clock cycle, made
// *before* the rising-edge eval(), with the pin values the DUT is driving
// (= the values the chip samples at that edge). tick() returns the value of
// sdram_dq_i for the cycle *after* that edge:
//
//     uint32_t dq_next = sdram.tick(pins_from(top));
//     top->clk = 1; top->eval();
//     top->sdram_dq_i = dq_next;
//     top->clk = 0; top->eval();
//
// Data timing: a READ sampled at edge k returns beat n from the tick() of edge
// k + CL - 1 + n (so the controller's input flops capture it at k + CL + n).
// A WRITE sampled at edge k writes beat n from the pins passed to the tick()
// of edge k + n. DQM masks write lanes with latency 0 and read lanes with
// latency 2 (DQM sampled at edge e blanks the data captured at edge e + 2).
// Undriven bus cycles / masked lanes read as kUndriven.
//
// Storage: 4 << (row_bits + col_bits) 32-bit words, allocated once, little
// endian, indexed by the word index {ba, row, col} - so byte offset N of RAM
// is bytes()[N] (word N >> 2, lane N & 3).
//
// Checks (each violation is counted; the first ones are logged with the
// cycle number): power-up delay, init sequence (PRECHARGE ALL, >= N AUTO
// REFRESH, MODE REGISTER SET), commands before initialisation, tRP, tRCD,
// tRAS, tRC, tRFC, tRRD, tWR, tMRD, ACTIVE to an open bank, READ/WRITE to a
// bank without an open row, REFRESH/MRS with open banks, commands to a bank
// with auto-precharge in progress, bursts interrupted while auto-precharging
// (or precharged mid-burst), DQ driven by the controller while the chip drives
// it, write beats with DQ not driven, unsupported mode register values, CKE
// low after initialisation (power-down / self-refresh are not modelled), and
// the refresh rate over time (more than `refresh_slack` refreshes owed).
//
// Standard C++17, no dependencies.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct SdramPins {
    bool     cke, cs_n, ras_n, cas_n, we_n;
    uint8_t  ba;
    uint16_t addr;
    uint8_t  dqm;
    uint32_t dq_o;
    bool     dq_oe;
};

// Datasheet-style values; SdramTiming::from_ns() turns them into cycle limits.
// Defaults = the table of ARCHITECTURE.md section 6.
struct SdramTimingNs {
    double   power_up_us       = 200.0;
    double   tRP_ns            = 18.0;
    double   tRCD_ns           = 18.0;
    double   tRAS_ns           = 42.0;
    double   tRC_ns            = 60.0;
    double   tRFC_ns           = 60.0;
    double   tRRD_ns           = 0.0;     // not in the table: unchecked
    unsigned tWR_clk           = 2;
    unsigned tMRD_clk          = 2;
    double   refresh_period_ms = 64.0;
    unsigned refresh_count     = 4096;
    unsigned cas_latency       = 2;
    unsigned burst_length      = 8;
};

// Timing limits in clock cycles. The default-constructed value equals
// SdramTiming::from_ns(50'000'000).
struct SdramTiming {
    uint32_t power_up       = 10000;  // cycles of NOP/DESELECT before the first command
    uint32_t tRP            = 1;      // PRECHARGE -> next command to that bank (ACT/REF/MRS)
    uint32_t tRCD           = 1;      // ACTIVE -> READ/WRITE
    uint32_t tRAS           = 3;      // ACTIVE -> PRECHARGE (min)
    uint32_t tRC            = 3;      // ACTIVE -> ACTIVE (same bank), ACTIVE -> REFRESH
    uint32_t tRFC           = 3;      // REFRESH -> ACTIVE/REFRESH/MRS
    uint32_t tRRD           = 0;      // ACTIVE -> ACTIVE (other bank), 0 = unchecked
    uint32_t tWR            = 2;      // last write data -> PRECHARGE
    uint32_t tMRD           = 2;      // MRS -> any command
    uint32_t tREFI          = 781;    // average refresh interval, 0 = unchecked
    uint32_t refresh_slack  = 8;      // refreshes that may be owed at any time
    uint32_t init_refreshes = 2;      // AUTO REFRESH required between PRECHARGE ALL and MRS
    uint32_t expect_cl      = 2;      // CL the MRS must program (0 = any supported: 2, 3)
    uint32_t expect_bl      = 8;      // BL the MRS must program (0 = any supported: 1, 2, 4, 8)

    static SdramTiming from_ns(uint64_t clk_hz, const SdramTimingNs& ns = SdramTimingNs());
};

class SdramModel {
public:
    enum Kind : unsigned {
        kPowerUp, kInitSeq, kNotInit, kCkeLow, kTRP, kTRCD, kTRAS, kTRC, kTRFC, kTRRD,
        kTWR, kTMRD, kBankActive, kBankIdle, kBanksOpen, kAutoPre, kBurstInterrupt,
        kContention, kWriteNoData, kModeReg, kRefresh, kNumKinds
    };
    static const char* kind_name(unsigned k);

    static constexpr uint32_t kUndriven = 0xDEADBEEFu;   // value of an undriven bus

    struct Stats {
        uint64_t act = 0, read = 0, write = 0, precharge = 0, refresh = 0, mrs = 0,
                 bst = 0, beats_read = 0, beats_written = 0;
    };

    SdramModel(unsigned row_bits, unsigned col_bits, const SdramTiming& t = SdramTiming());

    // One call per rising edge (see header comment).
    uint32_t tick(const SdramPins& p) {
        const uint64_t t = cycle_++;
        const bool idle_cmd = p.cs_n || (p.ras_n && p.cas_n && p.we_n);
        if (__builtin_expect(idle_cmd && p.cke && t < next_event_, 1)) {
            dqm_prev_ = p.dqm;
            return kUndriven;
        }
        return tick_slow(p, t);
    }

    uint8_t* bytes() { return mem_.data(); }
    size_t   size_bytes() const { return mem_.size(); }

    uint64_t violations() const { return violations_; }
    uint64_t violations(Kind k) const { return kind_count_[k]; }
    const std::vector<std::string>& violation_log() const { return log_; }
    void     set_log_limit(size_t n) { log_limit_ = n; }
    void     print_violations(FILE* f, size_t max_lines = 20) const;

    bool     initialized() const { return init_ == Init::Ready; }
    const Stats& stats() const { return stats_; }
    const SdramTiming& timing() const { return tim_; }
    unsigned cas_latency() const { return cl_; }
    unsigned burst_length() const { return bl_; }

private:
    enum class Init { WaitPrecharge, WaitRefresh, Ready };
    struct Bank {
        bool     active = false;
        uint32_t row    = 0;
        int64_t  t_act  = kLongAgo;   // last ACTIVE
        int64_t  t_pre  = kLongAgo;   // last precharge start (explicit or auto)
        int64_t  t_wr   = kLongAgo;   // last write beat in this activation
        bool     ap     = false;      // auto-precharge scheduled
        int64_t  t_ap   = 0;          // ... starting at this cycle
    };
    struct OutSlot {
        uint64_t cycle = ~0ull;
        uint32_t word  = 0;
    };
    static constexpr int64_t kLongAgo = -(int64_t(1) << 40);

    uint32_t tick_slow(const SdramPins& p, uint64_t t);
    void     violation(Kind k, uint64_t t, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
    void     settle(Bank& b, int64_t t);
    void     check_since(Kind k, int64_t t, int64_t since, uint32_t lim, const char* what);
    uint32_t word_index(unsigned ba, uint32_t row, uint32_t col) const {
        return (uint32_t(ba) << (row_bits_ + col_bits_)) | (row << col_bits_) | col;
    }
    uint32_t burst_col(uint32_t start, unsigned n) const;
    void     cmd_activate(const SdramPins& p, int64_t t);
    void     cmd_read_write(const SdramPins& p, int64_t t, bool write);
    void     cmd_precharge(const SdramPins& p, int64_t t);
    void     cmd_refresh(int64_t t);
    void     cmd_mode(const SdramPins& p, int64_t t);
    void     cmd_burst_stop(int64_t t);
    void     cancel_outputs_from(uint64_t c);
    void     refresh_deadline_hit(uint64_t t);
    void     update_refresh_deadline();
    void     update_next_event(uint64_t t);

    const unsigned row_bits_, col_bits_;
    const SdramTiming tim_;
    std::vector<uint8_t> mem_;

    uint64_t cycle_      = 0;
    uint64_t next_event_ = 0;     // first tick that must take the slow path
    uint8_t  dqm_prev_   = 0;     // DQM sampled at the previous edge (read mask latency 2)
    bool     drove_last_ = false; // chip drove DQ during the current cycle
    bool     any_command_ = false;
    bool     cke_low_reported_ = false;

    Init     init_ = Init::WaitPrecharge;
    unsigned init_refs_ = 0;
    unsigned bl_ = 8, cl_ = 2;
    bool     interleave_ = false, single_write_ = false;

    Bank     bank_[4];
    int64_t  t_ref_ = kLongAgo, t_mrs_ = kLongAgo, t_act_any_ = kLongAgo;

    // internal burst (column accesses), one at a time
    bool     burst_ = false, burst_write_ = false, burst_ap_ = false;
    unsigned burst_bank_ = 0, burst_len_ = 0;
    uint32_t burst_row_ = 0, burst_start_ = 0;
    int64_t  burst_k_ = 0;

    // read data output schedule (indexed by cycle & 15)
    OutSlot  out_[16];
    uint64_t out_until_ = 0;      // last scheduled output cycle (valid if out_any_)
    bool     out_any_ = false;

    // refresh rate accounting (from the end of initialisation)
    bool     ref_track_ = false;
    uint64_t ref_t0_ = 0, ref_done_ = 0, ref_deadline_ = ~0ull;

    uint64_t violations_ = 0;
    uint64_t kind_count_[kNumKinds] = {};
    std::vector<std::string> log_;
    size_t   log_limit_ = 64;
    Stats    stats_;
};
