// vboard - virtual board front end: a window, HUD, keyboard, serial terminal, frame dump and
// statistics around an interchangeable simulation engine (engine.h). See README.md.
#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "dfrm.h"
#include "engine.h"
#include "gfx.h"
#include "hud.h"
#include "keymap.h"
#include "loader.h"

using namespace vboard;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kExitHwError = 121, kExitSetup = 126, kExitUsage = 2;   // exit statuses: see usage() and kStops

constexpr int kPanelW = 360, kWinW = int(kMonWidth) + kPanelW, kWinH = int(kMonHeight);

struct Options {
    std::string engine = "fast", elf, wad, dump_frames, screenshot, stats_json, key_script;
    std::vector<std::string> args;
    bool headless = false, isa_test = false, cosim = false, cosim_model = false, quiet_stats = false, realtime = false, hold = false;
    bool check_scanout = true, keep_going = false;
    uint64_t cosim_inject = ~0ull;
    bool scan_inject = false;
    int scale = 1;
    uint64_t max_cycles = 0, max_frames = 0, screenshot_frame = 0;
    double max_seconds = 0;
    bool have_max_cycles = false, have_max_frames = false, have_max_seconds = false;
};

void usage(FILE* f) {
    fputs(
        "usage: vboard [options] program.elf [-- arg ...]\n"
        "  --engine fast|accurate|iss|model\n"
        "                        fast: the RTL with ideal memory, framebuffer read directly (default)\n"
        "                        accurate: the RTL with SDRAM pin timing and real video scan-out\n"
        "                        iss: the instruction-set golden model (no RTL, one cycle per instruction)\n"
        "                        model: cycle-exact software model of the accurate machine (no RTL)\n"
        "  --wad FILE            load a WAD at 0x80800000 and describe it in the boot-info page\n"
        "  --headless            no window: serial on stdout, statistics on stderr\n"
        "  --scale N             integer window scale (default 1; the window is 1000x480 at 1)\n"
        "  --max-cycles N        stop after N simulated cycles (ISS: instructions)        status 124\n"
        "  --max-seconds S       stop after S wall-clock seconds                          status 123\n"
        "  --max-frames N        stop after N presented frames                            status 122\n"
        "  --dump-frames FILE    write every presented frame to a DFRM file\n"
        "  --screenshot FILE     write the composed window image (PPM) at exit, or at presented\n"
        "                        frame N with --screenshot-frame N\n"
        "  --screenshot-frame N  see --screenshot\n"
        "  --isa-test            riscv-tests mode: a store to 'tohost' ends the run; exit 0 iff value 1\n"
        "  --cosim               RTL engines: run the ISS in lockstep and compare every retired instruction\n"
        "  --cosim=model         accurate: run the model in lockstep instead and compare every retired\n"
        "                        instruction with its cycle, the counters, frames and serial bytes\n"
        "  --no-check-scanout    accurate: disable the scan-out self-check\n"
        "  --keep-going          accurate: report hardware-checker errors but do not stop on them\n"
        "  --quiet-stats         no end-of-run statistics on stderr\n"
        "  --stats-json FILE     write the end-of-run statistics as JSON\n"
        "  --realtime            do not run faster than the simulated clock (cycles / clock Hz)\n"
        "  --hold                windowed: keep the window open after the program stops, until quit\n"
        "  --key-script SCRIPT   test hook: inject SDL key events, 'CYCLE:+Name,CYCLE:-Name,...'\n"
        "                        (+ press, - release, SDL key names, at simulated cycle CYCLE)\n"
        "hotkeys (not forwarded): F12 screenshot, ` pause, Cmd+Q / Cmd+W / window close quit\n"
        "exit status: low byte of SYS_EXIT; --isa-test 0 pass / 1 fail; 120 quit by user; 122 max-frames;\n"
        "  123 max-seconds; 124 max-cycles; 125 fault (bad fetch/access); 126 setup error; 2 usage;\n"
        "  121 hardware checker error (SDRAM protocol, serial, video, scan-out); 119 cosim mismatch\n",
        f);
}

enum class Stop { None, Exit, Fault, Tohost, MaxCycles, MaxSeconds, MaxFrames, UserQuit, HwError, Cosim };

double secs(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

// ---- key script ---------------------------------------------------------------------------
struct ScriptKey { uint64_t cycle; bool press; SDL_Keycode sym; };

bool parse_key_script(const std::string& s, std::vector<ScriptKey>& out, std::string& err) {
    size_t pos = 0;
    while (pos < s.size()) {
        size_t e = s.find(',', pos);
        if (e == std::string::npos) e = s.size();
        std::string tok = s.substr(pos, e - pos);
        pos = e + 1;
        if (tok.empty()) continue;
        size_t c = tok.find(':');
        if (c == std::string::npos || c + 2 > tok.size() || (tok[c + 1] != '+' && tok[c + 1] != '-')) {
            err = "bad key-script item '" + tok + "' (want CYCLE:+Name or CYCLE:-Name)";
            return false;
        }
        std::string name = tok.substr(c + 2);
        SDL_Keycode k = SDL_GetKeyFromName(name.c_str());
        if (k == SDLK_UNKNOWN) { err = "unknown SDL key name '" + name + "'"; return false; }
        out.push_back({strtoull(tok.substr(0, c).c_str(), nullptr, 0), tok[c + 1] == '+', k});
    }
    std::stable_sort(out.begin(), out.end(), [](const ScriptKey& a, const ScriptKey& b) { return a.cycle < b.cycle; });
    return true;
}

// ---- keyboard forwarding: SDL -> Doom codes, press/release bookkeeping -----------------------
class KeyForwarder {
public:
    explicit KeyForwarder(Engine& e) : e_(e) {}
    void down(SDL_Keycode sym, bool paused) {
        uint8_t code = sdl_to_doom_key(sym);
        if (!code || paused || held_.count(sym)) return;
        held_[sym] = code;
        if (refs_[code]++ == 0) e_.push_key(true, code);
    }
    void up(SDL_Keycode sym) {
        auto it = held_.find(sym);
        if (it == held_.end()) return;
        uint8_t code = it->second;
        held_.erase(it);
        if (--refs_[code] == 0) e_.push_key(false, code);
    }
    void release_all() {
        while (!held_.empty()) up(held_.begin()->first);
    }
private:
    Engine& e_;
    std::map<SDL_Keycode, uint8_t> held_;
    int refs_[256] = {};
};

// ---- statistics output ---------------------------------------------------------------------
// Per stop condition: its name in the summary and JSON, and the exit status (iss-run conventions plus more;
// Exit and Tohost take theirs from the program).
const struct { const char* name; int status; } kStops[] = {
    {"none", 120}, {"exit", 0}, {"fault", 125}, {"tohost", 0}, {"max-cycles", 124}, {"max-seconds", 123},
    {"max-frames", 122}, {"user-quit", 120}, {"hw-error", kExitHwError}, {"cosim-mismatch", 119}};
const char* stop_name(Stop s) { return kStops[int(s)].name; }

std::string opt_json(const Opt& o) {
    if (!o.ok || !std::isfinite(o.v)) return "null";
    char b[64];
    snprintf(b, sizeof b, "%.6g", o.v);
    return b;
}
std::string cnt_json(const Counter& c) { return c.ok ? std::to_string(c.v) : "null"; }
std::string json_escape(const std::string& s) {
    std::string o;
    for (char ch : s) {
        if (ch == '"' || ch == '\\') { o += '\\'; o += ch; }
        else if ((unsigned char)ch < 0x20) o += ' ';
        else o += ch;
    }
    return o;
}

struct Span { uint64_t first = 0, last = 0, frames = 0; };
Opt span_fps(const EngineInfo& info, const Span& sp) {
    if (sp.frames < 2 || sp.last <= sp.first) return Opt();
    return Opt(double(sp.frames - 1) / (double(sp.last - sp.first) / info.clk_hz));
}
Opt span_cpf(const Span& sp) {
    if (sp.frames < 2 || sp.last <= sp.first) return Opt();
    return Opt(double(sp.last - sp.first) / double(sp.frames - 1));
}

void print_summary(const EngineInfo& info, const Stats& s, const Metrics& m, double wall, Stop stop, int status,
                   const std::string& note, const Span& sp) {
    auto line = [](const char* k, const std::string& v) { fprintf(stderr, "  %-22s %s\n", k, v.c_str()); };
    auto cnt = [](const Counter& c) { return c.ok ? fmt_group(c.v) : std::string("-"); };
    fprintf(stderr, "vboard: %s engine, %s\n", info.name.c_str(), note.c_str());
    line("stop", std::string(stop_name(stop)) + ", exit status " + std::to_string(status));
    line("wall time", fmt_simtime(wall));
    line("simulated time", s.cycles.ok ? fmt_simtime(double(s.cycles.v) / info.clk_hz) + " at " + fmt_fixed(Opt(info.clk_hz / 1e6), 1) + " MHz" : "-");
    line("cycles", cnt(s.cycles));
    line("instructions", cnt(s.instret));
    line(info.speed_unit == SpeedUnit::Mips ? "speed (MIPS)" : "speed (sim MHz)", fmt_fixed(m.speed, 2));
    line("IPC", fmt_fixed(m.ipc, 3));
    line("I-cache hit rate", fmt_pct(m.ic_hit) + (s.ic_access.ok ? "  (" + fmt_group(s.ic_access.v) + " accesses)" : ""));
    line("D-cache hit rate", fmt_pct(m.dc_hit) + (s.dc_access.ok ? "  (" + fmt_group(s.dc_access.v) + " accesses)" : ""));
    line("branch accuracy", fmt_pct(m.branch_acc) + (s.branch.ok ? "  (" + fmt_group(s.branch.v) + " branches)" : ""));
    line("jump accuracy", fmt_pct(m.jump_acc) + (s.jump.ok ? "  (" + fmt_group(s.jump.v) + " jumps)" : ""));
    line("frames presented", cnt(s.flips));
    line("fps (simulated time)", fmt_fixed(m.sim_fps, 2));
    line("fps (wall clock)", fmt_fixed(m.wall_fps, 2));
    line("fps (first to last frame)", fmt_fixed(span_fps(info, sp), 2));
    line("cycles per frame", sp.frames >= 2 ? fmt_group(uint64_t(span_cpf(sp).v)) + "  (first to last frame)" : "-");
    line("SDRAM violations", cnt(s.sdram_violations));
    line("serial errors", cnt(s.serial_errors));
    line("video timing errors", cnt(s.video_errors));
    if (s.scan_checked.ok)
        line("scan-out check", fmt_group(s.scan_checked.v) + " frames checked, " + fmt_group(s.scan_skipped.v) + " skipped (changed during scan-out), " + fmt_group(s.scan_bad.v) + " mismatched");
    for (auto& kv : s.extras) line(kv.first.c_str(), fmt_group(kv.second));
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    int i = 1;
    for (; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&](const char* opt) -> const char* {
            if (i + 1 >= argc) { fprintf(stderr, "vboard: %s needs an argument\n", opt); exit(kExitUsage); }
            return argv[++i];
        };
        if (a == "--") { i++; break; }
        else if (a == "--engine") o.engine = need("--engine");
        else if (a == "--wad") o.wad = need("--wad");
        else if (a == "--headless") o.headless = true;
        else if (a == "--scale") { o.scale = atoi(need("--scale")); if (o.scale < 1 || o.scale > 8) { fprintf(stderr, "vboard: --scale must be 1..8\n"); return kExitUsage; } }
        else if (a == "--max-cycles") { o.max_cycles = strtoull(need("--max-cycles"), nullptr, 0); o.have_max_cycles = true; }
        else if (a == "--max-seconds") { o.max_seconds = atof(need("--max-seconds")); o.have_max_seconds = true; }
        else if (a == "--max-frames") { o.max_frames = strtoull(need("--max-frames"), nullptr, 0); o.have_max_frames = true; }
        else if (a == "--dump-frames") o.dump_frames = need("--dump-frames");
        else if (a == "--screenshot") o.screenshot = need("--screenshot");
        else if (a == "--screenshot-frame") o.screenshot_frame = strtoull(need("--screenshot-frame"), nullptr, 0);
        else if (a == "--isa-test") o.isa_test = true;
        else if (a == "--cosim") o.cosim = true;
        else if (a == "--cosim=model") o.cosim = o.cosim_model = true;
        else if (a == "--cosim-inject") o.cosim_inject = strtoull(need("--cosim-inject"), nullptr, 0);   // test hook
        else if (a == "--scanout-inject") o.scan_inject = true;   // test hook
        else if (a == "--no-check-scanout") o.check_scanout = false;
        else if (a == "--keep-going") o.keep_going = true;
        else if (a == "--quiet-stats") o.quiet_stats = true;
        else if (a == "--stats-json") o.stats_json = need("--stats-json");
        else if (a == "--realtime") o.realtime = true;
        else if (a == "--hold") o.hold = true;
        else if (a == "--key-script") o.key_script = need("--key-script");
        else if (a == "-h" || a == "--help") { usage(stdout); return 0; }
        else if (a.size() > 1 && a[0] == '-') { fprintf(stderr, "vboard: unknown option '%s'\n", a.c_str()); usage(stderr); return kExitUsage; }
        else if (o.elf.empty()) o.elf = a;
        else { fprintf(stderr, "vboard: unexpected argument '%s' (program arguments go after --)\n", a.c_str()); return kExitUsage; }
    }
    for (; i < argc; i++) o.args.push_back(argv[i]);
    if (o.elf.empty()) { usage(stderr); return kExitUsage; }
    if (o.screenshot_frame && o.screenshot.empty()) { fprintf(stderr, "vboard: --screenshot-frame needs --screenshot\n"); return kExitUsage; }

    // ---- engine, program, WAD, boot info ----
    std::string err;
    EngineOptions eopt;
    eopt.scan_inject = o.scan_inject; eopt.cosim = o.cosim; eopt.cosim_model = o.cosim_model; eopt.cosim_inject = o.cosim_inject; eopt.check_scanout = o.check_scanout; eopt.keep_going = o.keep_going;
    std::unique_ptr<Engine> eng = make_engine(o.engine, eopt, err);
    if (!eng) { fprintf(stderr, "vboard: %s\n", err.c_str()); return kExitSetup; }
    const EngineInfo& info = eng->info();

    uint32_t entry = 0, tohost = 0;
    if (!simc::load_program(eng->mem_writer(), o.elf, o.wad, o.args, o.isa_test, entry, tohost, err)) { fprintf(stderr, "vboard: %s\n", err.c_str()); return kExitSetup; }
    if (o.isa_test) eng->watch_tohost(tohost);
    if (!eng->start(entry, err)) { fprintf(stderr, "vboard: %s\n", err.c_str()); return kExitSetup; }

    // ---- sinks ----
    std::string serial;
    eng->on_serial = [&](uint8_t c) { serial.push_back(char(c)); };
    auto flush_serial = [&]() {
        if (serial.empty()) return;
        fwrite(serial.data(), 1, serial.size(), stdout);
        fflush(stdout);
        serial.clear();
    };

    simc::DfrmWriter dump;
    if (!o.dump_frames.empty() && !dump.open(o.dump_frames, err)) { fprintf(stderr, "vboard: %s\n", err.c_str()); return kExitSetup; }
    bool shot_pending = false;
    uint64_t first_frame_cycle = 0, last_frame_cycle = 0, frames_seen = 0;
    eng->on_frame = [&](const FrameView& f) {
        if (!frames_seen) first_frame_cycle = f.cycle;
        last_frame_cycle = f.cycle;
        frames_seen++;
        if (dump.frames() < (o.have_max_frames ? o.max_frames : UINT64_MAX) && !o.dump_frames.empty())
            dump.write_frame_xrgb(f.palette, f.indices);
        if (!o.screenshot.empty() && o.screenshot_frame && f.number == o.screenshot_frame) shot_pending = true;
    };

    // ---- SDL ----
    std::vector<ScriptKey> script;
    bool use_window = !o.headless;
    bool sdl_events = use_window || !o.key_script.empty();
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    SDL_Texture* tex = nullptr;
    if (sdl_events) {
        if (SDL_Init(use_window ? SDL_INIT_VIDEO | SDL_INIT_EVENTS : SDL_INIT_EVENTS) != 0) {
            fprintf(stderr, "vboard: SDL_Init: %s\n", SDL_GetError());
            return kExitSetup;
        }
        if (use_window) {
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
            win = SDL_CreateWindow(("vboard - " + simc::path_basename(o.elf) + " [" + info.name + "]").c_str(), SDL_WINDOWPOS_CENTERED,
                                   SDL_WINDOWPOS_CENTERED, kWinW * o.scale, kWinH * o.scale,
                                   SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
            if (win) {
                ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
                if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
            }
            if (ren) {
                SDL_RenderSetLogicalSize(ren, kWinW, kWinH);
                tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, kWinW, kWinH);
            }
            if (!win || !ren || !tex) {
                fprintf(stderr, "vboard: cannot open window: %s\n", SDL_GetError());
                return kExitSetup;
            }
        }
        if (!o.key_script.empty() && !parse_key_script(o.key_script, script, err)) { fprintf(stderr, "vboard: %s\n", err.c_str()); return kExitSetup; }
    }

    // ---- run state ----
    Canvas canvas(kWinW, kWinH);
    Hud hud;
    KeyForwarder keys(*eng);
    Stop stop = Stop::None;
    bool paused = false, quit = false;
    int shot_serial = 0;
    size_t script_i = 0;
    uint64_t cycles = 0;           // engine cycles executed (sum of RunResult::cycles)
    const double target_slice = use_window ? 0.005 : 0.025;   // wall seconds per engine slice
    double rate = 2e6;             // cycles per second, estimated
    auto t_start = Clock::now();
    auto t_last = t_start, t_ui = t_start;
    double run_wall = 0;           // seconds the engine was allowed to run (pauses excluded)
    double eng_time = 0, ui_eng_time = 0;
    uint64_t ui_cycles = 0;
    Stats stats_cache;
    std::string stop_text;

    auto make_state = [&]() {
        HudState hs;
        hs.info = &info;
        hs.stats = eng->stats();
        hs.status = eng->status();
        hs.paused = paused;
        hs.stopped = stop != Stop::None && stop != Stop::Exit && stop != Stop::Fault && stop != Stop::Tohost;
        hs.stop_text = stop_text;
        hs.wall = run_wall;
        hs.program = simc::path_basename(o.elf);
        return hs;
    };
    // The monitor image and the panel cover the whole canvas. The monitor part is redrawn only when the
    // engine has a new image or NO SIGNAL goes away; returns true if it was.
    uint64_t mon_key = ~0ull;
    auto compose = [&]() {
        uint64_t key = eng->monitor_serial() * 2 + (eng->frames_presented() == 0);
        bool mon = key != mon_key;
        if (mon) {
            canvas.blit(eng->monitor_image(), int(kMonWidth), int(kMonHeight), 0, 0);
            if (eng->frames_presented() == 0) {
                const char* msg = "NO SIGNAL";
                canvas.text(int(kMonWidth) / 2 - Canvas::text_width(msg, 2) / 2, 232, msg, 0x3A444E, 2);
            }
            mon_key = key;
        }
        hud.draw(canvas, int(kMonWidth), kPanelW, kWinH, make_state());
        return mon;
    };
    auto save_shot = [&](const std::string& path) {
        hud.sample(run_wall, eng->stats());
        compose();
        std::string e;
        if (!canvas.write_ppm(path, e)) fprintf(stderr, "vboard: %s\n", e.c_str());
        else fprintf(stderr, "vboard: screenshot written to %s\n", path.c_str());
    };
    auto present = [&]() {
        if (!use_window) return;
        static const SDL_Rect panel{int(kMonWidth), 0, kPanelW, kWinH};
        if (compose()) SDL_UpdateTexture(tex, nullptr, canvas.data(), kWinW * 4);
        else SDL_UpdateTexture(tex, &panel, canvas.data() + kMonWidth, kWinW * 4);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, nullptr, nullptr);
        SDL_RenderPresent(ren);
    };

    hud.sample(0, Stats());

    auto handle_events = [&]() {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT: quit = true; break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) keys.release_all();
                break;
            case SDL_KEYDOWN: {
                if (ev.key.repeat) break;
                SDL_Keycode k = ev.key.keysym.sym;
                bool gui = (ev.key.keysym.mod & KMOD_GUI) != 0;
                if (gui && (k == SDLK_q || k == SDLK_w)) { quit = true; break; }
                if (k == SDLK_F12) { char nm[64]; snprintf(nm, sizeof nm, "vboard-shot-%03d.ppm", ++shot_serial); save_shot(nm); break; }
                if (k == SDLK_BACKQUOTE) { paused = !paused; if (paused) keys.release_all(); break; }
                if (gui) break;
                keys.down(k, paused);
                break;
            }
            case SDL_KEYUP:
                if (ev.key.keysym.sym == SDLK_F12 || ev.key.keysym.sym == SDLK_BACKQUOTE) break;
                keys.up(ev.key.keysym.sym);
                break;
            default: break;
            }
        }
    };
    auto push_script_due = [&]() {
        while (script_i < script.size() && script[script_i].cycle <= cycles) {
            SDL_Event ev;
            memset(&ev, 0, sizeof ev);
            ev.type = script[script_i].press ? SDL_KEYDOWN : SDL_KEYUP;
            ev.key.state = script[script_i].press ? SDL_PRESSED : SDL_RELEASED;
            ev.key.keysym.sym = script[script_i].sym;
            SDL_PushEvent(&ev);
            script_i++;
        }
    };

    // ---- main loop ----
    bool machine_done = false;
    for (;;) {
        auto now = Clock::now();
        if (!paused && !machine_done) run_wall += secs(t_last, now);
        t_last = now;

        if (sdl_events) {
            push_script_due();
            handle_events();
            if (quit) { if (stop == Stop::None) { stop = Stop::UserQuit; stop_text = "quit"; } break; }
        }
        if (o.have_max_seconds && secs(t_start, now) >= o.max_seconds) { if (stop == Stop::None) { stop = Stop::MaxSeconds; stop_text = "max seconds"; } break; }

        if (machine_done) {          // --hold: keep the window alive and responsive
            present();
            SDL_Delay(20);
            continue;
        }

        if (paused) { present(); SDL_Delay(10); continue; }

        // budget for this engine call
        uint64_t budget = uint64_t(std::min(std::max(rate * target_slice, 1000.0), double(1u << 28)));
        if (o.have_max_cycles) {
            if (cycles >= o.max_cycles) { stop = Stop::MaxCycles; stop_text = "max cycles"; break; }
            budget = std::min(budget, o.max_cycles - cycles);
        }
        if (script_i < script.size()) budget = std::min(budget, std::max<uint64_t>(script[script_i].cycle - cycles, 1));
        if (o.realtime && info.clk_hz) {
            double allowed = run_wall * info.clk_hz - double(cycles);
            if (allowed < 1000) { flush_serial(); if (use_window) { present(); } SDL_Delay(1); continue; }
            budget = std::min<uint64_t>(budget, uint64_t(allowed));
        }

        auto e0 = Clock::now();
        RunResult rr = eng->run(budget);
        auto e1 = Clock::now();
        cycles += rr.cycles;
        eng_time += secs(e0, e1);
        flush_serial();

        if (rr.events & EvHalted) { stop = Stop::Exit; machine_done = true; }
        if (rr.events & EvFault) { stop = Stop::Fault; machine_done = true; }
        if (rr.events & EvTohost) { stop = Stop::Tohost; machine_done = true; }
        if (rr.events & EvCheck) { stop = Stop::HwError; machine_done = true; }
        if (rr.events & EvCosim) { stop = Stop::Cosim; machine_done = true; }
        if (shot_pending) { shot_pending = false; save_shot(o.screenshot); o.screenshot.clear(); }
        if (stop == Stop::None && o.have_max_frames && eng->frames_presented() >= o.max_frames) { stop = Stop::MaxFrames; stop_text = "max frames"; machine_done = true; }
        if (machine_done) {
            if (!(o.hold && use_window && stop != Stop::MaxFrames)) break;   // --hold: stay until quit
            present();
        }

        // UI tick: a few times per millisecond-scale slice, work by wall time
        auto n2 = Clock::now();
        if (secs(t_ui, n2) >= (use_window ? 0.0166 : 0.25)) {
            double de = eng_time - ui_eng_time;
            if (de > 1e-4) rate = 0.5 * rate + 0.5 * double(cycles - ui_cycles) / de;
            ui_eng_time = eng_time; ui_cycles = cycles;
            t_ui = n2;
            static auto t_hud = t_start;
            if (secs(t_hud, n2) >= 0.25) { hud.sample(run_wall, eng->stats()); t_hud = n2; }
            present();
        }
    }
    flush_serial();

    // ---- end of run ----
    double wall = secs(t_start, Clock::now());
    const Status& st = eng->status();
    int status = stop == Stop::Exit ? int(st.exit_code & 0xFF) : stop == Stop::Tohost ? (st.tohost == 1 ? 0 : 1) : kStops[int(stop)].status;
    if (stop == Stop::Fault) fprintf(stderr, "vboard: fault: %s\n", st.message.c_str());
    {
        std::string diag = eng->diagnostics();
        if (!diag.empty()) fprintf(stderr, "vboard: diagnostics:\n%s", diag.c_str());
    }
    if (stop == Stop::Tohost && st.tohost != 1)
        fprintf(stderr, "vboard: %s: FAILED test %u (tohost=0x%x) at pc=0x%08x\n", o.elf.c_str(), st.tohost >> 1, st.tohost, st.pc);

    Stats fin = eng->stats();
    if (o.keep_going && status == 0 && stop == Stop::Exit &&
        ((fin.sdram_violations.ok && fin.sdram_violations.v) || (fin.serial_errors.ok && fin.serial_errors.v) ||
         (fin.video_errors.ok && fin.video_errors.v) || (fin.scan_bad.ok && fin.scan_bad.v))) {
        fprintf(stderr, "vboard: --keep-going: hardware checker errors were seen (exit status would have been %d)\n", kExitHwError);
        status = kExitHwError;
    }
    double run_secs = std::max(run_wall, 1e-9);
    Metrics m = compute_metrics(info, fin, Stats(), run_secs);
    if (!o.screenshot.empty()) {
        run_wall = std::max(run_wall, 0.0);
        hud.sample(run_wall, fin);
        save_shot(o.screenshot);
    }
    dump.close();
    if (!o.quiet_stats) print_summary(info, fin, m, wall, stop, status, simc::path_basename(o.elf), Span{first_frame_cycle, last_frame_cycle, frames_seen});
    if (!o.stats_json.empty()) {
        FILE* f = fopen(o.stats_json.c_str(), "w");
        if (!f) fprintf(stderr, "vboard: cannot create '%s'\n", o.stats_json.c_str());
        else {
            fprintf(f, "{\n  \"engine\": \"%s\",\n  \"program\": \"%s\",\n  \"stop\": \"%s\",\n  \"exit_status\": %d,\n", info.name.c_str(),
                    json_escape(o.elf).c_str(), stop_name(stop), status);
            fprintf(f, "  \"program_exit_code\": %s,\n", stop == Stop::Exit ? std::to_string(st.exit_code).c_str() : "null");
            fprintf(f, "  \"clk_hz\": %u,\n  \"wall_seconds\": %.6f,\n  \"run_seconds\": %.6f,\n", info.clk_hz, wall, run_wall);
            fprintf(f, "  \"sim_seconds\": %s,\n", fin.cycles.ok ? std::to_string(double(fin.cycles.v) / info.clk_hz).c_str() : "null");
            fprintf(f, "  \"cycles\": %s,\n  \"instret\": %s,\n  \"ipc\": %s,\n", cnt_json(fin.cycles).c_str(), cnt_json(fin.instret).c_str(), opt_json(m.ipc).c_str());
            fprintf(f, "  \"speed\": %s,\n  \"speed_unit\": \"%s\",\n", opt_json(m.speed).c_str(), info.speed_unit == SpeedUnit::Mips ? "MIPS" : "MHz");
            fprintf(f, "  \"ic_access\": %s,\n  \"ic_miss\": %s,\n  \"ic_hit_rate\": %s,\n", cnt_json(fin.ic_access).c_str(), cnt_json(fin.ic_miss).c_str(), opt_json(m.ic_hit).c_str());
            fprintf(f, "  \"dc_access\": %s,\n  \"dc_miss\": %s,\n  \"dc_hit_rate\": %s,\n", cnt_json(fin.dc_access).c_str(), cnt_json(fin.dc_miss).c_str(), opt_json(m.dc_hit).c_str());
            fprintf(f, "  \"branches\": %s,\n  \"branch_miss\": %s,\n  \"branch_accuracy\": %s,\n", cnt_json(fin.branch).c_str(), cnt_json(fin.branch_miss).c_str(), opt_json(m.branch_acc).c_str());
            fprintf(f, "  \"jumps\": %s,\n  \"jump_miss\": %s,\n  \"jump_accuracy\": %s,\n", cnt_json(fin.jump).c_str(), cnt_json(fin.jump_miss).c_str(), opt_json(m.jump_acc).c_str());
            fprintf(f, "  \"frames\": %s,\n  \"fps_sim\": %s,\n  \"fps_wall\": %s,\n", cnt_json(fin.flips).c_str(), opt_json(m.sim_fps).c_str(), opt_json(m.wall_fps).c_str());
            Span sp{first_frame_cycle, last_frame_cycle, frames_seen};
            fprintf(f, "  \"fps_span\": %s,\n  \"cycles_per_frame\": %s,\n", opt_json(span_fps(info, sp)).c_str(), opt_json(span_cpf(sp)).c_str());
            fprintf(f, "  \"first_frame_cycle\": %llu,\n  \"last_frame_cycle\": %llu,\n", (unsigned long long)first_frame_cycle, (unsigned long long)last_frame_cycle);
            fprintf(f, "  \"serial_errors\": %s,\n  \"video_errors\": %s,\n", cnt_json(fin.serial_errors).c_str(), cnt_json(fin.video_errors).c_str());
            fprintf(f, "  \"scan_checked\": %s,\n  \"scan_skipped\": %s,\n  \"scan_bad\": %s,\n", cnt_json(fin.scan_checked).c_str(), cnt_json(fin.scan_skipped).c_str(), cnt_json(fin.scan_bad).c_str());
            fprintf(f, "  \"cosim_instructions\": %s,\n", o.cosim ? std::to_string(eng->cosim_compared()).c_str() : "null");
            fprintf(f, "  \"sdram_violations\": %s", cnt_json(fin.sdram_violations).c_str());
            for (auto& kv : fin.extras) fprintf(f, ",\n  \"%s\": %llu", json_escape(kv.first).c_str(), (unsigned long long)kv.second);
            fprintf(f, "\n}\n");
            fclose(f);
        }
    }
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    if (sdl_events) SDL_Quit();
    return status;
}
