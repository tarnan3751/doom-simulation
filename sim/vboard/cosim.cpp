#include "cosim.h"

#include <cstdio>
#include <cstring>

namespace vboard {

namespace {
enum Field : unsigned { F_TRAP = 1, F_PC = 2, F_INSN = 4, F_RD = 8, F_RDV = 16, F_MADDR = 32, F_MSTRB = 64, F_MDATA = 128, F_NPC = 256,
                        F_CYCLE = 512, F_COUNTERS = 1024 };

uint32_t lane_mask(uint32_t strb) {
    uint32_t m = 0;
    for (int i = 0; i < 4; i++)
        if (strb & (1u << i)) m |= 0xFFu << (8 * i);
    return m;
}

const char* const kCounterNames[] = {"cnt_cycle", "cnt_instret", "cnt_ic_access", "cnt_ic_miss", "cnt_dc_access", "cnt_dc_miss",
                                     "cnt_branch", "cnt_branch_miss", "cnt_jump", "cnt_jump_miss", "cnt_flip"};
const uint64_t* counter_array(const model::Counters& c) { return &c.cycle; }
bool same(const model::Counters& a, const model::Counters& b) { return memcmp(&a, &b, sizeof a) == 0; }
}  // namespace

Cosim::Cosim(uint32_t clk_hz, uint32_t sys_flags) : iss_([&] {
    iss::Config c;
    c.clk_hz = clk_hz;
    c.sys_flags = sys_flags;
    return c;
}()), ref_name_("ISS") {
    memset(hist_, 0, sizeof hist_);
    iss_.on_flip = [this]() {
        if (failed_) return;
        if (rtl_frames_.empty()) {
            fail("COSIM MISMATCH: the ISS presented a frame (VID_FLIP) that the RTL has not presented\n");
            return;
        }
        ref_frames_.push_back(frame(iss_.framebuffer(), iss_.palette(), 0));
        match_frames();
    };
}

Cosim::Cosim() : mdl_(new model::Machine), ref_name_("model") {
    memset(hist_, 0, sizeof hist_);
    mdl_->set_trace(true);
    mdl_->on_frame = [this](uint64_t cycle) { ref_frames_.push_back(frame(mdl_->iss().framebuffer(), mdl_->iss().palette(), cycle)); match_frames(); };
    mdl_->on_serial = [this](uint8_t c, uint64_t cycle) { ref_ser_.push_back({c, cycle}); match_serial(); };
}

simc::WriteFn Cosim::writer() {
    iss::Iss& m = mdl_ ? mdl_->iss() : iss_;
    return [&m](uint32_t a, const uint8_t* d, size_t n) { return m.write_mem(a, d, n); };
}

void Cosim::start(uint32_t entry) {
    if (mdl_) mdl_->reset(entry);
    else iss_.reset(entry);
}

void Cosim::key(bool pressed, uint8_t code, uint64_t cycle) {
    if (mdl_) mdl_->key(cycle + 1, uint16_t((pressed ? 0x100 : 0) | code));
    else iss_.push_key(pressed, code);
}

bool Cosim::needs_override(const RetireTrace& t) {
    if (t.trap) return false;
    uint32_t op = t.insn & 0x7F, f3 = (t.insn >> 12) & 7;
    if (op == 0x03) return (t.mem_addr >> 31) == 0;          // load from MMIO
    if (op == 0x73 && f3 != 0) {                              // Zicsr
        uint32_t csr = t.insn >> 20, rd = (t.insn >> 7) & 31;
        bool counter = csr == 0xC00 || csr == 0xC80 || csr == 0xC02 || csr == 0xC82 ||
                       csr == 0xB00 || csr == 0xB80 || csr == 0xB02 || csr == 0xB82;
        bool reads = !((f3 == 1 || f3 == 5) && rd == 0);      // csrrw/csrrwi with rd = x0 do not read
        return counter && reads;
    }
    return false;
}

bool Cosim::retire(const RetireTrace& t, uint64_t cycle, const model::Counters& cnt) {
    // After the exit store the RTL keeps retiring instructions (the ones behind it in the pipeline)
    // until `halted` is noticed; they are not part of the ISS's execution (the model executes them).
    if (!mdl_ && iss_.stop_reason() == iss::Stop::Exit) return true;
    Hist& h = hist_[count_ % kHist];
    h.t = t; h.cycle = cycle; h.index = count_;
    const bool inject = count_ == inject_;
    count_++;
    if (failed_) return false;
    iss::Retire r;
    uint64_t ref_cycle = 0;
    model::Counters ref{};
    unsigned d = 0;
    if (mdl_) {
        // the model must retire this instruction in this very cycle, with the same counters
        const uint64_t want = cycle + (inject ? 1 : 0);
        unsigned ev = 0;
        while (!(ev & (model::EvRetire | model::EvFault)) && mdl_->cycle() < want) ev |= mdl_->run(want);
        if (failed_) return false;                // a frame or serial byte differed on the way
        if (ev & model::EvFault) { fail_retire(t, r, cycle, 0, ("the model stopped: " + mdl_->fault()).c_str()); return false; }
        ref_cycle = mdl_->cycle();
        if (!(ev & model::EvRetire)) {
            fail_retire(t, r, cycle, F_CYCLE, "the RTL retired an instruction in this cycle, the model none by then", ref_cycle);
            return false;
        }
        r = mdl_->retired();
        ref = mdl_->counters();
        if (ref_cycle != want) d |= F_CYCLE;
        if (!same(ref, cnt)) d |= F_COUNTERS;
    } else {
        uint32_t v = t.rd_wdata;
        if (inject) iss_.set_pc(iss_.pc() + 4);
        r = iss_.step(needs_override(t) ? &v : nullptr);
        if (failed_) return false;                // a frame mismatch found by the ISS flip hook during step()
        if (!r.valid) {
            fail_retire(t, r, cycle, 0, iss_.stop_message().empty() ? "the ISS has stopped and cannot execute another instruction" : iss_.stop_message().c_str());
            return false;
        }
    }
    if (r.trap != t.trap) d |= F_TRAP;
    if (r.pc != t.pc) d |= F_PC;
    if (r.insn != t.insn) d |= F_INSN;
    if (r.rd != t.rd) d |= F_RD;
    if (r.rd_wdata != t.rd_wdata) d |= F_RDV;
    if (!t.trap && !r.trap && r.mem_addr != t.mem_addr) d |= F_MADDR;
    if (r.mem_wstrb != t.mem_wstrb) d |= F_MSTRB;
    if (((r.mem_wdata ^ t.mem_wdata) & lane_mask(t.mem_wstrb & r.mem_wstrb)) != 0) d |= F_MDATA;
    if (r.next_pc != t.next_pc) d |= F_NPC;
    if (d) { fail_retire(t, r, cycle, d, "retire records differ", ref_cycle, &cnt, &ref, inject); return false; }
    return true;
}

void Cosim::sync(const model::Counters& cnt, bool halted, uint32_t exit_code) {
    if (!mdl_ || failed_) return;
    unsigned ev = 0;
    while (!(ev & (model::EvRetire | model::EvFault)) && mdl_->cycle() < cnt.cycle) ev |= mdl_->run(cnt.cycle);
    if (failed_) return;
    char b[200];
    if (ev & (model::EvRetire | model::EvFault)) {
        snprintf(b, sizeof b, "COSIM MISMATCH: %s in cycle %llu, before the RTL's cycle %llu\n",
                 (ev & model::EvFault) ? ("the model stopped (" + mdl_->fault() + ")").c_str() : "the model retired an instruction the RTL did not retire",
                 (unsigned long long)mdl_->cycle(), (unsigned long long)cnt.cycle);
        fail(b);
        return;
    }
    const model::Counters ref = mdl_->counters();
    std::string o;
    if (!same(ref, cnt)) {
        snprintf(b, sizeof b, "COSIM MISMATCH: counters differ at the end of a slice, cycle %llu\n", (unsigned long long)cnt.cycle);
        o = b;
        for (int i = 0; i < 11; i++) {
            snprintf(b, sizeof b, "  %-16s RTL %-14llu model %-14llu %s\n", kCounterNames[i], (unsigned long long)counter_array(cnt)[i],
                     (unsigned long long)counter_array(ref)[i], counter_array(cnt)[i] != counter_array(ref)[i] ? "<-- DIFFERS" : "");
            o += b;
        }
    }
    if (halted != mdl_->halted() || (halted && exit_code != mdl_->exit_code())) {
        snprintf(b, sizeof b, "COSIM MISMATCH: cycle %llu: RTL halted %d exit code %u, model halted %d exit code %u\n", (unsigned long long)cnt.cycle,
                 halted, exit_code, mdl_->halted(), mdl_->exit_code());
        o += b;
    }
    if (!rtl_frames_.empty() || !ref_frames_.empty() || !rtl_ser_.empty() || !ref_ser_.empty()) {
        snprintf(b, sizeof b, "COSIM MISMATCH: by cycle %llu the RTL presented %zu frames / sent %zu bytes the model did not, the model %zu / %zu the RTL did not\n",
                 (unsigned long long)cnt.cycle, rtl_frames_.size(), rtl_ser_.size(), ref_frames_.size(), ref_ser_.size());
        o += b;
    }
    if (!o.empty()) fail(o);
}

void Cosim::fail(const std::string& what) {
    failed_ = true;
    report_ = what;
    append_history(report_);
}

void Cosim::fail_retire(const RetireTrace& t, const iss::Retire& r, uint64_t cycle, unsigned d, const char* why,
                        uint64_t ref_cycle, const model::Counters* rtl, const model::Counters* ref, bool injected) {
    failed_ = true;
    char b[512];
    std::string& o = report_;
    snprintf(b, sizeof b, "COSIM MISMATCH: %s\n  RTL cycle %llu, instruction #%llu (0-based, in program order)\n", why,
             (unsigned long long)cycle, (unsigned long long)(count_ - 1));
    o += b;
    auto row = [&](const char* name, unsigned bit, uint64_t rtl_v, uint64_t ref_v, const char* fmt) {
        char x[160], y[48], z[48];
        snprintf(y, sizeof y, fmt, (unsigned long long)rtl_v);
        snprintf(z, sizeof z, fmt, (unsigned long long)ref_v);
        snprintf(x, sizeof x, "  %-10s RTL %-12s %s %-12s %s\n", name, y, ref_name_, z, (d & bit) ? "<-- DIFFERS" : "");
        o += x;
    };
    snprintf(b, sizeof b, "  field      RTL value          %s value\n", ref_name_);
    o += b;
    if (mdl_) row(injected ? "cycle (+1)" : "cycle", F_CYCLE, cycle + injected, ref_cycle, "%llu");
    row("trap", F_TRAP, t.trap, r.trap, "%llu");
    row("pc", F_PC, t.pc, r.pc, "0x%08llx");
    row("insn", F_INSN, t.insn, r.insn, "0x%08llx");
    row("rd", F_RD, t.rd, r.rd, "x%llu");
    row("rd value", F_RDV, t.rd_wdata, r.rd_wdata, "0x%08llx");
    row("mem addr", F_MADDR, t.mem_addr, r.mem_addr, "0x%08llx");
    row("st strobes", F_MSTRB, t.mem_wstrb, r.mem_wstrb, "0x%llx");
    row("st data", F_MDATA, t.mem_wdata, r.mem_wdata, "0x%08llx");
    row("next pc", F_NPC, t.next_pc, r.next_pc, "0x%08llx");
    if (rtl && ref && (d & F_COUNTERS))
        for (int i = 0; i < 11; i++) {
            const uint64_t x = counter_array(*rtl)[i], y = counter_array(*ref)[i];
            snprintf(b, sizeof b, "  %-16s RTL %-14llu %s %-14llu %s\n", kCounterNames[i], (unsigned long long)x, ref_name_, (unsigned long long)y,
                     x != y ? "<-- DIFFERS" : "");
            o += b;
        }
    if (r.valid && r.trap) { snprintf(b, sizeof b, "  (%s trap cause %u)\n", ref_name_, r.cause); o += b; }
    append_history(o);
}

void Cosim::append_history(std::string& out) const {
    out += "  last retired instructions (RTL trace, oldest first; the last line is the mismatching one):\n";
    uint64_t n = count_ < kHist ? count_ : kHist;
    for (uint64_t i = count_ - n; i < count_; i++) {
        const Hist& h = hist_[i % kHist];
        char b[200];
        int k = snprintf(b, sizeof b, "   #%-9llu cyc %-11llu pc %08x  insn %08x", (unsigned long long)h.index,
                         (unsigned long long)h.cycle, h.t.pc, h.t.insn);
        if (h.t.trap) k += snprintf(b + k, sizeof b - k, "  TRAP -> %08x", h.t.next_pc);
        else {
            if (h.t.rd) k += snprintf(b + k, sizeof b - k, "  x%u=%08x", h.t.rd, h.t.rd_wdata);
            if (h.t.mem_wstrb) k += snprintf(b + k, sizeof b - k, "  st[%08x] strb=%x data=%08x", h.t.mem_addr, h.t.mem_wstrb, h.t.mem_wdata);
            else if (h.t.mem_addr) k += snprintf(b + k, sizeof b - k, "  mem[%08x]", h.t.mem_addr);
            k += snprintf(b + k, sizeof b - k, "  -> %08x", h.t.next_pc);
        }
        out += b;
        out += '\n';
    }
}

Cosim::Frame Cosim::frame(const uint8_t* fb, const uint32_t* pal, uint64_t cycle) {
    Frame p;
    p.fb.assign(fb, fb + kFbBytes);
    p.pal.resize(256);
    for (int i = 0; i < 256; i++) p.pal[i] = pal[i] & 0xFFFFFFu;
    p.cycle = cycle;
    return p;
}

void Cosim::rtl_flip(const uint8_t* fb, const uint32_t* pal, uint64_t cycle) {
    rtl_frames_.push_back(frame(fb, pal, mdl_ ? cycle : 0));
    if (mdl_) match_frames();
}

// Compare presented frames in order: RGB after palette lookup (and, against the model, the cycle).
void Cosim::match_frames() {
    while (!failed_ && !rtl_frames_.empty() && !ref_frames_.empty()) {
        const Frame& p = rtl_frames_.front();
        const Frame& q = ref_frames_.front();
        unsigned bad = 0, first = 0;
        for (unsigned i = 0; i < kFbBytes; i++)
            if (p.pal[p.fb[i]] != q.pal[q.fb[i]]) { if (!bad) first = i; bad++; }
        if (bad || p.cycle != q.cycle) {
            char b[400];
            int k = snprintf(b, sizeof b, "COSIM MISMATCH: frame #%llu differs between RTL and %s: %u pixels (RGB after palette lookup); first at x=%u y=%u: RTL %06x, %s %06x",
                             (unsigned long long)(frames_ok_ + 1), ref_name_, bad, first % kFbWidth, first / kFbWidth, p.pal[p.fb[first]], ref_name_,
                             q.pal[q.fb[first]]);
            if (mdl_) snprintf(b + k, sizeof b - k, "; presented in cycle %llu (RTL), %llu (model)", (unsigned long long)p.cycle, (unsigned long long)q.cycle);
            fail(std::string(b) + "\n");
            return;
        }
        frames_ok_++;
        rtl_frames_.pop_front();
        ref_frames_.pop_front();
    }
}

void Cosim::rtl_serial(uint8_t c, uint64_t cycle) {
    if (!mdl_) return;
    rtl_ser_.push_back({c, cycle});
    match_serial();
}

void Cosim::match_serial() {
    while (!failed_ && !rtl_ser_.empty() && !ref_ser_.empty()) {
        if (rtl_ser_.front() != ref_ser_.front()) {
            char b[200];
            snprintf(b, sizeof b, "COSIM MISMATCH: serial byte 0x%02x decoded in cycle %llu (RTL), 0x%02x in cycle %llu (model)\n", rtl_ser_.front().first,
                     (unsigned long long)rtl_ser_.front().second, ref_ser_.front().first, (unsigned long long)ref_ser_.front().second);
            fail(b);
            return;
        }
        rtl_ser_.pop_front();
        ref_ser_.pop_front();
    }
}

}  // namespace vboard
