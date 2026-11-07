#include "hud.h"

#include <cmath>
#include <cstdio>

namespace vboard {

namespace {
uint64_t delta(const Counter& a, const Counter& b) { return a.v >= b.v ? a.v - b.v : 0; }
Opt ratio_hit(const Counter& acc, const Counter& miss, const Counter& pacc, const Counter& pmiss) {
    if (!acc.ok || !miss.ok) return Opt();
    uint64_t a = delta(acc, pacc), m = delta(miss, pmiss);
    if (a == 0) return Opt();
    return Opt(1.0 - double(m) / double(a));
}
}  // namespace

Metrics compute_metrics(const EngineInfo& info, const Stats& cur, const Stats& prev, double dwall) {
    Metrics m;
    const Counter& num = info.speed_unit == SpeedUnit::Mips ? cur.instret : cur.cycles;
    const Counter& pnum = info.speed_unit == SpeedUnit::Mips ? prev.instret : prev.cycles;
    if (num.ok && dwall > 0) m.speed = Opt(double(delta(num, pnum)) / dwall / 1e6);
    if (cur.cycles.ok && cur.instret.ok && delta(cur.cycles, prev.cycles) > 0)
        m.ipc = Opt(double(delta(cur.instret, prev.instret)) / double(delta(cur.cycles, prev.cycles)));
    m.ic_hit = ratio_hit(cur.ic_access, cur.ic_miss, prev.ic_access, prev.ic_miss);
    m.dc_hit = ratio_hit(cur.dc_access, cur.dc_miss, prev.dc_access, prev.dc_miss);
    m.branch_acc = ratio_hit(cur.branch, cur.branch_miss, prev.branch, prev.branch_miss);
    m.jump_acc = ratio_hit(cur.jump, cur.jump_miss, prev.jump, prev.jump_miss);
    if (cur.flips.ok && cur.cycles.ok && info.clk_hz) {
        double dsim = double(delta(cur.cycles, prev.cycles)) / double(info.clk_hz);
        if (dsim > 0) m.sim_fps = Opt(double(delta(cur.flips, prev.flips)) / dsim);
    }
    if (cur.flips.ok && dwall > 0) m.wall_fps = Opt(double(delta(cur.flips, prev.flips)) / dwall);
    return m;
}

std::string fmt_group(uint64_t v) {
    std::string s = std::to_string(v), out;
    int n = int(s.size());
    for (int i = 0; i < n; i++) {
        out += s[i];
        if ((n - 1 - i) % 3 == 0 && i != n - 1) out += ',';
    }
    return out;
}

std::string fmt_fixed(const Opt& o, int dec) {
    if (!o.ok) return "-";
    char b[64];
    snprintf(b, sizeof b, "%.*f", dec, o.v);
    return b;
}

std::string fmt_pct(const Opt& o) {
    if (!o.ok) return "-";
    char b[32];
    snprintf(b, sizeof b, "%.1f%%", o.v * 100.0);
    return b;
}

std::string fmt_simtime(double s) {
    char b[48];
    if (s < 60) snprintf(b, sizeof b, "%.3f s", s);
    else snprintf(b, sizeof b, "%d m %04.1f s", int(s / 60), std::fmod(s, 60.0));
    return b;
}

void Hud::sample(double wall, const Stats& s) {
    hist_.push_back({wall, s});
    // Keep the newest sample that is at least kWindow old plus everything newer.
    while (hist_.size() > 2 && hist_[1].wall <= wall - kWindow) hist_.pop_front();
}

namespace {
// palette
constexpr uint32_t kBg = 0x0F1318, kBand = 0x161C23, kRule = 0x28313B, kLabel = 0x8896A5, kValue = 0xE9EEF3,
                   kDim = 0x4A5663, kAccent = 0x4FC3B0, kTrack = 0x232B34, kGreen = 0x5BC970, kAmber = 0xE5B24A,
                   kRed = 0xE5675D, kBlue = 0x6FA8E8;
}  // namespace

void Hud::draw(Canvas& c, int x0, int W, int H, const HudState& st) const {
    c.rect(x0, 0, W, H, kBg);
    c.rect(x0, 0, 1, H, kRule);
    const int L = x0 + 16, R = x0 + W - 16;
    const EngineInfo& info = *st.info;

    // ---- header ----
    c.rect(x0 + 1, 0, W - 1, 56, kBand);
    c.text(L, 10, "DOOM SoC", kValue, 2);
    c.text(L, 34, "virtual board", kLabel, 1);
    {
        std::string name = info.name;
        for (auto& ch : name) ch = char(toupper((unsigned char)ch));
        c.text_right(R, 12, name, kAccent, 2);
        c.text_right(R, 34, info.description.substr(0, info.description.find(" (")), kLabel, 1);
    }
    // status pill
    std::string label;
    uint32_t col = kGreen;
    switch (st.status.state) {
    case RunState::Running: label = st.paused ? "PAUSED" : "RUNNING"; col = st.paused ? kAmber : kGreen; break;
    case RunState::Halted: label = "HALTED, EXIT " + std::to_string(st.status.exit_code & 0xFF); col = kBlue; break;
    case RunState::Tohost: label = st.status.tohost == 1 ? "PASS" : "FAIL " + std::to_string(st.status.tohost >> 1); col = st.status.tohost == 1 ? kGreen : kRed; break;
    case RunState::Fault: label = "FAULT"; col = kRed; break;
    case RunState::HwError: label = "HARDWARE CHECK FAILED"; col = kRed; break;
    case RunState::CosimMismatch: label = "COSIM MISMATCH"; col = kRed; break;
    }
    if (st.stopped && st.status.state == RunState::Running) { label = "STOPPED: " + st.stop_text; col = kAmber; }
    int pw = Canvas::text_width(label) + 20;
    c.rect(L, 66, pw, 20, kTrack);
    c.rect(L, 66, 4, 20, col);
    c.text(L + 12, 70, label, col);
    {
        std::string prog = st.program;
        int room = (R - (L + pw) - 16) / 8;
        if (room < 4) prog.clear();
        else if (int(prog.size()) > room) prog = prog.substr(0, room - 2) + "..";
        c.text_right(R, 70, prog, kDim);
    }

    // ---- current / recent window ----
    Stats zero;
    Metrics tot = compute_metrics(info, st.stats, zero, st.wall);
    Metrics rec = tot;
    if (!hist_.empty()) {
        const Sample& old = hist_.front();
        if (st.wall - old.wall > 0.05) rec = compute_metrics(info, st.stats, old.s, st.wall - old.wall);
    }

    // ---- speed ----
    int y = 96;
    const char* unit = info.speed_unit == SpeedUnit::Mips ? "MIPS" : "MHz";
    c.text(L, y, "SIMULATION SPEED", kAccent);
    c.rect(L, y + 12, R - L, 1, kRule);
    y += 22;
    {
        std::string big = fmt_fixed(rec.speed, rec.speed.ok && rec.speed.v < 100 ? 2 : 1);
        int xe = c.text(L, y, big, kValue, 3);
        c.text(xe + 8, y + 14, unit, kLabel);
        c.text_right(R, y, "average", kDim);
        c.text_right(R, y + 12, fmt_fixed(tot.speed, 1) + " " + unit, kLabel);
    }
    y += 36;

    // ---- rate table ----
    const int cA = R - 96, cB = R;   // right edges of the "since reset" and "recent" columns
    c.text(L, y, "RATES", kAccent);
    c.text_right(cA, y, "SINCE RESET", kDim);
    c.text_right(cB, y, "RECENT", kDim);
    c.rect(L, y + 12, R - L, 1, kRule);
    y += 20;
    auto row = [&](const char* lab, const std::string& a, const std::string& b) {
        c.text(L, y, lab, kLabel);
        c.text_right(cA, y, a, a == "-" ? kDim : kValue);
        c.text_right(cB, y, b, b == "-" ? kDim : kValue);
        y += 14;
    };
    row("IPC", fmt_fixed(tot.ipc, 2), fmt_fixed(rec.ipc, 2));
    row("I-cache hit", fmt_pct(tot.ic_hit), fmt_pct(rec.ic_hit));
    row("D-cache hit", fmt_pct(tot.dc_hit), fmt_pct(rec.dc_hit));
    row("Branch accuracy", fmt_pct(tot.branch_acc), fmt_pct(rec.branch_acc));
    row("Jump accuracy", fmt_pct(tot.jump_acc), fmt_pct(rec.jump_acc));
    y += 4;
    row("fps @ sim clock", fmt_fixed(tot.sim_fps, 1), fmt_fixed(rec.sim_fps, 1));
    row("fps @ wall clock", fmt_fixed(tot.wall_fps, 1), fmt_fixed(rec.wall_fps, 1));
    {
        char b[64];
        snprintf(b, sizeof b, "on hardware: %s fps at %.0f MHz", fmt_fixed(rec.sim_fps, 1).c_str(), info.clk_hz / 1e6);
        c.text(L, y + 2, b, kDim);
        y += 16;
    }

    // ---- counters ----
    y += 4;
    c.text(L, y, "COUNTERS", kAccent);
    c.rect(L, y + 12, R - L, 1, kRule);
    y += 20;
    auto kv = [&](const char* lab, const std::string& v, bool avail = true) {
        c.text(L, y, lab, kLabel);
        c.text_right(R, y, v, avail ? kValue : kDim);
        y += 14;
    };
    double simt = st.stats.cycles.ok && info.clk_hz ? double(st.stats.cycles.v) / info.clk_hz : 0;
    kv("Simulated time", st.stats.cycles.ok ? fmt_simtime(simt) : "-", st.stats.cycles.ok);
    kv("Wall time", fmt_simtime(st.wall));
    kv("Cycles", st.stats.cycles.ok ? fmt_group(st.stats.cycles.v) : "-", st.stats.cycles.ok);
    kv("Instructions", st.stats.instret.ok ? fmt_group(st.stats.instret.v) : "-", st.stats.instret.ok);
    kv("Frames presented", st.stats.flips.ok ? fmt_group(st.stats.flips.v) : "-", st.stats.flips.ok);
    {
        auto vcol = [](const Counter& c) { return !c.ok ? kDim : c.v ? kRed : kGreen; };
        const Stats& S = st.stats;
        c.text(L, y, "SDRAM violations", kLabel);
        c.text_right(R, y, S.sdram_violations.ok ? fmt_group(S.sdram_violations.v) : "-", vcol(S.sdram_violations));
        y += 14;
        c.text(L, y, "Serial / video errors", kLabel);
        if (S.serial_errors.ok)
            c.text_right(R, y, fmt_group(S.serial_errors.v) + " / " + fmt_group(S.video_errors.v), (S.serial_errors.v || S.video_errors.v) ? kRed : kGreen);
 else c.text_right(R, y, "-", kDim);
        y += 14;
        c.text(L, y, "Scan-out ok/skip/bad", kLabel);
        if (S.scan_checked.ok) {
            std::string t = fmt_group(S.scan_checked.v) + " / " + fmt_group(S.scan_skipped.v) + " / " + fmt_group(S.scan_bad.v);
            c.text_right(R, y, t, S.scan_bad.v ? kRed : kValue);
        } else c.text_right(R, y, "-", kDim);
        y += 14;
    }

    // ---- footer: hotkeys ----
    int fy = H - 38;
    c.rect(L, fy - 6, R - L, 1, kRule);
    c.text(L, fy, "F12", kAccent); c.text(L + 32, fy, "screenshot", kLabel);
    c.text(L + 140, fy, "`", kAccent); c.text(L + 156, fy, "pause", kLabel);
    c.text(L, fy + 14, "Cmd+Q", kAccent); c.text(L + 48, fy + 14, "quit", kLabel);
    c.text(L + 140, fy + 14, "other keys go to Doom", kDim);
}

}  // namespace vboard
