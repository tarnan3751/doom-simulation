// engine.h - the one interface the board front end talks to (ISS, Verilator "fast" and "accurate", model).
// See README.md ("Engine interface").
//
// Units: "cycle" is the engine's own clock tick. For the ISS one cycle == one instruction
// (ARCHITECTURE.md section 11); for the RTL engines and the model it is one `clk` period.
#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "loader.h"

namespace vboard {

constexpr unsigned kFbWidth = 320, kFbHeight = 200, kFbBytes = kFbWidth * kFbHeight;
constexpr unsigned kMonWidth = 640, kMonHeight = 480;

// A counter an engine may not have. Unavailable fields are never faked.
struct Counter {
    uint64_t v = 0;
    bool ok = false;
    void set(uint64_t x) { v = x; ok = true; }
};

// Uniform statistics snapshot (all since reset).
struct Stats {
    Counter cycles, instret;
    Counter ic_access, ic_miss, dc_access, dc_miss;
    Counter branch, branch_miss, jump, jump_miss;
    Counter flips;
    Counter sdram_violations;                     // accurate engine only
    Counter serial_errors;                        // accurate: UART framing + bit-timing errors on the uart_tx pin
    Counter video_errors;                         // accurate: video timing deviations seen by the monitor
    Counter scan_checked, scan_skipped, scan_bad; // accurate: scan-out self-check (frames compared / skipped / mismatching)
    // Engine-specific extras, printed in the end-of-run summary and JSON only.
    std::vector<std::pair<std::string, uint64_t>> extras;
};

// Events returned by Engine::run (bit mask). run() returns as soon as any is set.
enum Event : unsigned {
    EvNone   = 0,
    EvFrame  = 1u << 0,   // at least one VID_FLIP happened during this call (frame sink already called)
    EvHalted = 1u << 1,   // program wrote SYS_EXIT (status().exit_code)
    EvFault  = 1u << 2,   // engine cannot continue (bad fetch/access, internal error); status().message
    EvTohost = 1u << 3,   // --isa-test: store to the tohost word (status().tohost)
    EvCheck  = 1u << 4,   // a hardware checker found an error (SDRAM protocol, serial, video, scan-out)
    EvCosim  = 1u << 5,   // lockstep comparison against the ISS failed (diagnostics())
};

struct RunResult {
    uint64_t cycles = 0;   // simulated cycles actually executed by this call (<= requested)
    unsigned events = EvNone;
};

enum class RunState { Running, Halted, Fault, Tohost, HwError, CosimMismatch };
struct Status {
    RunState state = RunState::Running;
    uint32_t exit_code = 0;
    uint32_t tohost = 0;
    uint32_t pc = 0;           // best effort, for diagnostics
    std::string message;       // one line, for Fault
};

enum class SpeedUnit { Mips, Mhz };   // how the HUD reports simulation speed

struct EngineInfo {
    std::string name;          // "iss", "fast", "accurate", "model"
    std::string description;   // "instruction-set simulator (1 cycle / instruction)"
    uint32_t clk_hz = 50000000;
    SpeedUnit speed_unit = SpeedUnit::Mips;
};

// The presented frame, valid only during the frame-sink call.
struct FrameView {
    const uint8_t* indices;    // kFbBytes palette indices, row-major
    const uint32_t* palette;   // 256 entries 0x00RRGGBB
    uint64_t number;           // 1-based count of frames presented so far
    uint64_t cycle;            // engine cycle of the VID_FLIP
};

// One retired instruction as seen on the RTL retire trace (--cosim, --isa-test).
struct RetireTrace {
    bool trap;
    uint32_t pc, insn, rd, rd_wdata, mem_addr, mem_wstrb, mem_wdata, next_pc;
};
void gather_fb(const uint8_t* const lanes[4], uint8_t* out);   // 4 byte-lane arrays of 16000 -> 64000 row-major indices

// Render the section 7 monitor picture: framebuffer pixel-doubled to 640x400 shown on active
// lines 40..439, black elsewhere. out: kMonWidth*kMonHeight words 0x00RRGGBB.
void synth_monitor(const uint8_t* fb, const uint32_t* pal, uint32_t* out);

// Options that shape an engine (all optional).
struct EngineOptions {
    bool cosim = false;           // lockstep ISS comparison (RTL engines)
    bool cosim_model = false;     // ... against the cycle-exact model instead (--cosim=model, accurate engine)
    bool check_scanout = true;    // accurate: compare each stable monitor frame with the predicted image
    uint64_t cosim_inject = ~0ull; // test hook: corrupt the ISS pc before instruction #N so the mismatch report can be tested
    bool scan_inject = false;     // test hook: corrupt one pixel of the predicted scan-out image
    bool keep_going = false;      // accurate: hardware-checker errors are reported but do not end the run
};

class Engine {
public:
    virtual ~Engine() = default;

    virtual const EngineInfo& info() const = 0;

    // ---- setup (before start) ----
    // Backdoor writer into guest RAM (shared ELF / WAD / boot-info loaders use it).
    virtual simc::WriteFn mem_writer() = 0;
    // --isa-test: report EvTohost on the first store to the aligned word at `addr`.
    virtual void watch_tohost(uint32_t addr) = 0;
    // Release reset with the PC at `entry` (RTL engines: entry must be 0x80000000).
    virtual bool start(uint32_t entry, std::string& err) = 0;

    // ---- running ----
    // Advance the machine by at most `max_cycles` cycles; return early on any Event.
    // Must not block, poll SDL or format strings: the caller does the UI between slices.
    virtual RunResult run(uint64_t max_cycles) = 0;
    virtual const Status& status() const = 0;
    virtual Stats stats() = 0;

    // ---- host -> machine ----
    virtual void push_key(bool pressed, uint8_t doom_code) = 0;

    // ---- machine -> host (callbacks fire synchronously from inside run()) ----
    std::function<void(uint8_t)> on_serial;
    std::function<void(const FrameView&)> on_frame;

    // 640x480 0x00RRGGBB: what a monitor on the video pins shows right now. Engines without
    // a native monitor synthesize it from the last presented frame (all black before the first).
    virtual const uint32_t* monitor_image() {
        if (mon_dirty_) {
            if (have_frame_) synth_monitor(snap_fb_, snap_pal_, mon_);
            else memset(mon_, 0, sizeof mon_);
            mon_dirty_ = false;
        }
        return mon_;
    }
    // Changes whenever monitor_image() may have changed (lets the front end skip unchanged redraws).
    virtual uint64_t monitor_serial() const { return frame_no_; }
    uint64_t frames_presented() const { return frame_no_; }

    // Multi-line text for the end of the run: cosim mismatch report, checker logs. May be empty.
    virtual std::string diagnostics() { return std::string(); }
    // Total number of instructions compared in lockstep (0 when cosim is off).
    virtual uint64_t cosim_compared() const { return 0; }

protected:
    // Engines call this at VID_FLIP with the live framebuffer/palette; it snapshots them
    // (for monitor synthesis), counts the frame and calls on_frame.
    void present_frame(const uint8_t* fb, const uint32_t* pal, uint64_t cycle) {
        memcpy(snap_fb_, fb, kFbBytes);
        memcpy(snap_pal_, pal, sizeof snap_pal_);
        have_frame_ = mon_dirty_ = true;
        frame_no_++;
        if (on_frame) on_frame(FrameView{snap_fb_, snap_pal_, frame_no_, cycle});
    }

private:
    uint8_t snap_fb_[kFbBytes];
    uint32_t snap_pal_[256];
    uint32_t mon_[kMonWidth * kMonHeight];
    bool have_frame_ = false, mon_dirty_ = true;
    uint64_t frame_no_ = 0;
};

// Factory. Returns null and fills err for unknown engines or unsupported options.
std::unique_ptr<Engine> make_engine(const std::string& name, const EngineOptions& opt, std::string& err);
std::unique_ptr<Engine> make_fast_engine(const EngineOptions& opt, std::string& err);       // engine_rtl.cpp
std::unique_ptr<Engine> make_accurate_engine(const EngineOptions& opt, std::string& err);   // engine_rtl.cpp
std::unique_ptr<Engine> make_model_engine(const EngineOptions& opt, std::string& err);      // engine_model.cpp

}  // namespace vboard
