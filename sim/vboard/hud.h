// hud.h - derived performance metrics and the instrument-panel renderer (no SDL).
#pragma once
#include <deque>
#include <string>

#include "engine.h"
#include "gfx.h"

namespace vboard {

struct Opt {
    bool ok = false;
    double v = 0;
    Opt() = default;
    Opt(double x) : ok(true), v(x) {}
};

// Quantities shown by the HUD, computed over the interval between two Stats snapshots
// (`prev` = all-zero Stats and wall 0 gives the since-reset values).
struct Metrics {
    Opt speed;        // MIPS (ISS) or simulated MHz (RTL) per wall second
    Opt ipc;          // retired instructions / cycles
    Opt ic_hit, dc_hit;       // 0..1
    Opt branch_acc, jump_acc; // 0..1  (1 - mispredicts / resolved)
    Opt sim_fps;      // frames per simulated second (cycles / clk_hz)
    Opt wall_fps;     // frames per wall second
};

Metrics compute_metrics(const EngineInfo& info, const Stats& cur, const Stats& prev, double dwall);

// number formatting helpers (shared with the end-of-run summary)
std::string fmt_group(uint64_t v);                     // 1,234,567
std::string fmt_fixed(const Opt& o, int dec);          // "12.3" or "-"
std::string fmt_pct(const Opt& o);                     // "98.7%" or "-"
std::string fmt_simtime(double seconds);               // "12.345 s" / "3 m 04.2 s"

struct HudState {
    const EngineInfo* info = nullptr;
    Stats stats;
    Status status;
    bool paused = false;
    bool stopped = false;       // a stop condition ended the run
    std::string stop_text;      // e.g. "max frames"
    double wall = 0;            // seconds the engine actually ran (pauses excluded)
    std::string program;        // base name of the ELF
};

class Hud {
public:
    // Call a few times per second with the current snapshot. Keeps the history that the
    // "recent" columns are computed from (window ~ 2 s of run time).
    void sample(double wall, const Stats& s);
    // Draw the panel into c at (x0, 0), width x height.
    void draw(Canvas& c, int x0, int width, int height, const HudState& st) const;
    static constexpr double kWindow = 2.0;

private:
    struct Sample { double wall; Stats s; };
    std::deque<Sample> hist_;
};

}  // namespace vboard
