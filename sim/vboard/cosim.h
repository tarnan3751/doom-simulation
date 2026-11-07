// cosim.h - lockstep comparison of the RTL retire trace against a reference. One Cosim lives inside an RTL
// engine and is only touched when --cosim is on.
//   --cosim        reference = the ISS (sim/iss/README.md, "Override rules"): content of every retired
//                  instruction and every frame; MMIO and counter reads take the RTL's values.
//   --cosim=model  reference = the cycle-exact model (sim/model, accurate engine only), no overrides: every
//                  retired instruction including the cycle it retires in and all eleven counters in that cycle,
//                  every frame and serial byte with its cycle, and counters, halt and exit code at every slice end.
#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "engine.h"
#include "iss.h"
#include "model.h"

namespace vboard {

class Cosim {
public:
    Cosim(uint32_t clk_hz, uint32_t sys_flags);   // against the ISS
    Cosim();                                       // against the model

    simc::WriteFn writer();                     // preload the reference's RAM (same bytes as the RTL gets)
    void start(uint32_t entry);
    void inject_at(uint64_t n) { inject_ = n; }   // test hook (--cosim-inject): ISS pc off by 4 / model cycle off by 1
    void key(bool pressed, uint8_t code, uint64_t cycle);   // the RTL drives the event in `cycle`

    // One retired/trapped instruction from the RTL trace in `cycle`, with the RTL's counters in that cycle.
    // Returns false on the first mismatch (report() then holds the explanation).
    bool retire(const RetireTrace& t, uint64_t cycle, const model::Counters& cnt);

    // The RTL presented a frame (VID_FLIP) / its serial receiver decoded a byte in `cycle`.
    void rtl_flip(const uint8_t* fb, const uint32_t* pal, uint64_t cycle);
    void rtl_serial(uint8_t c, uint64_t cycle);
    // End of an engine slice (model only): the model must reach this cycle with no further retire and agree.
    void sync(const model::Counters& cnt, bool halted, uint32_t exit_code);

    bool failed() const { return failed_; }
    const std::string& report() const { return report_; }
    uint64_t compared() const { return count_; }
    uint64_t frames_compared() const { return frames_ok_; }

private:
    struct Hist { RetireTrace t; uint64_t cycle; uint64_t index; };
    struct Frame { std::vector<uint8_t> fb; std::vector<uint32_t> pal; uint64_t cycle; };
    static bool needs_override(const RetireTrace& t);
    static Frame frame(const uint8_t* fb, const uint32_t* pal, uint64_t cycle);
    void fail_retire(const RetireTrace& t, const iss::Retire& r, uint64_t cycle, unsigned diff_mask, const char* why,
                     uint64_t ref_cycle = 0, const model::Counters* rtl = nullptr, const model::Counters* ref = nullptr,
                     bool injected = false);
    void fail(const std::string& what);
    void append_history(std::string& out) const;
    void match_frames();
    void match_serial();

    iss::Iss iss_;
    std::unique_ptr<model::Machine> mdl_;
    const char* ref_name_;
    uint64_t inject_ = ~0ull;
    uint64_t count_ = 0, frames_ok_ = 0;
    bool failed_ = false;
    std::string report_;
    static constexpr unsigned kHist = 32;
    Hist hist_[kHist];
    std::deque<Frame> rtl_frames_, ref_frames_;
    std::deque<std::pair<uint8_t, uint64_t>> rtl_ser_, ref_ser_;
};

}  // namespace vboard
