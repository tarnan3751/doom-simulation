// model.h - cycle-exact software model of the accurate machine (soc_top FAST=0, UART_DIV=434, SDRAM at
// 50 MHz). Instructions execute in the ISS (sim/iss); this file adds the timing: the pipeline's stall,
// flush and redirect rules, the branch predictor, both caches with background fill, store buffer and write
// buffer, the line arbiter, the SDRAM controller, MMIO and device timing. See README.md.
//
// Time: cycle N is the state after N clock edges with rst low; cnt_cycle == N. Everything the RTL shows in
// cycle N (counters, retire trace, MMIO reads, VID_FLIP, halted) is what this model shows at cycle() == N.
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <string>

#include "iss.h"

namespace model {

// soc_top's cnt_* outputs as visible in the current cycle.
struct Counters {
    uint64_t cycle, instret, ic_access, ic_miss, dc_access, dc_miss, branch, branch_miss, jump, jump_miss, flip;
};

// run() returns as soon as one of these becomes visible (in the cycle the RTL engine's loop would see it).
enum Event : unsigned {
    EvFrame  = 1u << 0,   // vid_flip (on_frame has been called)
    EvHalted = 1u << 1,   // halted rose (SYS_EXIT); the machine keeps running, as the RTL does
    EvFault  = 1u << 2,   // the ISS stopped on a bad fetch/access (fault()); cannot continue
    EvTohost = 1u << 3,   // the store to the watched word retired (tohost_value())
    EvRetire = 1u << 4,   // trace mode: an instruction retired in this cycle (retired())
};

class Machine {
public:
    Machine();
    iss::Iss& iss() { return iss_; }      // RAM backdoor (load before reset), framebuffer and palette
    void reset(uint32_t entry);           // entry must be the reset PC 0x80000000
    void watch_store(uint32_t addr) { watch_ = addr & ~3u; }   // EvTohost when a store to this word retires
    void set_trace(bool on) { trace_ = on; }                     // EvRetire after every retired instruction

    unsigned run(uint64_t until);         // advance to cycle `until`, or less if an event shows up
    uint64_t cycle() const { return now_; }
    Counters counters() const;

    void key(uint64_t edge, uint16_t ev); // keyboard event {pressed, code} on key_valid/key_data before `edge`
    void push_key(uint16_t ev);           // ... as the RTL engine drives them: one per cycle from now on

    bool halted() const { return halted_; }
    uint32_t exit_code() const { return exit_code_; }
    uint32_t tohost_value() const { return tohost_val_; }
    const std::string& fault() const { return fault_; }
    const iss::Retire& retired() const { return last_; }   // after EvRetire: the record (trace_* in cycle())

    std::function<void(uint8_t, uint64_t)> on_serial;   // a byte and the cycle the receiver on uart_tx has it
    std::function<void(uint64_t)> on_frame;              // VID_FLIP, visible in that cycle; picture in iss()

private:
    struct Dec { uint16_t f; uint8_t rd, rs1, rs2; };           // what the RTL decoder says, for timing
    struct Slot { uint32_t pc, insn, ptgt; Dec d; uint8_t pcnt; bool pt; };   // ID, EX: with its prediction
    struct Back { bool v, trap, watch; uint8_t k; uint32_t wdata; };   // MEM, WB (trace mode: record in ring_[k])
    struct Btb { uint32_t tgt; uint16_t tag; uint8_t kind, valid; };
    struct Cnt {                          // a cnt_* register: an event decided in cycle c shows from cycle c + 2
        uint64_t n = 0, last = 0;         // (perf strobe flop, then the counter), at most one event per cycle
        void add(uint64_t c) { n++; last = c; }
        uint64_t at(uint64_t now) const { return n - (n && last + 2 > now); }
    };
    struct ICache {
        bool pend, first, hit, fill_on, lp_req;
        uint32_t a, fill_line;            // a: held request, RAM word address (offset >> 2)
        uint8_t fill_w, fill_cnt, fill_mask;
        uint16_t tag[256];                // {valid, tag[11:0]}
    };
    struct DCache {
        bool pend, first, hit, we, own, fill_on, lp_req, lp_we, vx_on, wb_full, wb_asked, sb_v;
        uint32_t a, fill_line, wb_line;
        uint16_t vtag;                    // tag RAM output read at the accept: {valid, tag} of the victim
        uint8_t fill_w, fill_cnt, fill_mask, vx_p, wb_ptr;
        uint16_t tag[512];                // {valid, tag[10:0]}
        uint8_t dirty[512];
    };
    struct Sdram {
        uint64_t t;                       // the cycle this state belongs to (lagging while the caches are quiet)
        uint16_t cnt, ref_timer;
        uint8_t state, iref, wbeat, ref_pend, rd_ins, rd_drain, rdp, ridp;
        bool we_l, id_l, rd_id, init_done;
    };

    static Dec decode(uint32_t insn);
    Dec decode_at(uint32_t pc, uint32_t& insn);
    void advance();                       // one step of the whole pipeline (cycle now_ advances)
    void busy_cycle();                    // one cycle with a miss, fill or write-back in flight
    void i_issue(uint32_t addr);
    void d_issue(uint32_t addr, bool store);
    void csr_write(const iss::Retire& r, uint32_t insn);
    void mmio_store(const iss::Retire& r);
    uint32_t mmio_read(uint32_t wa);
    void kbd_apply(uint64_t c);
    void sd_edge(bool gnt, bool do_ref, bool bk_we, bool sel_d);
    void sd_catch_up(uint64_t to);
    void flush_serial();
    void fail(const std::string& why);

    iss::Iss iss_;
    uint64_t now_ = 0;
    unsigned ev_ = 0;
    bool busy_ = false, trace_ = false;
    // core
    bool f_valid_ = false, d_valid_ = false, e_valid_ = false;
    uint32_t f_pc_ = 0;
    Btb btbq_{};                          // BTB entry read at the fetch of f_pc_
    Slot d_{}, e_{};
    Back m_{}, w_{};
    uint64_t md_done_ = 0;                // first cycle the mul/div in EX may leave
    iss::Retire ring_[4], last_;
    uint32_t seq_ = 0;
    Btb btb_[256];
    uint8_t bht_[1024];
    uint32_t ras_[8];
    uint8_t ras_sp_ = 0, ras_cp_ = 0;
    uint64_t cyc_off_ = 0, minstret_ = 0; // mcycle = cycle + cyc_off_
    // memory system
    ICache ic_;
    DCache dc_;
    Sdram sd_;
    bool prio_d_ = false;
    // devices
    uint64_t uart_ready_ = 0, ser_at_ = 0;
    bool ser_pending_ = false;
    uint8_t ser_byte_ = 0;
    std::deque<std::pair<uint64_t, uint16_t>> keys_;   // (edge, event) not yet in the FIFO
    uint16_t kq_[16];
    unsigned kr_ = 0, kn_ = 0;
    uint64_t last_pop_ = 0, key_drive_ = 0;
    uint32_t flips_ = 0, watch_ = 1, tohost_val_ = 0, exit_code_ = 0;
    bool halted_ = false;
    std::string fault_;
    Cnt instret_, ic_access_, ic_miss_, dc_access_, dc_miss_, branch_, branch_miss_, jump_, jump_miss_, flip_;
    struct { uint32_t insn; Dec d; } dcache_[1 << 15];   // decoded instructions, indexed by pc
};

}  // namespace model
