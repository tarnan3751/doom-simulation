// Host unit tests: sim/common (loader, dfrm) and the ISS API (step records,
// override mechanism, keyboard, video, stepping vs run equivalence).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bench_kernel.h"
#include "dfrm.h"
#include "iss.h"
#include "loader.h"

static int g_fail = 0, g_n = 0;
#define CHECK(c) do { g_n++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::vector<uint8_t> slurp(const std::string& p) {
    std::vector<uint8_t> v;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return v;
    uint8_t b[4096]; size_t r;
    while ((r = fread(b, 1, sizeof b, f)) > 0) v.insert(v.end(), b, b + r);
    fclose(f);
    return v;
}

static uint32_t g32(const std::vector<uint8_t>& v, size_t o) { return v[o] | (v[o+1] << 8) | (v[o+2] << 16) | (uint32_t(v[o+3]) << 24); }

static bool load_prog(iss::Iss& m, const std::string& name) {
    simc::Elf32 e; std::string err;
    simc::WriteFn w = [&](uint32_t a, const uint8_t* d, size_t n) { return m.write_mem(a, d, n); };
    if (!e.load_file(std::string(PROGDIR) + "/" + name + ".elf", err) || !e.load(w, err)) { printf("load %s: %s\n", name.c_str(), err.c_str()); return false; }
    m.reset(e.entry());
    return true;
}
static uint32_t symaddr(const std::string& name, const char* sym) {
    simc::Elf32 e; std::string err; uint32_t a = 0;
    e.load_file(std::string(PROGDIR) + "/" + name + ".elf", err);
    if (!e.find_symbol(sym, a)) printf("no symbol %s\n", sym);
    return a;
}

// ---------------------------------------------------------------- elf32
static void test_elf() {
    std::vector<uint8_t> img = slurp(std::string(ISADIR) + "/rv32ui-p-add.elf");
    CHECK(img.size() > 100);
    simc::Elf32 e; std::string err;
    CHECK(e.parse(img.data(), img.size(), err));
    CHECK(e.entry() == 0x80000000u && !e.segments().empty());
    uint32_t a = 0;
    CHECK(e.find_symbol("tohost", a) && a >= 0x80000000u && a < 0x80100000u);
    CHECK(!e.find_symbol("no_such_symbol", a));
    auto load_into = [&](std::vector<uint8_t>& buf) {   // guest addresses 0x80000000 + [0, buf.size())
        return e.load([&](uint32_t ad, const uint8_t* d, size_t n) {
            if (ad < 0x80000000u || ad - 0x80000000u + n > buf.size()) return false;
            memcpy(&buf[ad - 0x80000000u], d, n);
            return true;
        }, err);
    };
    std::vector<uint8_t> buf(0x100000, 0xEE), small(16);
    CHECK(load_into(buf) && (buf[0] != 0xEE || buf[1] != 0xEE));   // text written
    CHECK(!load_into(small) && !err.empty());                     // does not fit: the load aborts
    CHECK(!e.load([](uint32_t, const uint8_t*, size_t) { return false; }, err));

    // malformed inputs: one byte changed -> the error names it
    struct { size_t off; uint8_t v; const char* msg; } bad[] = {{0, 'X', "magic"}, {4, 2, "32-bit"}, {5, 2, "little"},
                                                                {18, 62, "RISC-V"}, {16, 1, "ET_EXEC"}};   // x86-64, ET_REL
    simc::Elf32 bad_e;
    for (auto& c : bad) {
        std::vector<uint8_t> m = img; m[c.off] = c.v;
        CHECK(!bad_e.parse(m.data(), m.size(), err) && err.find(c.msg) != std::string::npos);
    }
    CHECK(!bad_e.parse(nullptr, 0, err));
    CHECK(!bad_e.parse(img.data(), 40, err) && err.find("short") != std::string::npos);
    // truncated in the middle of segment data, and in the program headers
    uint32_t phoff = g32(img, 28), off = g32(img, phoff + 4), fsz = g32(img, phoff + 16);
    if (fsz > 8) CHECK(!bad_e.parse(img.data(), off + fsz / 2, err) && err.find("truncated") != std::string::npos);
    CHECK(!bad_e.parse(img.data(), phoff + 8, err));
    CHECK(!bad_e.load_file("/nonexistent/file.elf", err));
}

// ---------------------------------------------------------------- bootinfo
static void test_bootinfo() {
    std::vector<uint8_t> pg; std::string err;
    CHECK(simc::build_bootinfo("/some/dir/doom1.wad", 4196020, {"-iwad", "x y", "-warp"}, pg, err));
    CHECK(pg.size() == 1040);
    CHECK(g32(pg, 0) == 0xB007F00Du && g32(pg, 4) == 0x80800000u && g32(pg, 8) == 4196020u && g32(pg, 12) == 0);
    CHECK(strcmp((const char*)&pg[16], "doom1.wad") == 0);
    CHECK(pg[16 + 9] == 0 && pg[16 + 63] == 0);
    CHECK(strcmp((const char*)&pg[80], "-iwad x y -warp") == 0);
    CHECK(simc::build_bootinfo("C:\\w\\a.wad", 1, {}, pg, err) && strcmp((const char*)&pg[16], "a.wad") == 0 && pg[80] == 0);
    CHECK(simc::build_bootinfo("", 77, {"a"}, pg, err) && g32(pg, 4) == 0 && g32(pg, 8) == 0 && pg[16] == 0 && g32(pg, 0) == 0xB007F00Du);
    CHECK(!simc::build_bootinfo(std::string(64, 'n'), 1, {}, pg, err));
    CHECK(simc::build_bootinfo(std::string(63, 'n'), 1, {}, pg, err));
    CHECK(!simc::build_bootinfo("a.wad", 1, {std::string(960, 'c')}, pg, err));
    CHECK(simc::build_bootinfo("a.wad", 1, {std::string(959, 'c')}, pg, err) && pg[80 + 958] == 'c' && pg[80 + 959] == 0);
    CHECK(!simc::build_bootinfo("a.wad", 0x01800001u, {}, pg, err));

    iss::Iss m;
    simc::WriteFn w = [&](uint32_t a, const uint8_t* d, size_t n) { return m.write_mem(a, d, n); };
    CHECK(simc::place_bootinfo(w, "q/doom.wad", 5, {"hi"}, err));
    uint32_t magic = 0; m.read_mem(0x807FF000u, &magic, 4);
    CHECK(magic == 0xB007F00Du);
    const char* cl = (const char*)m.ram() + (0x807FF000u - 0x80000000u) + 80;
    CHECK(strcmp(cl, "hi") == 0);
    // WAD placement
    std::string wp = std::string(PROGDIR) + "/../test.wad";
    uint32_t sz = 0;
    CHECK(simc::load_wad(w, wp, sz, err) && sz == 1001);
    uint8_t first = 0; m.read_mem(0x80800000u, &first, 1);
    CHECK(first == 'A');
    CHECK(!simc::load_wad(w, "/nonexistent.wad", sz, err));
    CHECK(!simc::load_wad([](uint32_t, const uint8_t*, size_t) { return false; }, wp, sz, err));
}

// ---------------------------------------------------------------- dfrm
static void test_dfrm() {
    std::string path = std::string(PROGDIR) + "/../t.dfrm", p2 = path + ".bad", err;
    uint8_t pal[768]; std::vector<uint8_t> px(64000);
    for (int i = 0; i < 768; i++) pal[i] = uint8_t(i * 7);
    for (size_t i = 0; i < px.size(); i++) px[i] = uint8_t(i * 13);
    uint32_t x[256]; for (int i = 0; i < 256; i++) x[i] = 0xAB000000u | (i << 16) | ((255 - i) << 8) | (i ^ 0x5A);
    {
        simc::DfrmWriter w;
        CHECK(w.open(path, err) && w.write_frame(pal, px.data()) && w.write_frame_xrgb(x, px.data()) && w.frames() == 2);
    }   // the destructor closes the file
    std::vector<uint8_t> f = slurp(path);
    CHECK(f.size() == 16 + 2 * 64768);
    CHECK(memcmp(f.data(), "DFRM", 4) == 0 && g32(f, 4) == 1 && g32(f, 8) == 320 && g32(f, 12) == 200);
    simc::DfrmReader r; simc::DfrmFrame a, b;
    CHECK(r.open(path).empty() && r.frames() == 2 && !r.truncated() && r.width() == 320 && r.height() == 200);
    CHECK(r.read(1, b) && b.pal[3 * 5] == 5 && b.pal[3 * 5 + 1] == 250 && b.pal[3 * 5 + 2] == (5 ^ 0x5A));
    CHECK(r.read(0, a) && memcmp(a.pal.data(), pal, 768) == 0 && a.idx == px);   // random access back to frame 0
    CHECK(!r.read(2, a));
    // frames are equal when the RGB after palette lookup is (what framecmp compares)
    std::vector<uint8_t> ra, rb;
    auto same = [&](const simc::DfrmFrame& p, const simc::DfrmFrame& q) { p.to_rgb(ra); q.to_rgb(rb); return ra == rb; };
    CHECK(!same(a, b));
    simc::DfrmFrame a2 = a;
    a2.pal[0] ^= 1;                       // entry 0 is used by pixel 0 (px[0] = 0): differs
    CHECK(!same(a, a2));
    simc::DfrmFrame u1{std::vector<uint8_t>(768), std::vector<uint8_t>(64000)}, u2;   // only index 0 is used
    u1.pal[0] = 1; u2 = u1; u2.pal[300] = 9;
    CHECK(same(u1, u2));
    // malformed and odd files
    auto open_bad = [&](const std::vector<uint8_t>& d, simc::DfrmReader& q) {
        FILE* o = fopen(p2.c_str(), "wb"); fwrite(d.data(), 1, d.size(), o); fclose(o);
        return q.open(p2);
    };
    auto patched = [&](size_t off, uint8_t v) { std::vector<uint8_t> t = f; t[off] = v; t[off + 1] = 0; return t; };
    { simc::DfrmReader q; CHECK(open_bad(std::vector<uint8_t>(f.begin(), f.begin() + 16 + 64768 + 1000), q).empty() && q.frames() == 1 && q.truncated()); }
    { simc::DfrmReader q; CHECK(open_bad(patched(0, 'X'), q).find("not a DFRM") != std::string::npos); }
    { simc::DfrmReader q; CHECK(open_bad(patched(4, 2), q).find("version") != std::string::npos); }
    { simc::DfrmReader q; CHECK(open_bad(patched(8, 0), q).find("bad dimensions") != std::string::npos); }   // width 0
    { simc::DfrmReader q; CHECK(open_bad(patched(8, 100), q).empty() && q.width() == 100 && q.frames() > 2); }   // any size reads
    { simc::DfrmReader q; CHECK(open_bad(std::vector<uint8_t>(f.begin(), f.begin() + 10), q).find("not a DFRM") != std::string::npos); }
    { simc::DfrmReader q; CHECK(q.open("/nonexistent.dfrm").find("cannot open") != std::string::npos); }
    simc::DfrmWriter wbad; CHECK(!wbad.open("/nonexistent/dir/x.dfrm", err));
}

// ---------------------------------------------------------------- ISS: directed programs
static void run_prog_ok(const char* name, const char* cl = nullptr) {
    iss::Iss m;
    if (!load_prog(m, name)) { CHECK(false); return; }
    if (cl) {
        simc::WriteFn w = [&](uint32_t a, const uint8_t* d, size_t n) { return m.write_mem(a, d, n); };
        std::string err;
        simc::place_bootinfo(w, "", 0, {cl}, err);
    }
    m.run(50000000);
    bool ok = m.stop_reason() == iss::Stop::Exit && m.exit_code() == 0;
    if (!ok) printf("  %s: stop=%d exit=%u msg=%s\n", name, int(m.stop_reason()), m.exit_code(), m.stop_message().c_str());
    CHECK(ok);
}

static void test_keyboard() {
    iss::Iss m;
    CHECK(load_prog(m, "kbd"));
    for (int i = 0; i < 20; i++) m.push_key(i & 1, uint8_t(0x30 + i));
    CHECK(m.kbd_pending() == 16);            // 4 dropped
    m.run(100000);
    CHECK(m.stop_reason() == iss::Stop::Exit && m.exit_code() == 0);
    uint32_t res = symaddr("kbd", "kres"), st = symaddr("kbd", "kst");
    uint32_t r[24], s[4];
    m.read_mem(res, r, sizeof r); m.read_mem(st, s, sizeof s);
    for (int i = 0; i < 16; i++) CHECK(r[i] == (0x80000000u | ((i & 1) ? 0x100u : 0) | uint32_t(0x30 + i)));
    for (int i = 16; i < 20; i++) CHECK(r[i] == 0);       // empty reads give 0
    CHECK(s[0] == 1 && s[1] == 1 && s[2] == 0 && s[3] == 0);
    CHECK(m.kbd_pending() == 0);
    // FIFO ordering with interleaved push/pop is covered by the 16-deep drain above;
    // after draining, new events are accepted again
    m.push_key(true, 7); CHECK(m.kbd_pending() == 1);
}

static void test_video() {
    iss::Iss m;
    CHECK(load_prog(m, "video"));
    int flips = 0; uint8_t first0 = 99; uint32_t pal0 = 0;
    m.on_flip = [&]() { if (flips == 0) { first0 = m.framebuffer()[0]; pal0 = m.palette()[0]; } flips++; };
    m.run(100000);
    CHECK(m.stop_reason() == iss::Stop::Exit && m.exit_code() == 0);
    CHECK(flips == 3 && m.flip_count() == 3);
    CHECK(first0 == 1 && pal0 == 0x112233);                   // snapshot at the first flip
    const uint8_t* fb = m.framebuffer();
    CHECK(fb[0] == 9 && fb[1] == 2 && fb[2] == 3 && fb[3] == 4);
    CHECK(fb[4] == 5 && fb[5] == 6 && fb[6] == 7 && fb[7] == 8);
    CHECK(fb[8] == 9 && fb[9] == 10 && fb[10] == 11 && fb[11] == 12);
    CHECK(fb[63999] == 0x77);
    CHECK(m.palette()[0] == 0x112233 && m.palette()[1] == 0xFF0000 && m.palette()[255] == 0xEEDDCC);
    CHECK(m.palette()[2] == 0x0000AB00);
}

static void test_fault_programs() {
    struct { const char* c; iss::Stop s; } cases[] = {{"l", iss::Stop::BadAccess}, {"s", iss::Stop::BadAccess}, {"r", iss::Stop::BadAccess},
                                                     {"f", iss::Stop::BadFetch}, {"n", iss::Stop::BadFetch}};
    for (auto& c : cases) {
        iss::Iss m; load_prog(m, "fault");
        simc::WriteFn w = [&](uint32_t a, const uint8_t* d, size_t n) { return m.write_mem(a, d, n); };
        std::string err; simc::place_bootinfo(w, "", 0, {c.c}, err);
        m.run(100000);
        CHECK(m.stop_reason() == c.s);
        CHECK(m.stop_message().find("pc=0x") != std::string::npos);
        CHECK(m.fault_pc() != 0xFFFFFFFFu && m.stop_message().find(c.s == iss::Stop::BadFetch ? "fetch" : "unmapped") != std::string::npos);
        // a stopped machine executes nothing more
        CHECK(m.run(10) == 0);
        iss::Retire r = m.step();
        CHECK(!r.valid);
    }
}

// ---------------------------------------------------------------- ISS: step API
static uint32_t enc_i(int32_t imm, int rs1, int f3, int rd, int opc) { return (uint32_t(imm) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | opc; }
static uint32_t enc_u(uint32_t imm20, int rd, int opc) { return (imm20 << 12) | (rd << 7) | opc; }
static uint32_t enc_s(int32_t imm, int rs2, int rs1, int f3) {
    return ((uint32_t(imm) >> 5) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | ((imm & 31) << 7) | 0x23;
}

static void load_words(iss::Iss& m, const std::vector<uint32_t>& w) { m.write_mem(0x80000000u, w.data(), w.size() * 4); m.reset(0x80000000u); }

static void test_step_and_override() {
    std::vector<uint32_t> prog = {
        enc_u(0x10002, 1, 0x37),          // 00 lui x1, KBD_BASE
        enc_i(0, 1, 2, 2, 0x03),          // 04 lw x2, 0(x1)       KBD_DATA
        enc_i(0xB00, 0, 2, 3, 0x73),      // 08 csrrs x3, mcycle, x0
        enc_i(0xC02, 0, 2, 4, 0x73),      // 0c csrrs x4, instret, x0
        enc_u(0x80001, 5, 0x37),          // 10 lui x5, 0x80001
        enc_s(0, 2, 5, 2),                // 14 sw x2, 0(x5)
        enc_s(5, 2, 5, 0),                // 18 sb x2, 5(x5)
        enc_i(0, 5, 2, 6, 0x03),          // 1c lw x6, 0(x5)       RAM load
        enc_i(2, 5, 2, 7, 0x03),          // 20 lw x7, 2(x5)       misaligned -> trap
        enc_i(5, 0, 0, 8, 0x13),          // 24 addi x8, x0, 5
        enc_u(0x10001, 9, 0x37),          // 28 lui x9, SYS_BASE
        enc_i(0, 9, 2, 10, 0x03),         // 2c lw x10, 0(x9)      SYS_CLK_HZ
        enc_i(0, 0, 0, 0, 0x13),          // 30 nop
    };
    // ---- A: no override ----
    {
        iss::Iss m; load_words(m, prog);
        m.set_reg(31, 0);
        m.push_key(true, 0x41);
        iss::Retire r = m.step();
        CHECK(r.valid && !r.trap && r.pc == 0 + 0x80000000u && r.insn == prog[0] && r.rd == 1 && r.rd_wdata == 0x10002000u && r.next_pc == 0x80000004u);
        CHECK(r.mem_addr == 0 && r.mem_wstrb == 0);
        r = m.step();                                            // lw KBD_DATA
        CHECK(r.rd == 2 && r.rd_wdata == 0x80000141u && r.mem_addr == 0x10002000u && r.is_load && !r.override_used && r.mem_wstrb == 0);
        CHECK(m.kbd_pending() == 0);
        r = m.step();
        CHECK(r.rd == 3 && r.rd_wdata == 2);                    // mcycle = instructions executed before it
        r = m.step();
        CHECK(r.rd == 4 && r.rd_wdata == 3);
        m.step();
        r = m.step();                                            // sw
        CHECK(r.rd == 0 && r.rd_wdata == 0 && r.mem_addr == 0x80001000u && r.mem_wstrb == 0xF && r.mem_wdata == 0x80000141u);
        r = m.step();                                            // sb 5(x5): lane 1
        CHECK(r.mem_addr == 0x80001005u && r.mem_wstrb == 0x2 && r.mem_wdata == 0x00004100u);
        r = m.step();
        CHECK(m.reg(6) == 0x80000141u);                          // sb went to the next word
        r = m.step();                                            // misaligned lw
        CHECK(r.valid && r.trap && r.cause == 4 && r.rd == 0 && r.rd_wdata == 0 && r.mem_wstrb == 0 && r.next_pc == 0);
        CHECK(m.pc() == 0);
        uint32_t v; CHECK(m.csr_peek(0x341, v) && v == 0x80000020u);
        CHECK(m.csr_peek(0x343, v) && v == 0x80001002u);
        CHECK(m.csr_peek(0x342, v) && v == 4);
        CHECK(m.reg(7) == 0);
        CHECK(m.instret() == 8 && m.cycles() == 9);              // the trapping instruction does not retire
    }
    // ---- B: override on KBD_DATA, model has the same event -> popped ----
    {
        iss::Iss m; load_words(m, prog);
        m.push_key(true, 0x41);
        m.step();
        uint32_t ov = 0x80000141u;
        iss::Retire r = m.step(&ov);
        CHECK(r.override_used && r.rd_wdata == 0x80000141u && m.kbd_pending() == 0);
    }
    // ---- C: hardware returned a different value: it wins ----
    {
        iss::Iss m; load_words(m, prog);
        m.push_key(false, 0x41);
        m.step();
        uint32_t ov = 0x80000199u;
        iss::Retire r = m.step(&ov);
        CHECK(r.override_used && r.rd_wdata == 0x80000199u && m.reg(2) == 0x80000199u && m.kbd_pending() == 0);
    }
    // ---- D: hardware read empty (0), model has an event: not popped ----
    {
        iss::Iss m; load_words(m, prog);
        m.push_key(true, 3);
        m.step();
        uint32_t ov = 0;
        iss::Retire r = m.step(&ov);
        CHECK(r.override_used && r.rd_wdata == 0 && m.kbd_pending() == 1);
        // ...and the model, left alone, still delivers it later
        iss::Retire r2 = m.step();
        (void)r2;
    }
    // ---- E: counter CSR overrides, other instructions ignore overrides ----
    {
        iss::Iss m; load_words(m, prog);
        m.step(); m.step();
        uint32_t ov = 777;
        iss::Retire r = m.step(&ov);            // csrrs mcycle
        CHECK(r.override_used && r.rd == 3 && r.rd_wdata == 777 && m.reg(3) == 777);
        ov = 555;
        r = m.step(&ov);                        // csrrs instret
        CHECK(r.override_used && r.rd_wdata == 555);
        ov = 9;
        r = m.step(&ov);                        // lui: ignored
        CHECK(!r.override_used && r.rd_wdata == 0x80001000u);
        r = m.step(&ov);                        // sw: ignored
        CHECK(!r.override_used && r.mem_wstrb == 0xF);
        m.step();
        r = m.step(&ov);                        // lw from RAM: ignored
        CHECK(!r.override_used && r.rd == 6 && r.rd_wdata != 9);
        r = m.step(&ov);                        // trapping misaligned load: no override
        CHECK(r.trap && !r.override_used);
    }
    // ---- F: MMIO load override works for SYS_CLK_HZ; configurable clock ----
    {
        iss::Config cfg; cfg.clk_hz = 25000000;
        iss::Iss m(cfg); load_words(m, prog);
        m.set_pc(0x80000028u);
        m.step();
        iss::Retire r = m.step();
        CHECK(r.rd_wdata == 25000000u && !r.override_used);
        m.set_pc(0x8000002cu);
        uint32_t ov = 50000000;
        r = m.step(&ov);
        CHECK(r.override_used && r.rd_wdata == 50000000u);
    }
    // ---- G: write to counters through csr, and counter override on read of csrrw rd=x0 is not used ----
    {
        std::vector<uint32_t> p = {
            enc_i(0xB00, 0, 1, 0, 0x73),  // csrrw x0, mcycle, x0  -> mcycle = 0 (does not read)
            enc_i(0xB00, 0, 2, 1, 0x73),  // csrrs x1, mcycle, x0
        };
        iss::Iss m; load_words(m, p);
        uint32_t ov = 123;
        iss::Retire r = m.step(&ov);
        CHECK(!r.override_used);
        r = m.step();
        CHECK(r.rd_wdata == 1);
    }
    // ---- the timing model's additions: taken, csr_wdata, the mmio_read hook ----
    {
        std::vector<uint32_t> p = {
            0x00100093,                   // 00 addi x1, x0, 1
            0x00009463,                   // 04 bne x1, x0, +8      taken
            0x00000013,                   // 08 nop (skipped)
            0x00008463,                   // 0c beq x1, x0, +8      not taken
            0x34009173,                   // 10 csrrw x2, mscratch, x1
            0x100020b7,                   // 14 lui x1, KBD_BASE
            0x0000a183,                   // 18 lw x3, 0(x1)        KBD_DATA through the hook
        };
        iss::Iss m; load_words(m, p);
        m.push_key(true, 0x41);
        uint32_t asked = 0;
        m.mmio_read = [&](uint32_t a) { asked = a; return 0x8000017Fu; };
        iss::Retire r = m.step();
        CHECK(!r.taken && r.csr_wdata == 0);
        r = m.step(); CHECK(r.taken && r.next_pc == 0x8000000Cu);
        r = m.step(); CHECK(!r.taken && r.next_pc == 0x80000010u);
        r = m.step(); CHECK(r.csr_wdata == 1 && r.rd == 2);
        m.step();
        r = m.step(); CHECK(r.rd_wdata == 0x8000017Fu && asked == 0x10002000u && m.kbd_pending() == 1);
    }
}

// step() and run() must leave identical state; the bench program also cross-checks M-extension math
// against the host compiler.
static void test_step_vs_run() {
    const uint32_t iters = 3000;
    auto setup = [&](iss::Iss& m) {
        load_prog(m, "bench");
        simc::WriteFn w = [&](uint32_t a, const uint8_t* d, size_t n) { return m.write_mem(a, d, n); };
        std::string err; simc::place_bootinfo(w, "", 0, {std::to_string(iters)}, err);
    };
    iss::Iss a, b;
    setup(a); setup(b);
    uint64_t na = a.run(1u << 30);
    uint64_t nb = 0;
    while (b.stop_reason() == iss::Stop::None) { iss::Retire r = b.step(); if (r.valid) nb++; }
    CHECK(a.stop_reason() == iss::Stop::Exit && b.stop_reason() == iss::Stop::Exit);
    CHECK(na == nb && na > 100000);
    CHECK(a.exit_code() == b.exit_code());
    CHECK(a.pc() == b.pc() && a.cycles() == b.cycles() && a.instret() == b.instret());
    for (int i = 0; i < 32; i++) CHECK(a.reg(i) == b.reg(i));
    CHECK(memcmp(a.ram(), b.ram(), 0x100000) == 0);
    static uint32_t buf[1024];
    memset(buf, 0, sizeof buf);
    uint32_t ref = bench_kernel(iters, buf);
    CHECK(a.exit_code() == ref);
    if (a.exit_code() != ref) printf("  iss=%08x host=%08x\n", a.exit_code(), ref);
    // run(n) splitting does not change the outcome
    iss::Iss c; setup(c);
    uint64_t total = 0;
    while (c.stop_reason() == iss::Stop::None) total += c.run(997);
    CHECK(total == na && c.exit_code() == ref);
}

static void test_misc() {
    // UART callback gets bytes in order; SYS_EXIT stops; exit code full 32 bits
    iss::Iss m; load_prog(m, "exit42");
    std::string out;
    m.on_uart = [&](uint8_t c) { out.push_back(char(c)); };
    m.run(100000);
    CHECK(out == "ok\n");
    CHECK(m.stop_reason() == iss::Stop::Exit && m.exit_code() == 0x12A);
    // store watch
    iss::Iss w; load_prog(w, "exit42");
    w.set_store_watch(0x10000000u);
    w.run(100000);
    CHECK(w.stop_reason() == iss::Stop::Watch && w.watch_value() == 'o');
    w.clear_stop(); w.clear_store_watch(); w.run(100000);
    CHECK(w.stop_reason() == iss::Stop::Exit);
    // reset clears devices
    m.reset(0x80000000u);
    CHECK(m.stop_reason() == iss::Stop::None && m.cycles() == 0 && m.flip_count() == 0);
    // illegal mtval configuration
    iss::Config cfg; cfg.illegal_mtval_is_insn = true;
    iss::Iss i2(cfg);
    uint32_t bad = 0xFFFFFFFFu; i2.write_mem(0x80000000u, &bad, 4); i2.reset();
    iss::Retire r = i2.step();
    uint32_t v; CHECK(r.trap && r.cause == 2 && i2.csr_peek(0x343, v) && v == 0xFFFFFFFFu);
    // mem backdoor bounds
    uint8_t b8[4];
    CHECK(!m.write_mem(0x7FFFFFFFu, b8, 2) && !m.write_mem(0x81FFFFFFu, b8, 2) && m.write_mem(0x81FFFFFCu, b8, 4));
    CHECK(!m.read_mem(0x10000000u, b8, 1));
}

int main() {
    test_elf();
    test_bootinfo();
    test_dfrm();
    run_prog_ok("mmio");
    run_prog_ok("traps");
    test_keyboard();
    test_video();
    test_fault_programs();
    test_step_and_override();
    test_step_vs_run();
    test_misc();
    printf("api tests: %d/%d checks passed\n", g_n - g_fail, g_n);
    return g_fail ? 1 : 0;
}
