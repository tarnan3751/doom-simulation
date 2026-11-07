// iss-run - headless runner for the ISS (no Verilator or SDL needed).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "dfrm.h"
#include "iss.h"
#include "loader.h"

namespace {

constexpr int kExitTimeout = 124, kExitFault = 125, kExitSetup = 126, kExitUsage = 2;

void usage(FILE* f) {
    fputs(
        "usage: iss-run [options] program.elf [-- arg ...]\n"
        "  --wad FILE          load a WAD at 0x80800000 and describe it in the boot-info page\n"
        "  --max-insns N       stop after N instructions (exit status 124)\n"
        "  --dump-frames FILE  write every presented frame (VID_FLIP) to a DFRM file\n"
        "  --trace FILE        write a text retire trace (slow)\n"
        "  --isa-test          riscv-tests mode: stop on a store to 'tohost'; exit 0 iff value == 1\n"
        "  --stats             print instruction count, wall time and MIPS on stderr\n"
        "exit status: low byte of SYS_EXIT; 124 max-insns; 125 bad fetch/access; 126 setup error; 2 usage\n",
        f);
}

std::string g_out;
void flush_out() {
    fwrite(g_out.data(), 1, g_out.size(), stdout);
    g_out.clear();
    fflush(stdout);
}
int setup_error(const std::string& e) { fprintf(stderr, "iss-run: %s\n", e.c_str()); return kExitSetup; }

}  // namespace

int main(int argc, char** argv) {
    std::string elf_path, wad_path, frames_path, trace_path, err;
    std::vector<std::string> args;
    uint64_t max_insns = ~0ull;
    bool isa_test = false, stats = false;
    int i = 1;
    for (; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&](const char* opt) -> const char* {
            if (i + 1 >= argc) { fprintf(stderr, "iss-run: %s needs an argument\n", opt); exit(kExitUsage); }
            return argv[++i];
        };
        if (a == "--") { i++; break; }
        else if (a == "--wad") wad_path = need("--wad");
        else if (a == "--max-insns") max_insns = strtoull(need("--max-insns"), nullptr, 0);
        else if (a == "--dump-frames") frames_path = need("--dump-frames");
        else if (a == "--trace") trace_path = need("--trace");
        else if (a == "--isa-test") isa_test = true;
        else if (a == "--stats") stats = true;
        else if (a == "-h" || a == "--help") { usage(stdout); return 0; }
        else if (a.size() > 1 && a[0] == '-') { fprintf(stderr, "iss-run: unknown option '%s'\n", a.c_str()); usage(stderr); return kExitUsage; }
        else if (elf_path.empty()) elf_path = a;
        else { fprintf(stderr, "iss-run: unexpected argument '%s' (program arguments go after --)\n", a.c_str()); return kExitUsage; }
    }
    for (; i < argc; i++) args.push_back(argv[i]);
    if (elf_path.empty()) { usage(stderr); return kExitUsage; }

    iss::Iss m;
    uint32_t entry = 0, tohost = 0;
    if (!simc::load_program([&](uint32_t a, const uint8_t* d, size_t n) { return m.write_mem(a, d, n); }, elf_path, wad_path,
                            args, isa_test, entry, tohost, err))
        return setup_error(err);
    m.reset(entry);
    if (isa_test) m.set_store_watch(tohost);
    m.on_uart = [](uint8_t c) {
        g_out.push_back(char(c));
        if (c == '\n' || g_out.size() > 4096) flush_out();
    };
    simc::DfrmWriter frames;
    if (!frames_path.empty()) {
        if (!frames.open(frames_path, err)) return setup_error(err);
        m.on_flip = [&]() { frames.write_frame_xrgb(m.palette(), m.framebuffer()); };
    }
    FILE* tf = nullptr;
    if (!trace_path.empty() && !(tf = fopen(trace_path.c_str(), "w"))) return setup_error("cannot create '" + trace_path + "'");

    auto t0 = std::chrono::steady_clock::now();
    uint64_t executed = 0;
    while (m.stop_reason() == iss::Stop::None && executed < max_insns) {
        uint64_t want = std::min<uint64_t>(1u << 20, max_insns - executed);
        if (!tf) executed += m.run(want);
        for (uint64_t k = 0; tf && k < want; k++) {
            iss::Retire r = m.step();
            if (!r.valid) break;
            executed++;
            fprintf(tf, "%08x %08x", r.pc, r.insn);
            if (r.trap) fprintf(tf, " TRAP cause=%u", r.cause);
            if (r.rd) fprintf(tf, " x%u=%08x", r.rd, r.rd_wdata);
            if (r.mem_wstrb) fprintf(tf, " st [%08x] strb=%x data=%08x", r.mem_addr, r.mem_wstrb, r.mem_wdata);
            else if (r.is_load) fprintf(tf, " ld [%08x]", r.mem_addr);
            fprintf(tf, " -> %08x\n", r.next_pc);
        }
        flush_out();
    }
    flush_out();
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (tf) fclose(tf);
    frames.close();
    if (stats)
        fprintf(stderr, "iss-run: %llu instructions, %.3f s, %.1f MIPS, %llu frames\n", (unsigned long long)executed, secs,
                secs > 0 ? executed / secs / 1e6 : 0.0, (unsigned long long)frames.frames());
    switch (m.stop_reason()) {
    case iss::Stop::Exit: return int(m.exit_code() & 0xFF);
    case iss::Stop::Watch:
        if (m.watch_value() == 1) return 0;
        fprintf(stderr, "iss-run: %s: FAILED test %u (tohost=0x%x) at pc=0x%08x\n", elf_path.c_str(), m.watch_value() >> 1, m.watch_value(), m.pc());
        return 1;
    case iss::Stop::BadFetch:
    case iss::Stop::BadAccess: fprintf(stderr, "iss-run: fault: %s\n", m.stop_message().c_str()); return kExitFault;
    default:
        fprintf(stderr, "iss-run: stopped after %llu instructions (--max-insns) at pc=0x%08x\n", (unsigned long long)executed, m.pc());
        return kExitTimeout;
    }
}
