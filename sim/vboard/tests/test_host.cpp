// Host-side checks for vboard: unit tests (mode "unit"), decoded DFRM comparison (mode "frames"),
// screenshot geometry (mode "shot"). Exit status 0 iff everything passed.
#include <SDL_keycode.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "dfrm.h"
#include "engine.h"
#include "hud.h"
#include "keymap.h"

using namespace vboard;

static int g_fail = 0, g_pass = 0;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// The pattern drawn by tests/video.c.
static uint32_t expected_rgb(int f, int x, int y) {
    static const uint32_t cols[8] = {0, 0xE03030, 0x30E030, 0x3030E0, 0xE0E030, 0x30E0E0, 0xE030E0, 0xF0F0F0};
    int p = 1 + ((x / 40 + y / 25) % 7);
    if (x < 2 || y < 2 || x >= 320 - 2 || y >= 200 - 2) p = 0;
    int bx = 16 * f + 8, by = 8 + 20 * f;
    if (x >= bx && x < bx + 20 && y >= by && y < by + 20) p = 8;
    if (p == 0) return 0x101010u * (uint32_t)(f + 1);
    if (p == 8) return 0xFF8000;
    return cols[p];
}

static int test_frames(const char* path, int n) {
    simc::DfrmReader r;
    std::string err = r.open(path);
    if (!err.empty()) { printf("  %s\n", err.c_str()); return 1; }
    CHECK(r.width() == 320 && r.height() == 200 && !r.truncated(), "DFRM geometry %ux%u%s", r.width(), r.height(), r.truncated() ? ", truncated" : "");
    simc::DfrmFrame fr;
    std::vector<uint8_t> rgb;
    int count = 0;
    for (; r.read(count, fr); count++) {
        fr.to_rgb(rgb);
        int bad = 0;
        for (int i = 0; i < 64000; i++)
            if ((uint32_t(rgb[3 * i]) << 16 | rgb[3 * i + 1] << 8 | rgb[3 * i + 2]) != expected_rgb(count, i % 320, i / 320)) bad++;
        CHECK(bad == 0, "frame %d: %d pixels differ from the drawn pattern", count, bad);
    }
    CHECK(count == n, "DFRM holds %d frames, expected %d", count, n);
    return g_fail;
}

static bool read_ppm(const char* path, int& w, int& h, std::vector<uint8_t>& px) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    int maxv;
    if (fscanf(f, "P6 %d %d %d", &w, &h, &maxv) != 3 || maxv != 255) { fclose(f); return false; }
    fgetc(f);
    px.resize(size_t(w) * h * 3);
    bool ok = fread(px.data(), 1, px.size(), f) == px.size();
    fclose(f);
    return ok;
}

static int test_shot(const char* path, int frame0) {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    if (!read_ppm(path, w, h, px)) { printf("  cannot read PPM %s\n", path); return 1; }
    CHECK(w == 1000 && h == 480, "size %dx%d, expected 1000x480", w, h);
    if (w != 1000 || h != 480) return g_fail;
    auto at = [&](int x, int y) { const uint8_t* p = &px[(size_t(y) * w + x) * 3]; return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]; };
    int bad = 0, blackbad = 0;
    for (int y = 0; y < 200; y++)
        for (int x = 0; x < 320; x++) {
            uint32_t e = expected_rgb(frame0, x, y);
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++)
                    if (at(2 * x + dx, 40 + 2 * y + dy) != e) bad++;
        }
    for (int x = 0; x < 640; x++) {
        for (int y = 0; y < 40; y++) if (at(x, y)) blackbad++;
        for (int y = 440; y < 480; y++) if (at(x, y)) blackbad++;
    }
    CHECK(bad == 0, "%d screen pixels differ from the expected doubled pattern (frame index %d)", bad, frame0);
    CHECK(blackbad == 0, "%d letterbox pixels are not black", blackbad);
    std::set<uint32_t> colors;
    int lit = 0;
    for (int y = 0; y < 480; y++)
        for (int x = 640; x < 1000; x++) { uint32_t c = at(x, y); colors.insert(c); if (c > 0xC0C0C0) lit++; }
    CHECK(colors.size() >= 6, "HUD has only %zu distinct colours", colors.size());
    CHECK(lit > 300, "HUD has too little bright text (%d px)", lit);
    return g_fail;
}

static int test_unit() {
    // keymap (follows doomgeneric_sdl.c)
    CHECK(sdl_to_doom_key(SDLK_LEFT) == 0xac && sdl_to_doom_key(SDLK_RIGHT) == 0xae, "left/right");
    CHECK(sdl_to_doom_key(SDLK_UP) == 0xad && sdl_to_doom_key(SDLK_DOWN) == 0xaf, "up/down");
    CHECK(sdl_to_doom_key(SDLK_LCTRL) == 0xa3 && sdl_to_doom_key(SDLK_RCTRL) == 0xa3, "ctrl = fire");
    CHECK(sdl_to_doom_key(SDLK_SPACE) == 0xa2, "space = use");
    CHECK(sdl_to_doom_key(SDLK_LSHIFT) == 0xb6 && sdl_to_doom_key(SDLK_RSHIFT) == 0xb6, "shift = run");
    CHECK(sdl_to_doom_key(SDLK_LALT) == 0xb8 && sdl_to_doom_key(SDLK_RALT) == 0xb8, "alt");
    CHECK(sdl_to_doom_key(SDLK_RETURN) == 13 && sdl_to_doom_key(SDLK_ESCAPE) == 27 && sdl_to_doom_key(SDLK_TAB) == 9, "enter/esc/tab");
    CHECK(sdl_to_doom_key(SDLK_F1) == 0xbb && sdl_to_doom_key(SDLK_F2) == 0xbc && sdl_to_doom_key(SDLK_F10) == 0xc4, "F1/F2/F10");
    CHECK(sdl_to_doom_key(SDLK_F11) == 0xd7 && sdl_to_doom_key(SDLK_F12) == 0xd8, "F11/F12");
    CHECK(sdl_to_doom_key(SDLK_a) == 'a' && sdl_to_doom_key(SDLK_z) == 'z' && sdl_to_doom_key(SDLK_0) == '0' && sdl_to_doom_key(SDLK_9) == '9', "letters/digits");
    CHECK(sdl_to_doom_key(SDLK_EQUALS) == '=' && sdl_to_doom_key(SDLK_MINUS) == '-' && sdl_to_doom_key(SDLK_COMMA) == ',', "= - ,");
    CHECK(sdl_to_doom_key(SDLK_BACKSPACE) == 0x7f, "backspace");
    CHECK(sdl_to_doom_key(SDLK_CAPSLOCK) == 0 && sdl_to_doom_key(SDLK_LGUI) == 0, "unmapped keys give 0");

    // monitor synthesis: pixel doubling and letterbox
    std::vector<uint8_t> fb(kFbBytes);
    uint32_t pal[256];
    for (int i = 0; i < 256; i++) pal[i] = 0xFF000000u | (uint32_t(i) << 16) | uint32_t(255 - i);
    for (unsigned i = 0; i < kFbBytes; i++) fb[i] = uint8_t(i * 7);
    std::vector<uint32_t> mon(kMonWidth * kMonHeight, 0xDEADBEEF);
    synth_monitor(fb.data(), pal, mon.data());
    int bad = 0;
    for (unsigned y = 0; y < kMonHeight; y++)
        for (unsigned x = 0; x < kMonWidth; x++) {
            uint32_t e = 0;
            if (y >= 40 && y < 440) e = pal[fb[((y - 40) / 2) * 320 + x / 2]] & 0xFFFFFF;
            if (mon[y * kMonWidth + x] != e) bad++;
        }
    CHECK(bad == 0, "synth_monitor: %d wrong pixels", bad);

    // metrics
    EngineInfo info; info.clk_hz = 50000000; info.speed_unit = SpeedUnit::Mhz;
    Stats a, z;
    a.cycles.set(100000000); a.instret.set(60000000);
    a.ic_access.set(1000); a.ic_miss.set(10); a.dc_access.set(500); a.dc_miss.set(50);
    a.branch.set(100); a.branch_miss.set(7); a.jump.set(40); a.jump_miss.set(4); a.flips.set(10);
    Metrics m = compute_metrics(info, a, z, 2.0);
    CHECK(m.speed.ok && std::fabs(m.speed.v - 50.0) < 1e-9, "MHz = 100M cycles / 2 s");
    CHECK(m.ipc.ok && std::fabs(m.ipc.v - 0.6) < 1e-9, "ipc");
    CHECK(m.ic_hit.ok && std::fabs(m.ic_hit.v - 0.99) < 1e-9 && std::fabs(m.dc_hit.v - 0.9) < 1e-9, "hit rates");
    CHECK(std::fabs(m.branch_acc.v - 0.93) < 1e-9 && std::fabs(m.jump_acc.v - 0.9) < 1e-9, "predictor accuracy");
    CHECK(std::fabs(m.sim_fps.v - 5.0) < 1e-9 && std::fabs(m.wall_fps.v - 5.0) < 1e-9, "fps: 10 frames in 2 s sim and 2 s wall");
    info.speed_unit = SpeedUnit::Mips;
    m = compute_metrics(info, a, z, 2.0);
    CHECK(std::fabs(m.speed.v - 30.0) < 1e-9, "MIPS = 60M instr / 2 s");
    Stats b = a;  // recent window: +1M cycles, +500k instr, +1 frame
    b.cycles.set(a.cycles.v + 1000000); b.instret.set(a.instret.v + 500000); b.flips.set(11);
    m = compute_metrics(info, b, a, 0.1);
    CHECK(std::fabs(m.ipc.v - 0.5) < 1e-9 && std::fabs(m.sim_fps.v - 50.0) < 1e-9 && std::fabs(m.wall_fps.v - 10.0) < 1e-9, "recent window deltas");
    CHECK(!m.ic_hit.ok, "no new I-cache accesses in the window -> unavailable");
    Stats iss_only; iss_only.cycles.set(1000); iss_only.instret.set(1000);
    m = compute_metrics(info, iss_only, z, 1.0);
    CHECK(m.ipc.ok && !m.ic_hit.ok && !m.dc_hit.ok && !m.branch_acc.ok && !m.sim_fps.ok, "unavailable counters stay unavailable");
    CHECK(fmt_group(1234567) == "1,234,567" && fmt_group(12) == "12" && fmt_group(1000) == "1,000", "fmt_group");
    CHECK(fmt_pct(Opt()) == "-" && fmt_pct(Opt(0.987)) == "98.7%" && fmt_fixed(Opt(), 2) == "-", "fmt");

    // engine factory
    std::string err;
    EngineOptions eo;
    CHECK(make_engine("iss", eo, err) != nullptr, "iss engine");
    { auto m = make_engine("model", eo, err); CHECK(m && m->info().name == "model" && m->info().speed_unit == SpeedUnit::Mhz, "model engine"); }
    CHECK(make_engine("bogus", eo, err) == nullptr && err.find("unknown") != std::string::npos, "bogus engine");
    eo.cosim = true;
    CHECK(make_engine("iss", eo, err) == nullptr && err.find("RTL engine") != std::string::npos, "cosim on iss rejected");
    CHECK(make_engine("model", eo, err) == nullptr && err.find("--cosim=model") != std::string::npos, "cosim on the model rejected");
    return g_fail;
}

int main(int argc, char** argv) {
    int rc = 1;
    if (argc >= 2 && !strcmp(argv[1], "unit")) rc = test_unit();
    else if (argc >= 4 && !strcmp(argv[1], "frames")) rc = test_frames(argv[2], atoi(argv[3]));
    else if (argc >= 4 && !strcmp(argv[1], "shot")) rc = test_shot(argv[2], atoi(argv[3]));
    else { fprintf(stderr, "usage: test_host unit | frames FILE N | shot FILE FRAME_INDEX\n"); return 2; }
    printf("test_host %s: %d checks passed, %d failed\n", argv[1], g_pass, g_fail);
    return rc ? 1 : 0;
}
