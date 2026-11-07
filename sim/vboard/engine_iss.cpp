// engine_iss.cpp - Engine implementation on top of the instruction-set simulator.
#include "engine.h"
#include "iss.h"

double sc_time_stamp() { return 0; }   // required by the Verilator runtime (linked into every vboard binary)

namespace vboard {

void synth_monitor(const uint8_t* fb, const uint32_t* pal, uint32_t* out) {
    // Active lines 0..39 and 440..479 black; 40..439 show fb rows 0..199 doubled in both axes.
    memset(out, 0, sizeof(uint32_t) * kMonWidth * 40);
    for (unsigned y = 0; y < kFbHeight; y++) {
        uint32_t* row = out + size_t(40 + 2 * y) * kMonWidth;
        const uint8_t* src = fb + size_t(y) * kFbWidth;
        for (unsigned x = 0; x < kFbWidth; x++) {
            uint32_t c = pal[src[x]] & 0xFFFFFFu;
            row[2 * x] = row[2 * x + 1] = c;
        }
        memcpy(row + kMonWidth, row, sizeof(uint32_t) * kMonWidth);
    }
    memset(out + size_t(440) * kMonWidth, 0, sizeof(uint32_t) * kMonWidth * 40);
}

namespace {

class IssEngine : public Engine {
public:
    IssEngine() {
        info_.name = "iss";
        info_.description = "instruction-set simulator (1 cycle per instruction)";
        info_.clk_hz = 50000000;
        info_.speed_unit = SpeedUnit::Mips;
        m_.on_uart = [this](uint8_t c) { if (on_serial) on_serial(c); };
        m_.on_flip = [this]() { flipped_ = true; present_frame(m_.framebuffer(), m_.palette(), m_.cycles()); };
    }
    const EngineInfo& info() const override { return info_; }
    simc::WriteFn mem_writer() override {
        return [this](uint32_t a, const uint8_t* d, size_t n) { return m_.write_mem(a, d, n); };
    }
    void watch_tohost(uint32_t addr) override { tohost_mode_ = true; m_.set_store_watch(addr); }
    bool start(uint32_t entry, std::string&) override {
        m_.reset(entry);
        // Without --isa-test, a watch on VID_FLIP makes run() return right after each frame.
        if (!tohost_mode_) m_.set_store_watch(VID_FLIP_ADDR);
        return true;
    }
    RunResult run(uint64_t max_cycles) override {
        RunResult r;
        if (st_.state != RunState::Running) return r;
        uint64_t c0 = m_.cycles();
        flipped_ = false;
        m_.run(max_cycles);
        r.cycles = m_.cycles() - c0;
        if (flipped_) r.events |= EvFrame;
        switch (m_.stop_reason()) {
        case iss::Stop::None: break;
        case iss::Stop::Exit:
            st_.state = RunState::Halted; st_.exit_code = m_.exit_code(); r.events |= EvHalted; break;
        case iss::Stop::Watch:
            if (tohost_mode_) { st_.state = RunState::Tohost; st_.tohost = m_.watch_value(); r.events |= EvTohost; }
            else m_.clear_stop();    // the VID_FLIP watch: just a slice boundary
            break;
        case iss::Stop::BadFetch:
        case iss::Stop::BadAccess:
            st_.state = RunState::Fault; st_.message = m_.stop_message(); r.events |= EvFault; break;
        }
        st_.pc = m_.pc();
        return r;
    }
    const Status& status() const override { return st_; }
    Stats stats() override {
        Stats s;
        s.cycles.set(m_.cycles());
        s.instret.set(m_.instret());
        s.flips.set(m_.flip_count());
        return s;
    }
    void push_key(bool pressed, uint8_t code) override { m_.push_key(pressed, code); }

private:
    static constexpr uint32_t VID_FLIP_ADDR = 0x10003000u;   // ARCHITECTURE.md section 7
    iss::Iss m_;
    EngineInfo info_;
    Status st_;
    bool tohost_mode_ = false, flipped_ = false;
};

}  // namespace

void gather_fb(const uint8_t* const lanes[4], uint8_t* out) {
    for (unsigned w = 0; w < kFbBytes / 4; w++)
        for (unsigned b = 0; b < 4; b++) out[4 * w + b] = lanes[b][w];
}

std::unique_ptr<Engine> make_engine(const std::string& name, const EngineOptions& opt, std::string& err) {
    if (name == "iss") {
        if (opt.cosim) { err = "--cosim needs an RTL engine (fast or accurate)"; return nullptr; }
        return std::unique_ptr<Engine>(new IssEngine());
    }
    if (name == "fast") return make_fast_engine(opt, err);
    if (name == "accurate") return make_accurate_engine(opt, err);
    if (name == "model") return make_model_engine(opt, err);
    err = "unknown engine '" + name + "' (iss, fast, accurate, model)";
    return nullptr;
}

}  // namespace vboard
