// engine_model.cpp - the cycle-exact model of the accurate machine (sim/model) as a board engine. It reports
// what the accurate engine reports from the RTL's counters and stops where that engine stops (the frame, the
// halt plus the same drain, the tohost retire), so runs of either give the same statistics.
#include "engine.h"
#include "model.h"

namespace vboard {
namespace {

class ModelEngine final : public Engine {
public:
    ModelEngine() {
        info_.name = "model";
        info_.description = "cycle-exact software model (timing of the accurate machine)";
        info_.clk_hz = 50000000;
        info_.speed_unit = SpeedUnit::Mhz;
        m_.on_serial = [this](uint8_t c, uint64_t) { if (on_serial) on_serial(c); };
        m_.on_frame = [this](uint64_t cyc) { present_frame(m_.iss().framebuffer(), m_.iss().palette(), cyc); };
    }
    const EngineInfo& info() const override { return info_; }
    simc::WriteFn mem_writer() override {
        return [this](uint32_t a, const uint8_t* d, size_t n) { return m_.iss().write_mem(a, d, n); };
    }
    void watch_tohost(uint32_t addr) override { m_.watch_store(addr); trace_ = true; }
    bool start(uint32_t entry, std::string& err) override {
        if (entry != 0x80000000u) { err = "the model starts at 0x80000000 (ELF entry is not)"; return false; }
        m_.reset(entry);
        return true;
    }
    RunResult run(uint64_t max_cycles) override {
        RunResult r;
        if (st_.state != RunState::Running) return r;
        const uint64_t c0 = m_.cycle();
        unsigned ev = m_.run(c0 + max_cycles);
        if ((ev & model::EvHalted) && !(ev & (model::EvTohost | model::EvFault))) {
            // as the accurate engine: the last UART frame still on the pin, plus 64 (another 64 when tracing)
            const uint64_t end = m_.cycle() + 12 * 434 + 64 + (trace_ ? 64 : 0);
            while (m_.cycle() < end && !(ev & (model::EvTohost | model::EvFault))) ev |= m_.run(end);
            st_.exit_code = m_.exit_code();
        }
        r.cycles = m_.cycle() - c0;
        if (ev & model::EvFrame) r.events |= EvFrame;
        if (ev & model::EvFault) { st_.state = RunState::Fault; st_.message = m_.fault(); r.events |= EvFault; }
        else if (ev & model::EvTohost) { st_.state = RunState::Tohost; st_.tohost = m_.tohost_value(); r.events |= EvTohost; }
        else if (ev & model::EvHalted) { st_.state = RunState::Halted; r.events |= EvHalted; }
        return r;
    }
    const Status& status() const override { return st_; }
    Stats stats() override {
        const model::Counters c = m_.counters();
        Stats s;
        s.cycles.set(c.cycle);           s.instret.set(c.instret);
        s.ic_access.set(c.ic_access);    s.ic_miss.set(c.ic_miss);
        s.dc_access.set(c.dc_access);    s.dc_miss.set(c.dc_miss);
        s.branch.set(c.branch);          s.branch_miss.set(c.branch_miss);
        s.jump.set(c.jump);              s.jump_miss.set(c.jump_miss);
        s.flips.set(c.flip > frames_presented() ? c.flip : frames_presented());   // as the RTL engines
        return s;
    }
    void push_key(bool pressed, uint8_t code) override { m_.push_key(uint16_t((pressed ? 0x100 : 0) | code)); }

private:
    model::Machine m_;
    EngineInfo info_;
    Status st_;
    bool trace_ = false;
};

}  // namespace

std::unique_ptr<Engine> make_model_engine(const EngineOptions& opt, std::string& err) {
    if (opt.cosim) { err = "--cosim needs an RTL engine; the model is checked with --engine accurate --cosim=model"; return nullptr; }
    return std::unique_ptr<Engine>(new ModelEngine());
}

}  // namespace vboard
