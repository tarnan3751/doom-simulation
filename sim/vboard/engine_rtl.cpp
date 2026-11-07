// engine_rtl.cpp - the Verilator engines. Compiled once per verilated model (Makefile): VB_MODEL = Vsoc_fast
// (soc_top FAST=1: ideal memory) or Vsoc_acc (FAST=0: SDRAM pins against the chip model, real scan-out,
// serial from the uart_tx pin), VB_ACC = 0/1, VB_UART_DIV; the model headers come in with -include.
// cycle<>() is the only code that runs billions of times: the clock edge, the device-model ticks of the
// accurate engine and a few byte-wide pin reads. No virtual or std::function calls, no allocation; the
// trace_* pins are looked at only when --cosim or --isa-test asked for them (template parameter TRACE).
#include <verilated_syms.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "cosim.h"
#include "engine.h"
#include "sdram_model.h"
#include "uart_decoder.h"
#include "vb_model_check.h"   // VB_MODEL_CHECKED, written by vl_check.py next to the generated model
#include "video_monitor.h"

// Two shortcuts past Verilator's public eval() rely on internals of the generated code. They are used only
// with Verilator 5.054 and when vl_check.py has proved, on this model's generated code, the facts they need;
// otherwise the engine falls back to the portable two-eval() loop (slower, always right):
//   VB_SINGLE_EVAL  one eval per cycle: clk stays 1, the "previous clk" trigger memory is reset by hand.
//   VB_DIRECT_EVAL  call the model's scheduling-region functions directly (see eval_step()).
#if VERILATOR_VERSION_INTEGER == 5054000 && VB_MODEL_CHECKED
#ifndef VB_SINGLE_EVAL
#define VB_SINGLE_EVAL 1
#endif
#ifndef VB_DIRECT_EVAL
#define VB_DIRECT_EVAL VB_SINGLE_EVAL
#endif
#else
#warning "engine_rtl.cpp: Verilator is not 5.054 or vl_check.py could not verify the model (see vb_model_check.h); portable eval() loop"
#undef VB_SINGLE_EVAL
#undef VB_DIRECT_EVAL
#define VB_SINGLE_EVAL 0
#define VB_DIRECT_EVAL 0
#endif
#define VB_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define VB_ALWAYS_INLINE inline __attribute__((always_inline))
#define VB_CAT_(a, b) a##b
#define VB_CAT(a, b) VB_CAT_(a, b)
#define VB_REGION(r) VB_CAT(VB_MODEL, ___024root___eval_##r)   // the generated scheduling-region functions

using VbRoot = VB_CAT(VB_MODEL, ___024root);
void VB_REGION(sample)(VbRoot*), VB_REGION(postponed)(VbRoot*);
bool VB_REGION(ico)(VbRoot*, CData), VB_REGION(act)(VbRoot*), VB_REGION(inact)(VbRoot*), VB_REGION(nba)(VbRoot*),
    VB_REGION(obs)(VbRoot*), VB_REGION(react)(VbRoot*);

namespace vboard {
namespace {

using Model = VB_MODEL;
constexpr bool ACC = VB_ACC;
constexpr uint32_t kRamBase = 0x80000000u;

class RtlEngine final : public Engine {
public:
    explicit RtlEngine(const EngineOptions& o) : opt_(o), uart_(VB_UART_DIV) {
        info_.name = ACC ? "accurate" : "fast";
        info_.description = ACC ? "RTL, SDRAM + video pins" : "RTL, ideal memory (FAST=1)";
        info_.clk_hz = 50000000;
        info_.speed_unit = SpeedUnit::Mhz;
    }

    bool init(std::string& err) {
        ctx_.reset(new VerilatedContext);
        ctx_->traceEverOn(false);
        top_.reset(new Model(ctx_.get()));
        for (int b = 0; b < 4; b++) fb_[b] = static_cast<const uint8_t*>(find_var("u_video", ("fb" + std::to_string(b)).c_str()));
        pal_ = static_cast<const uint32_t*>(find_var("u_video", "palette"));
        if (!fb_[0] || !fb_[1] || !fb_[2] || !fb_[3] || !pal_) { err = "cannot find the framebuffer/palette arrays in the model (scope table)"; return false; }
        if constexpr (ACC) {
            sdram_.reset(new SdramModel(13, 8, SdramTiming::from_ns(info_.clk_hz)));
            sdram_->set_log_limit(32);
            ram_size_ = sdram_->size_bytes();
            uart_.on_byte = [this](uint8_t c) {
                if (cosim_) cosim_->rtl_serial(c, top_->cnt_cycle);
                if (on_serial) on_serial(c);
            };
            black_.assign(kMonWidth * kMonHeight, 0);
            snap_[0].resize(kSnapBytes); snap_[1].resize(kSnapBytes);
            predicted_.resize(kMonWidth * kMonHeight);
            idx_.resize(kFbBytes);
        } else {
            ram_ = static_cast<uint8_t*>(find_var("u_ideal", "mem"));
            if (!ram_) { err = "cannot find the mem_ideal array in the model (scope table)"; return false; }
            ram_size_ = size_t(1) << 25;
        }
        if (opt_.cosim_model && !ACC) { err = "--cosim=model checks the model against the accurate engine (--engine accurate)"; return false; }
        if (opt_.cosim) { cosim_.reset(opt_.cosim_model ? new Cosim() : new Cosim(info_.clk_hz, ACC ? 0u : 1u)); cosim_->inject_at(opt_.cosim_inject); }
        return true;
    }

    const EngineInfo& info() const override { return info_; }

    simc::WriteFn mem_writer() override {
        return [this](uint32_t a, const uint8_t* d, size_t n) {
            if (a < kRamBase || size_t(a - kRamBase) + n > ram_size_) return false;
            memcpy((ACC ? sdram_->bytes() : ram_) + (a - kRamBase), d, n);
            return !cosim_ || cosim_->writer()(a, d, n);
        };
    }

    void watch_tohost(uint32_t addr) override { tohost_ = addr & ~3u; watch_ = true; }

    bool start(uint32_t entry, std::string& err) override {
        if (entry != kRamBase) { err = "RTL engines start at 0x80000000 (ELF entry is not)"; return false; }
        Model& t = *top_;
        t.clk = 0; t.rst = 1; t.key_valid = 0; t.key_data = 0; t.sdram_dq_i = 0;
        t.eval();
        for (int i = 0; i < 5; i++) {      // reset cycles: device models see them, the checkers do not
            uint32_t dq = 0;
            if constexpr (ACC) dq = sdram_->tick(pins());
            t.clk = 1; t.eval();
            if constexpr (ACC) t.sdram_dq_i = dq;
            low_phase();
        }
        t.rst = 0;
        if (cosim_) cosim_->start(entry);
        return true;
    }

    RunResult run(uint64_t max_cycles) override {
        if (st_.state != RunState::Running) return RunResult();
        return cosim_ || watch_ ? run_impl<true>(max_cycles) : run_impl<false>(max_cycles);
    }

    const Status& status() const override { return st_; }

    void push_key(bool pressed, uint8_t code) override {
        if (kn_ >= kKq) return;
        kq_[(kh_ + kn_++) % kKq] = uint16_t((pressed ? 0x100 : 0) | code);
        key_busy_ = true;
    }

    model::Counters counters() const {
        const Model& t = *top_;
        return model::Counters{t.cnt_cycle, t.cnt_instret, t.cnt_ic_access, t.cnt_ic_miss, t.cnt_dc_access, t.cnt_dc_miss,
                               t.cnt_branch, t.cnt_branch_miss, t.cnt_jump, t.cnt_jump_miss, t.cnt_flip};
    }

    Stats stats() override {
        Stats s;
        Model& t = *top_;
        s.cycles.set(t.cnt_cycle);        s.instret.set(t.cnt_instret);
        s.ic_access.set(t.cnt_ic_access); s.ic_miss.set(t.cnt_ic_miss);
        s.dc_access.set(t.cnt_dc_access); s.dc_miss.set(t.cnt_dc_miss);
        s.branch.set(t.cnt_branch);       s.branch_miss.set(t.cnt_branch_miss);
        s.jump.set(t.cnt_jump);           s.jump_miss.set(t.cnt_jump_miss);
        s.flips.set(t.cnt_flip > frames_presented() ? t.cnt_flip : frames_presented());   // cnt_flip steps one cycle after the vid_flip pulse
        if constexpr (ACC) {
            s.sdram_violations.set(sdram_->violations());
            s.serial_errors.set(uart_.framing_errors() + uart_.timing_errors());
            s.video_errors.set(mon_.errors());
            s.scan_checked.set(scan_checked_); s.scan_skipped.set(scan_skipped_); s.scan_bad.set(scan_bad_);
            const SdramModel::Stats& m = sdram_->stats();
            s.extras = {{"sdram activates", m.act}, {"sdram reads", m.read}, {"sdram writes", m.write},
                        {"sdram refreshes", m.refresh}, {"monitor frames", mon_.frames()}};
        }
        if (cosim_) {
            s.extras.push_back({"cosim instructions compared", cosim_->compared()});
            s.extras.push_back({"cosim frames compared", cosim_->frames_compared()});
        }
        return s;
    }

    const uint32_t* monitor_image() override {
        if constexpr (ACC) return mon_.frames() ? mon_.frame().data() : black_.data();
        return Engine::monitor_image();
    }
    uint64_t monitor_serial() const override { return ACC ? mon_.frames() : Engine::monitor_serial(); }

    std::string diagnostics() override {
        std::string o;
        if (cosim_ && cosim_->failed()) o += cosim_->report();
        if constexpr (ACC) {
            if (sdram_->violations()) {
                o += "SDRAM protocol/timing violations (" + std::to_string(sdram_->violations()) + "):\n";
                for (const auto& l : sdram_->violation_log()) o += "  " + l + "\n";
            }
            if (uart_.framing_errors() || uart_.timing_errors())
                o += "serial decoder: " + std::to_string(uart_.framing_errors()) + " framing errors, " + std::to_string(uart_.timing_errors()) + " bit-timing errors\n";
            if (mon_.errors()) {
                o += "video monitor: " + std::to_string(mon_.errors()) + " timing deviations:\n";
                for (const auto& l : mon_.error_log()) o += "  " + l + "\n";
            }
            o += scan_msg_;
        }
        return o;
    }

    uint64_t cosim_compared() const override { return cosim_ ? cosim_->compared() : 0; }

private:
    static constexpr size_t kSnapBytes = 4 * 16000 + 256 * 4;   // the four framebuffer lanes, then the palette
    static constexpr unsigned kKq = 256;

    void* find_var(const char* scope_suffix, const char* var) {
        size_t sl = strlen(scope_suffix);
        for (const auto& kv : *ctx_->scopeNameMap()) {
            std::string name(kv.first);
            if (name.size() >= sl && name.compare(name.size() - sl, sl, scope_suffix) == 0 &&
                (name.size() == sl || name[name.size() - sl - 1] == '.'))
                if (const VerilatedVar* v = kv.second->varFind(var)) return v->datap();
        }
        return nullptr;
    }

    SdramPins pins() {
        Model& t = *top_;
        return SdramPins{bool(t.sdram_cke), bool(t.sdram_cs_n), bool(t.sdram_ras_n), bool(t.sdram_cas_n), bool(t.sdram_we_n),
                         t.sdram_ba, uint16_t(t.sdram_addr), t.sdram_dqm, t.sdram_dq_o, bool(t.sdram_dq_oe)};
    }

    // Framebuffer lanes + palette words -> row-major indices + 0x00RRGGBB palette.
    static void unpack(const uint8_t* const lanes[4], const uint32_t* pal_words, uint8_t* fb, uint32_t* pal) {
        gather_fb(lanes, fb);
        for (int i = 0; i < 256; i++) pal[i] = pal_words[i] & 0xFFFFFFu;
    }

    // ---- scan-out self check (accurate) ----
    void scan_edge() {                               // vsync fell on the pins: a frame boundary
        std::swap(snap_[0], snap_[1]);               // [0] = previous boundary, [1] = this one
        for (int b = 0; b < 4; b++) memcpy(&snap_[1][size_t(b) * 16000], fb_[b], 16000);
        memcpy(&snap_[1][64000], pal_, 1024);
        snaps_taken_++;
    }
    void scan_frame_published() {                    // the monitor completed a frame between the last two boundaries
        if (!opt_.check_scanout) return;
        if (snaps_taken_ < 2 || memcmp(snap_[0].data(), snap_[1].data(), kSnapBytes) != 0) { scan_skipped_++; return; }
        const uint8_t* lanes[4] = {&snap_[1][0], &snap_[1][16000], &snap_[1][32000], &snap_[1][48000]};
        uint32_t pal[256];
        unpack(lanes, reinterpret_cast<const uint32_t*>(&snap_[1][64000]), idx_.data(), pal);
        synth_monitor(idx_.data(), pal, predicted_.data());
        if (opt_.scan_inject) predicted_[1000] ^= 0x10;
        const std::vector<uint32_t>& got = mon_.frame();
        unsigned bad = 0, first = 0;
        for (unsigned i = 0; i < kMonWidth * kMonHeight; i++)
            if ((got[i] & 0xFFFFFFu) != predicted_[i]) { if (!bad) first = i; bad++; }
        if (!bad) { scan_checked_++; return; }
        scan_bad_++;
        if (scan_msg_.size() < 1500) {
            char b[260];
            snprintf(b, sizeof b, "scan-out mismatch in monitor frame #%llu (static framebuffer+palette): %u of %u pixels differ; first at x=%u y=%u: monitor %06x, expected %06x\n",
                     (unsigned long long)mon_.frames(), bad, kMonWidth * kMonHeight, first % kMonWidth, first / kMonWidth, got[first] & 0xFFFFFFu, predicted_[first]);
            scan_msg_ += b;
        }
    }

    void drive_key() {
        Model& t = *top_;
        t.key_valid = kn_ != 0;
        if (!kn_) { key_busy_ = false; return; }
        uint16_t e = kq_[kh_];
        kh_ = (kh_ + 1) % kKq; kn_--;
        t.key_data = e;
        if (cosim_) cosim_->key(e & 0x100, uint8_t(e), t.cnt_cycle);
    }

    // One rising clock edge (clk = 1), inputs as currently set.
    // VB_DIRECT_EVAL: the time step of VerilatedEvalLoop::evalImpl<false> (verilated.cpp, 5.054) with the
    // same region functions in the same nested-loop order, called directly instead of through virtual calls,
    // and without evalBegin/evalEnd: VlDeleter::deleteAll() frees SystemVerilog class objects (none here) and
    // Verilated::endOfEval() drains the cross-thread message queue (only mtasks of a --threads model post to
    // it). The convergence limit becomes a hard stop. The input-combinational (ico) region runs only when an
    // input other than clk differs from what the region last saw (its own trigger memory): vl_check.py has
    // proved that nothing in it would run otherwise.
    VB_ALWAYS_INLINE void eval_step() {
#if VB_DIRECT_EVAL
        VbRoot* r = top_->rootp;
        unsigned n = 0;
        VB_REGION(sample)(r);
        const auto& in = r->__VprimaryPorts;
        if (VB_UNLIKELY((in.rst != r->__Vtrigprevexpr___TOP__rst__0) | (in.key_valid != r->__Vtrigprevexpr___TOP__key_valid__0) |
                        (in.key_data != r->__Vtrigprevexpr___TOP__key_data__0) | (in.sdram_dq_i != r->__Vtrigprevexpr___TOP__sdram_dq_i__0)))
            for (bool first = true; VB_REGION(ico)(r, first); first = false) converge(n);
        do {
            do {
                do {
                    do {
                        while (VB_REGION(act)(r)) converge(n);
                    } while (VB_REGION(inact)(r));
                } while (VB_REGION(nba)(r) && converge(n));
            } while (VB_REGION(obs)(r));
        } while (VB_REGION(react)(r));
        VB_REGION(postponed)(r);
#else
        top_->eval();
#endif
    }
    static VB_ALWAYS_INLINE bool converge(unsigned& n) {
        if (VB_UNLIKELY(++n > 10000)) { fprintf(stderr, "vboard: verilated model did not converge\n"); abort(); }
        return true;
    }

    // The clock's low phase. All logic is posedge-clocked and no output depends combinationally on clk, so the
    // second eval() per cycle (clk=0) only serves to let Verilator see the next rising edge. With
    // VB_SINGLE_EVAL it is skipped: clk stays 1 and Verilator's "previous clk" trigger memory is reset by hand.
    VB_ALWAYS_INLINE void low_phase() {
#if VB_SINGLE_EVAL
        top_->rootp->__Vtrigprevexpr___TOP__clk__0 = 0;
        top_->rootp->__Vtrigprevexpr___TOP__clk__1 = 0;
#else
        top_->clk = 0; top_->eval();
#endif
    }

    template <bool TRACE>
    VB_ALWAYS_INLINE unsigned cycle() {
        Model& t = *top_;
        uint32_t dq_next = 0;
        if constexpr (ACC) dq_next = sdram_->tick(pins());
        if (VB_UNLIKELY(key_busy_)) drive_key();
        t.clk = 1; eval_step();
        if constexpr (ACC) t.sdram_dq_i = dq_next;
        low_phase();
        unsigned ev = 0;
        if constexpr (ACC) {
            uart_.tick(t.uart_tx);
            bool vs = t.vid_vsync;
            mon_.tick(t.vid_pix_ce, t.vid_hsync, vs, t.vid_de, t.vid_r, t.vid_g, t.vid_b);
            if (VB_UNLIKELY(!vs && prev_vs_)) scan_edge();
            prev_vs_ = vs;
            if (VB_UNLIKELY(mon_.frames() != mon_frames_)) { mon_frames_ = mon_.frames(); scan_frame_published(); }
        } else {
            if (t.uart_tx_valid && on_serial) on_serial(t.uart_tx_byte);
        }
        if (VB_UNLIKELY(t.vid_flip)) ev |= EvFrame;
        if (VB_UNLIKELY(t.halted)) ev |= EvHalted;
        if constexpr (TRACE) {
            if (t.trace_valid) ev |= on_trace();
        }
        return ev;
    }

    unsigned on_trace() {
        Model& t = *top_;
        RetireTrace tr{bool(t.trace_trap), t.trace_pc, t.trace_insn, t.trace_rd, t.trace_rd_wdata,
                       t.trace_mem_addr, t.trace_mem_wstrb, t.trace_mem_wdata, t.trace_next_pc};
        if (cosim_ && (!cosim_->retire(tr, t.cnt_cycle, counters()) || cosim_->failed())) return EvCosim;
        if (watch_ && !tr.trap && tr.mem_wstrb && (tr.mem_addr & ~3u) == tohost_) {
            st_.tohost = tr.mem_wdata;
            return EvTohost;
        }
        return 0;
    }

    void do_frame() {
        uint8_t fb[kFbBytes];
        uint32_t pal[256];
        unpack(fb_, pal_, fb, pal);
        if (cosim_) cosim_->rtl_flip(fb, pal, top_->cnt_cycle);
        present_frame(fb, pal, top_->cnt_cycle);
    }

    bool hw_error() const {
        return ACC && (sdram_->violations() || uart_.framing_errors() || uart_.timing_errors() || mon_.errors() || scan_bad_);
    }

    template <bool TRACE>
    RunResult run_impl(uint64_t max_cycles) {
        RunResult r;
        unsigned ev = 0;
        uint64_t i = 0;
        for (; i < max_cycles; i++) {
            ev = cycle<TRACE>();
            if (VB_UNLIKELY(ev)) { i++; break; }
        }
        r.cycles = i;
        if (ev & EvFrame) do_frame();
        if (ev & EvHalted) {
            // let in-flight work finish: the UART frame still on the pin, trace entries still in the pipeline
            uint64_t drain = (ACC ? 12ull * VB_UART_DIV + 64 : 0) + (TRACE ? 64 : 0);
            for (uint64_t k = 0; k < drain; k++) {
                unsigned e2 = cycle<TRACE>();
                r.cycles++;
                if (e2 & EvFrame) do_frame();
                ev |= e2 & (EvCosim | EvTohost | EvFrame);
                if (e2 & (EvCosim | EvTohost)) break;
            }
            st_.exit_code = top_->exit_code;
        }
        if (cosim_) cosim_->sync(counters(), top_->halted, top_->exit_code);
        if (cosim_ && cosim_->failed()) ev = (ev & ~(EvHalted | EvTohost)) | EvCosim;
        if (ev & EvCosim) { st_.state = RunState::CosimMismatch; st_.message = "lockstep mismatch against the ISS"; ev &= ~(EvHalted | EvTohost); }
        else if (ev & EvTohost) { st_.state = RunState::Tohost; ev &= ~EvHalted; }
        else if (ev & EvHalted) st_.state = RunState::Halted;
        if (!(ev & EvCosim) && !opt_.keep_going && hw_error()) {
            st_.state = RunState::HwError;
            st_.message = "hardware checker error (see diagnostics)";
            ev |= EvCheck;
        }
        st_.pc = 0;
        r.events = ev;
        return r;
    }

    EngineOptions opt_;
    EngineInfo info_;
    Status st_;
    std::unique_ptr<VerilatedContext> ctx_;
    std::unique_ptr<Model> top_;
    const uint8_t* fb_[4] = {};
    const uint32_t* pal_ = nullptr;
    uint8_t* ram_ = nullptr;
    size_t ram_size_ = 0;
    uint32_t tohost_ = 0;
    bool watch_ = false;
    std::unique_ptr<Cosim> cosim_;
    // keyboard queue
    uint16_t kq_[kKq];
    unsigned kh_ = 0, kn_ = 0;
    bool key_busy_ = false;
    // accurate-engine devices
    std::unique_ptr<SdramModel> sdram_;
    UartDecoder uart_;
    VideoMonitor mon_;
    std::vector<uint32_t> black_, predicted_;
    std::vector<uint8_t> snap_[2], idx_;
    bool prev_vs_ = true;
    uint64_t mon_frames_ = 0, snaps_taken_ = 0, scan_checked_ = 0, scan_skipped_ = 0, scan_bad_ = 0;
    std::string scan_msg_;
};

}  // namespace

std::unique_ptr<Engine> VB_CAT(make_, VB_CAT(VB_ENGINE, _engine))(const EngineOptions& opt, std::string& err) {
    auto e = std::make_unique<RtlEngine>(opt);
    if (!e->init(err)) return nullptr;
    return e;
}

}  // namespace vboard
