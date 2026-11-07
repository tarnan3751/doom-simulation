// tb_main.cpp - Verilator testbench for rv32_core alone (rtl/core/README.md, "Verification").
//
// The core runs against a C++ memory that implements the request / held-response
// contract of ARCHITECTURE.md section 3 on both ports, with seeded, independently
// randomised response latency, and that checks the core's side of the contract on
// every cycle. Every retired instruction is compared with the golden ISS (sim/iss).
#include <verilated.h>
#include <verilated_vcd_c.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "Vrv32_core.h"
#include "Vrv32_core___024root.h"
#include "iss.h"
#include "loader.h"
#include "rand_prog.h"

namespace {

constexpr uint32_t RESET_PC = iss::kRamBase;
constexpr uint64_t MAX_CYCLES = 20'000'000, HANG_CYCLES = 50'000;

// functional coverage points (bit i of rv32_core.cov_events)
constexpr int NCOV = 44;
const char* const COV_NAMES[NCOV] = {
    "fwd rs1<-MEM", "fwd rs1<-WB", "fwd rs2<-MEM", "fwd rs2<-WB", "fwd store data", "fwd AGU base",
    "fwd branch operand", "fwd jalr base", "fwd CSR source", "fwd mul/div operand", "load-use stall",
    "load-use + redirect", "regfile write-through", "branch mispredict", "jump mispredict",
    "non-CF predicted taken", "trap", "mret", "RAS return hit", "taken branch predicted", "mul/div busy cycle",
    "mul/div done, memory stall", "I-port-only stall", "D-port-only stall", "both ports stalled",
    "load waiting in MEM", "store issued", "misaligned ld/st trap", "misaligned target trap", "illegal insn trap",
    "ecall/ebreak", "CSR write", "minstret write", "call", "divide by zero", "INT_MIN/-1",
    "flush beats load-use", "redirect held by stall", "load-use during stall", "load base fwd from WB",
    "store data fwd from WB", "jump predicted", "branch not taken", "divide, 8-24 quotient bits"};

// response latency: random on the I-port, the D-port or both, or phases of very slow responses
enum Mode { FAST, RANDI, RANDD, BOTH, BURST, NMODES };
const char* const MODE_NAMES[NMODES] = {"fast", "randi", "randd", "both", "burst"};

class LatGen {
public:
    enum Kind { Fixed1, Random, Bursty };
    LatGen(Kind k, uint64_t seed) : kind_(k), rng_(seed) {}
    unsigned next() {
        if (kind_ == Fixed1) return 1;
        if (kind_ == Random) return normal();
        if (burst_left_ > 0) { burst_left_--; return 10 + unsigned(rng_() % 120); }
        if (calm_left_ > 0) { calm_left_--; return normal(); }
        if (rng_() % 3 == 0) burst_left_ = 2 + unsigned(rng_() % 40);
        else calm_left_ = 10 + unsigned(rng_() % 300);
        return normal();
    }
    unsigned init_wait() { return kind_ == Fixed1 ? 0 : unsigned(rng_() % 40); }
    uint32_t poison() { return uint32_t(rng_()); }

private:
    unsigned normal() {
        unsigned r = unsigned(rng_() % 100);
        if (r < 55) return 1;
        if (r < 80) return 2 + unsigned(rng_() % 3);
        if (r < 95) return 5 + unsigned(rng_() % 12);
        return 17 + unsigned(rng_() % 40);
    }
    Kind kind_;
    std::mt19937_64 rng_;
    unsigned burst_left_ = 0, calm_left_ = 0;
};

// One slave port: holds the response of the last accepted request (R3); garbage while not ready.
struct Port {
    explicit Port(LatGen g) : lat(g) {}
    LatGen   lat;
    unsigned busy = 0;        // cycles until the outstanding request completes
    unsigned init = 0;        // initialisation period after reset with rsp_ready = 0 (R6)
    uint32_t rdata = 0;
    bool ready() const { return init == 0 && busy == 0; }
    uint32_t out_rdata() { return ready() ? rdata : lat.poison(); }
    void edge(bool accept, uint32_t data) {   // `accept` = req_valid sampled before the edge
        if (init > 0) { init--; return; }
        if (busy > 0) busy--;
        if (accept) { rdata = data; busy = lat.next() - 1; }
    }
};

// The DUT's memory: the 32 MB of RAM. Fetches are speculative and may go anywhere (0 there).
struct Ram {
    uint8_t* m = static_cast<uint8_t*>(std::calloc(iss::kRamSize, 1));
    ~Ram() { std::free(m); }
    bool load(uint32_t a, const uint8_t* d, size_t n) {
        if (a < RESET_PC || uint64_t(a - RESET_PC) + n > iss::kRamSize) return false;
        std::memcpy(m + (a - RESET_PC), d, n);
        return true;
    }
    uint8_t* word(uint32_t a) { return a - RESET_PC < iss::kRamSize ? m + ((a - RESET_PC) & ~3u) : nullptr; }
    uint32_t read32(uint32_t a) { uint32_t v = 0; if (uint8_t* p = word(a)) std::memcpy(&v, p, 4); return v; }
    void write(uint32_t a, uint32_t d, unsigned strb) {   // byte lanes of the aligned word (R7)
        if (uint8_t* p = word(a))
            for (int i = 0; i < 4; i++) if ((strb >> i) & 1) p[i] = uint8_t(d >> (8 * i));
    }
};

// ============================================================================
// one run
// ============================================================================
struct RunConfig {
    std::string name, elf;       // riscv-test / benchmark ELF, or empty for a random program
    bool        random = false, rand_reset = false, print_bench = false;
    uint64_t    rand_seed = 0, seed = 1;
    RandOptions rand_opt;
    Mode        mode = FAST;
    std::string vcd;
};

struct Stats {
    uint64_t cycles = 0, retired = 0, checked = 0, traps = 0, branches = 0, branch_miss = 0, jumps = 0,
             jump_miss = 0, loads = 0, stores = 0, cov[NCOV] = {};
    void add(const Stats& o) {
        cycles += o.cycles; retired += o.retired; checked += o.checked; traps += o.traps; branches += o.branches;
        branch_miss += o.branch_miss; jumps += o.jumps; jump_miss += o.jump_miss; loads += o.loads; stores += o.stores;
        for (int k = 0; k < NCOV; k++) cov[k] += o.cov[k];
    }
};

struct RunResult : Stats {
    bool        pass = false;
    std::string msg;
    std::vector<std::pair<std::string, uint32_t>> bench;
};

std::string hex(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "%08x", v); return b; }

std::string fmt_rec(const iss::Retire& t) {
    char b[200];
    std::snprintf(b, sizeof b, "pc=%08x insn=%08x trap=%d rd=x%-2u wd=%08x maddr=%08x strb=%x wdata=%08x npc=%08x",
                  t.pc, t.insn, int(t.trap), t.rd, t.rd_wdata, t.mem_addr, t.mem_wstrb, t.mem_wdata, t.next_pc);
    return b;
}

bool same(const iss::Retire& a, const iss::Retire& b) {
    return a.pc == b.pc && a.insn == b.insn && a.trap == b.trap && a.rd == b.rd && a.rd_wdata == b.rd_wdata &&
           a.mem_addr == b.mem_addr && a.mem_wstrb == b.mem_wstrb && a.mem_wdata == b.mem_wdata && a.next_pc == b.next_pc;
}

// The counter CSRs as the RTL implements them, which the ISS (one cycle per instruction) cannot
// know: a CSR instruction sees mcycle = the cycle in which it leaves EX and minstret = the
// non-trapping instructions before it, and a write replaces that cycle's / instruction's
// increment. Each counter read is handed to the ISS as its override value.
struct Counters {
    uint64_t cyc0 = 0, cyc_at = 0, instret = 0;          // mcycle(c) = cyc0 + (c - cyc_at)

    iss::Retire step(iss::Iss& ref, uint64_t commit_cycle) {
        uint32_t in = 0;
        ref.read_mem(ref.pc(), &in, 4);
        const uint32_t csr = in >> 20, op = (in >> 12) & 3, rs1 = (in >> 15) & 31;
        const bool cyc = csr == 0xB00 || csr == 0xB80 || csr == 0xC00 || csr == 0xC80;
        const bool ctr = (in & 0x7f) == 0x73 && op != 0 &&
                         (cyc || csr == 0xB02 || csr == 0xB82 || csr == 0xC02 || csr == 0xC82);
        const uint64_t cur = cyc ? cyc0 + (commit_cycle - cyc_at) : instret;
        const bool hi = csr & 0x80;
        const uint32_t old = uint32_t(hi ? cur >> 32 : cur), src = (in & (1u << 14)) ? rs1 : ref.reg(rs1);
        const iss::Retire r = ref.step(ctr ? &old : nullptr);
        if (!r.valid || r.trap) return r;
        if (ctr && (op == 1 || rs1 != 0)) {                  // a write (to a read-only shadow it traps)
            const uint32_t v = op == 1 ? src : op == 2 ? (old | src) : (old & ~src);
            const uint64_t nv = hi ? (cur & 0xffffffffull) | (uint64_t(v) << 32) : (cur & ~0xffffffffull) | v;
            if (!cyc) { instret = nv; return r; }
            cyc0 = nv;
            cyc_at = commit_cycle + 1;
        }
        instret++;
        return r;
    }
};

RunResult run_one(const RunConfig& cfg) {
    RunResult res;
    auto fail = [&](const std::string& m) { res.pass = false; res.msg = m; return res; };

    // ---- program: into the DUT's memory and the reference ----
    Ram ram;
    iss::Iss ref;
    uint32_t tohost = 0;
    simc::Elf32 elf;
    std::string err;
    const simc::WriteFn write = [&](uint32_t a, const uint8_t* d, size_t n) { return ram.load(a, d, n) && ref.write_mem(a, d, n); };
    if (cfg.random) {
        const RandProgram p = gen_random_program(cfg.rand_seed, cfg.rand_opt, RESET_PC);
        for (auto& b : p.bytes) if (!write(b.first, &b.second, 1)) return fail("random program outside RAM");
        for (auto& w : p.words) {
            uint8_t b[4];
            std::memcpy(b, &w.second, 4);
            if (!write(w.first, b, 4)) return fail("random program outside RAM");
        }
        tohost = p.tohost;
    } else {
        if (!elf.load_file(cfg.elf, err) || !elf.load(write, err)) return fail(err);
        if (!elf.find_symbol("tohost", tohost)) return fail("no tohost symbol in " + cfg.elf);
    }
    ref.reset(RESET_PC);
    Counters ctr;

    // ---- model ----
    auto ctx = std::make_unique<VerilatedContext>();
    ctx->randReset(cfg.rand_reset ? 2 : 0);
    ctx->randSeed(int(cfg.seed & 0x7fffffff));
    std::unique_ptr<VerilatedVcdC> vcd;
    if (!cfg.vcd.empty()) ctx->traceEverOn(true);
    auto top = std::make_unique<Vrv32_core>(ctx.get(), "");
    if (!cfg.vcd.empty()) {
        vcd = std::make_unique<VerilatedVcdC>();
        top->trace(vcd.get(), 99);
        vcd->open(cfg.vcd.c_str());
    }
    uint64_t tstamp = 0;
    auto clock = [&](int level) {
        top->clk = level;
        top->eval();
        if (vcd) vcd->dump(tstamp++);
    };

    const Mode m = cfg.mode;
    auto kind = [&](bool rnd) { return m == BURST ? LatGen::Bursty : rnd ? LatGen::Random : LatGen::Fixed1; };
    Port ip(LatGen(kind(m == RANDI || m == BOTH), cfg.seed * 0x9e3779b97f4a7c15ull + 1));
    Port dp(LatGen(kind(m == RANDD || m == BOTH), cfg.seed * 0xc2b2ae3d27d4eb4full + 2));

    struct DReq { uint32_t addr; bool we; uint32_t strb, wdata; };
    std::deque<DReq> dfifo;          // D-port requests not yet matched with a retired load/store
    std::deque<uint64_t> commits;    // EX-commit cycle of each instruction in flight (program order)
    std::deque<iss::Retire> history;
    bool have_prev = false;
    uint32_t prev_npc = 0;
    uint64_t last_retire = 0;

    // ---- reset ----
    clock(0);
    top->rst = 1;
    for (int i = 0; i < 6; i++) {
        top->imem_rsp_ready = (i & 1);
        top->dmem_rsp_ready = (i & 2) ? 1 : 0;
        top->imem_rsp_rdata = ip.lat.poison();
        top->dmem_rsp_rdata = dp.lat.poison();
        top->eval();
        if (top->imem_req_valid || top->dmem_req_valid) return fail("request presented while rst is high (R6)");
        clock(1);
        clock(0);
    }
    top->rst = 0;
    ip.init = ip.lat.init_wait();
    dp.init = dp.lat.init_wait();

    // ---- run ----
    for (uint64_t cyc = 0;; cyc++) {
        if (cyc >= MAX_CYCLES) return fail("timeout after " + std::to_string(cyc) + " cycles");
        if (cyc - last_retire > HANG_CYCLES)
            return fail("hang: no instruction retired for " + std::to_string(HANG_CYCLES) + " cycles at cycle " + std::to_string(cyc));

        top->imem_rsp_ready = ip.ready();
        top->imem_rsp_rdata = ip.out_rdata();
        top->dmem_rsp_ready = dp.ready();
        top->dmem_rsp_rdata = dp.out_rdata();
        top->eval();

        // ---------------- retire trace ----------------
        if (top->trace_valid) {
            iss::Retire t;
            t.trap = top->trace_trap;
            t.pc = top->trace_pc;
            t.insn = top->trace_insn;
            t.rd = top->trace_rd;
            t.rd_wdata = top->trace_rd_wdata;
            t.mem_addr = top->trace_mem_addr;
            t.mem_wstrb = top->trace_mem_wstrb;
            t.mem_wdata = top->trace_mem_wdata;
            t.next_pc = top->trace_next_pc;
            last_retire = cyc;
            res.retired++;
            res.traps += t.trap;
            history.push_back(t);
            if (history.size() > 24) history.pop_front();
            auto ctxdump = [&]() {
                std::string s;
                for (auto& h : history) s += "\n    " + fmt_rec(h);
                return s;
            };
            const uint32_t opc = t.insn & 0x7f;
            const bool is_ld = opc == 0x03, is_st = opc == 0x23;
            if (have_prev && t.pc != prev_npc)
                return fail("trace pc " + hex(t.pc) + " != previous next_pc " + hex(prev_npc) + ctxdump());
            have_prev = true;
            prev_npc = t.next_pc;
            if (t.rd == 0 && t.rd_wdata != 0) return fail("trace rd=0 with nonzero rd_wdata" + ctxdump());
            if (t.trap && (t.rd || t.mem_addr || t.mem_wstrb || t.mem_wdata))
                return fail("trapped instruction reports a register/memory effect" + ctxdump());
            if (t.insn != ram.read32(t.pc)) return fail("trace insn differs from memory at pc" + ctxdump());
            if (!t.trap && (is_ld || is_st)) {
                (is_ld ? res.loads : res.stores)++;
                if (dfifo.empty()) return fail("retired load/store without a D-port request" + ctxdump());
                const DReq q = dfifo.front();
                dfifo.pop_front();
                if (q.addr != t.mem_addr || q.we != is_st || (is_st && (q.strb != t.mem_wstrb || q.wdata != t.mem_wdata)))
                    return fail("D-port request (addr=" + hex(q.addr) + " we=" + std::to_string(q.we) + " strb=" +
                                std::to_string(q.strb) + " wdata=" + hex(q.wdata) +
                                ") does not match the retiring instruction (extra, missing or wrong request)" + ctxdump());
            } else if (t.mem_addr || t.mem_wstrb || t.mem_wdata) {
                return fail("non-memory instruction reports a memory access" + ctxdump());
            }
            if (commits.empty()) return fail("retired instruction without an EX commit" + ctxdump());
            const iss::Retire r = ctr.step(ref, commits.front());
            commits.pop_front();
            if (!r.valid) return fail("reference stopped: " + ref.stop_message() + ctxdump());
            res.checked++;
            if (!same(r, t))
                return fail("lockstep mismatch at retired instruction #" + std::to_string(res.retired) + " (cycle " +
                            std::to_string(cyc) + ")\n    DUT " + fmt_rec(t) + "\n    REF " + fmt_rec(r) +
                            "\n  history (oldest first):" + ctxdump());
            if (t.mem_wstrb && (t.mem_addr & ~3u) == (tohost & ~3u)) {
                res.cycles = cyc;
                if (t.mem_wdata != 1)
                    return fail("tohost = " + std::to_string(t.mem_wdata) + " (failing test #" +
                                std::to_string(t.mem_wdata >> 1) + ")");
                res.pass = true;
                break;
            }
        }
        const uint64_t ev = top->rootp->rv32_core__DOT__cov_events;
        for (int i = 0; i < NCOV; i++) res.cov[i] += (ev >> i) & 1;
        if ((ev >> 47) & 1) commits.push_back(cyc);   // after the trace: commit and retire never coincide
        res.branches += top->perf_branch;
        res.branch_miss += top->perf_branch_miss;
        res.jumps += top->perf_jump;
        res.jump_miss += top->perf_jump_miss;
        if (top->perf_branch_miss && !top->perf_branch) return fail("perf_branch_miss without perf_branch");
        if (top->perf_jump_miss && !top->perf_jump) return fail("perf_jump_miss without perf_jump");

        // ---------------- requests (core side of the contract) ----------------
        const bool ireq = top->imem_req_valid, dreq = top->dmem_req_valid, dwe = top->dmem_req_we;
        const uint32_t iaddr = top->imem_req_addr, daddr = top->dmem_req_addr, dstrb = top->dmem_req_wstrb,
                       dwdata = top->dmem_req_wdata;
        if (ireq && !ip.ready()) return fail("imem_req_valid while imem_rsp_ready=0 (R1) at cycle " + std::to_string(cyc));
        if (dreq && !dp.ready()) return fail("dmem_req_valid while dmem_rsp_ready=0 (R1) at cycle " + std::to_string(cyc));
        if (ireq && (iaddr & 3)) return fail("misaligned fetch request " + hex(iaddr));
        if (dreq) {
            if (!dwe && dstrb) return fail("dmem_req_wstrb != 0 on a load (R7)");
            if (dwe) {
                const unsigned lo = daddr & 3;
                const bool ok = (dstrb == (1u << lo)) || (dstrb == 0x3 && lo == 0) || (dstrb == 0xc && lo == 2) ||
                                (dstrb == 0xf && lo == 0);
                if (!ok) return fail("illegal store strobe pattern " + std::to_string(dstrb) + " for address " + hex(daddr));
                for (int i = 0; i < 4; i++)
                    if (!((dstrb >> i) & 1) && ((dwdata >> (8 * i)) & 0xff)) return fail("store data in a disabled byte lane");
            }
            dfifo.push_back({daddr, dwe, dwe ? dstrb : 0, dwe ? dwdata : 0});
            if (dfifo.size() > 3) return fail("more D-port requests than instructions in flight (duplicate request?)");
        }

        // ---------------- clock edge: accepted requests are performed once (R4) ----------------
        clock(1);
        const uint32_t idata = ireq ? ram.read32(iaddr) : 0;
        uint32_t ddata = 0;
        if (dreq && dwe) ram.write(daddr, dwdata, dstrb);
        else if (dreq) ddata = ram.read32(daddr);
        ip.edge(ireq, idata);
        dp.edge(dreq, dwe ? dp.lat.poison() : ddata);
        clock(0);
    }
    if (vcd) vcd->close();
    if (cfg.print_bench)
        for (const char* n : {"bench_cycles", "bench_instret", "bench_iters", "bench_body", "bench_result"}) {
            uint32_t a;
            if (elf.find_symbol(n, a)) res.bench.push_back({n, ram.read32(a)});
        }
    top->final();
    return res;
}

// ============================================================================
// suites
// ============================================================================
std::vector<std::string> list_elfs(const std::string& dir) {
    std::vector<std::string> v;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 4 && n.substr(n.size() - 4) == ".elf") v.push_back(dir + "/" + n);
        }
        closedir(d);
    }
    std::sort(v.begin(), v.end());
    return v;
}

std::string basename_noext(const std::string& p) {
    const std::string b = p.substr(p.find_last_of('/') + 1);
    return b.substr(0, b.rfind('.'));
}

Stats run_all(std::vector<RunConfig>& cfgs, unsigned jobs, uint64_t& fails) {
    std::atomic<size_t> next{0};
    std::mutex mu;
    Stats tot;
    auto worker = [&]() {
        for (size_t i; (i = next++) < cfgs.size();) {
            const RunResult r = run_one(cfgs[i]);
            std::lock_guard<std::mutex> g(mu);
            tot.add(r);
            if (!r.pass && fails++ < 10)
                std::printf("FAIL %s [mode=%s seed=%" PRIu64 "%s]: %s\n", cfgs[i].name.c_str(), MODE_NAMES[cfgs[i].mode],
                            cfgs[i].seed, cfgs[i].rand_reset ? " rand-reset" : "", r.msg.c_str());
            std::fflush(stdout);
        }
    };
    std::vector<std::thread> th;
    for (unsigned j = 0; j < std::max(1u, jobs); j++) th.emplace_back(worker);
    for (auto& t : th) t.join();
    return tot;
}

void usage() {
    std::puts(
        "usage: tb_core [options]\n"
        "  single run:   --elf FILE (riscv-test or benchmark) | --random SEED (generated program)\n"
        "  suites:       --suite isa --elf-dir DIR (every *.elf x modes x seeds) | --suite random (--count N programs x modes)\n"
        "  --mode M | --modes a,b|all   latency modes: fast randi randd both burst\n"
        "  --seed N                     latency/reset seed (single run); --seeds N per ELF, --seed-base N (suites)\n"
        "  --count N, --items N         random programs, top-level items per program (default 400)\n"
        "  --rand-reset 0|1|mixed       randomise all un-reset state (default: mixed in suites, 0 single)\n"
        "  -j N                         parallel runs (suites)\n"
        "  --vcd FILE, --bench          waveform, benchmark statistics (single run)\n"
        "  --label TEXT                 name printed in the suite summary\n"
        "  --require-coverage           suite fails if any functional coverage point is never hit\n"
        "  --coverage                   print the functional coverage counters");
}

}  // namespace

int main(int argc, char** argv) {
    std::string elf, suite, elf_dir, vcd, label, rand_reset;
    bool have_random = false, bench = false, req_cov = false, show_cov = false;
    uint64_t rseed = 0, seed = 1, seed_base = 1;
    unsigned seeds = 1, count = 10, jobs = std::max(1u, std::thread::hardware_concurrency());
    RandOptions ropt;
    std::vector<Mode> modes = {FAST};

    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        auto num = [&]() { return std::stoull(val()); };
        if (a == "--elf") elf = val();
        else if (a == "--random") { have_random = true; rseed = num(); }
        else if (a == "--suite") suite = val();
        else if (a == "--elf-dir") elf_dir = val();
        else if (a == "--mode" || a == "--modes") {
            std::string s = val(), x;
            modes.clear();
            if (s == "all") s = "fast,randi,randd,both,burst";
            std::stringstream ss(s);
            while (std::getline(ss, x, ',')) {
                const auto* p = std::find(MODE_NAMES, MODE_NAMES + NMODES, x);
                if (p == MODE_NAMES + NMODES) { std::fprintf(stderr, "unknown mode %s\n", x.c_str()); return 2; }
                modes.push_back(Mode(p - MODE_NAMES));
            }
        }
        else if (a == "--seed") seed = num();
        else if (a == "--seeds") seeds = unsigned(num());
        else if (a == "--seed-base") seed_base = num();
        else if (a == "--count") count = unsigned(num());
        else if (a == "--items") ropt.items = unsigned(num());
        else if (a == "--rand-reset") rand_reset = val();
        else if (a == "-j") jobs = unsigned(num());
        else if (a == "--vcd") vcd = val();
        else if (a == "--bench") bench = true;
        else if (a == "--label") label = val();
        else if (a == "--require-coverage") req_cov = true;
        else if (a == "--coverage") show_cov = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
    }
    auto print_cov = [&](const uint64_t* cov) {
        if (show_cov)
            for (int k = 0; k < NCOV; k++) std::printf("  cov %-28s %" PRIu64 "\n", COV_NAMES[k], cov[k]);
    };

    // ---------------- single run ----------------
    if (suite.empty()) {
        if (elf.empty() && !have_random) { usage(); return 2; }
        RunConfig c;
        c.elf = elf;
        c.random = have_random;
        c.rand_seed = rseed;
        c.rand_opt = ropt;
        c.mode = modes.empty() ? FAST : modes[0];
        c.seed = seed;
        c.rand_reset = rand_reset == "1";
        c.vcd = vcd;
        c.print_bench = bench;
        c.name = have_random ? "random-" + std::to_string(rseed) : basename_noext(elf);
        const auto t0 = std::chrono::steady_clock::now();
        const RunResult r = run_one(c);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("%s %s mode=%s seed=%" PRIu64 ": cycles=%" PRIu64 " retired=%" PRIu64 " traps=%" PRIu64
                    " CPI=%.3f lockstep=%" PRIu64 " branches=%" PRIu64 " (miss %" PRIu64 ") jumps=%" PRIu64
                    " (miss %" PRIu64 ") loads=%" PRIu64 " stores=%" PRIu64 " [%.2fs, %.2f Mcyc/s]\n",
                    r.pass ? "PASS" : "FAIL", c.name.c_str(), MODE_NAMES[c.mode], c.seed, r.cycles, r.retired, r.traps,
                    r.retired ? double(r.cycles) / double(r.retired) : 0.0, r.checked, r.branches, r.branch_miss,
                    r.jumps, r.jump_miss, r.loads, r.stores, secs, double(r.cycles) / secs / 1e6);
        uint32_t bc = 0, bi = 0;
        for (auto& b : r.bench) {
            std::printf("  %s = %u\n", b.first.c_str(), b.second);
            if (b.first == "bench_cycles") bc = b.second;
            if (b.first == "bench_instret") bi = b.second;
        }
        if (bi) std::printf("  kernel CPI = %.4f\n", double(bc) / double(bi));
        print_cov(r.cov);
        if (!r.pass) std::printf("  %s\n", r.msg.c_str());
        return r.pass ? 0 : 1;
    }

    // ---------------- suites ----------------
    std::vector<RunConfig> cfgs;
    auto rr = [&](uint64_t k) { return rand_reset == "1" || (rand_reset != "0" && k % 2 == 1); };
    if (suite == "isa") {
        const auto elfs = list_elfs(elf_dir);
        if (elfs.empty()) { std::fprintf(stderr, "no ELFs in %s\n", elf_dir.c_str()); return 2; }
        for (auto& e : elfs)
            for (Mode m : modes)
                for (unsigned s = 0; s < seeds; s++) {
                    RunConfig c;
                    c.name = basename_noext(e);
                    c.elf = e;
                    c.mode = m;
                    c.seed = seed_base + s;
                    c.rand_reset = rr(s);
                    cfgs.push_back(c);
                }
        if (label.empty()) label = "isa: " + std::to_string(elfs.size()) + " ELFs";
    } else if (suite == "random") {
        for (unsigned k = 0; k < count; k++)
            for (Mode m : modes) {
                RunConfig c;
                c.random = true;
                c.rand_seed = seed_base + k;
                c.rand_opt = ropt;
                c.name = "random-" + std::to_string(c.rand_seed);
                c.mode = m;
                c.seed = seed_base + k * 7919 + unsigned(m);
                c.rand_reset = rr(k);
                cfgs.push_back(c);
            }
        if (label.empty()) label = "random: " + std::to_string(count) + " programs (" + std::to_string(ropt.items) + " items)";
    } else {
        std::fprintf(stderr, "unknown suite %s\n", suite.c_str());
        return 2;
    }

    const auto t0 = std::chrono::steady_clock::now();
    uint64_t fails = 0;
    const Stats t = run_all(cfgs, jobs, fails);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::string ms, missing;
    for (Mode m : modes) ms += std::string(ms.empty() ? "" : ",") + MODE_NAMES[m];
    std::printf("%-6s %-44s modes=%s: %zu runs, %" PRIu64 " FAILED | %.2fM cycles, %.2fM retired, %.2fM lockstep-checked,"
                " %" PRIu64 " traps | branch miss %.2f%% jump miss %.2f%% | %.1fs\n",
                fails ? "FAIL" : "PASS", label.c_str(), ms.c_str(), cfgs.size(), fails, t.cycles / 1e6, t.retired / 1e6,
                t.checked / 1e6, t.traps, t.branches ? 100.0 * double(t.branch_miss) / double(t.branches) : 0.0,
                t.jumps ? 100.0 * double(t.jump_miss) / double(t.jumps) : 0.0, secs);
    for (int k = 0; k < NCOV; k++)
        if (t.cov[k] == 0) missing += std::string(missing.empty() ? "" : ", ") + COV_NAMES[k];
    print_cov(t.cov);
    if (req_cov) {
        if (!missing.empty()) {
            std::printf("FAIL   coverage: never hit: %s\n", missing.c_str());
            return 1;
        }
        std::printf("       coverage: all %d functional coverage points hit (min count %" PRIu64 ")\n", NCOV,
                    *std::min_element(t.cov, t.cov + NCOV));
    }
    return fails ? 1 : 0;
}
