// model.cpp - the timing of the accurate machine, cycle by cycle, around the ISS. Each piece mirrors one RTL
// module under rtl/ (README.md has the map); comments name the RTL signals.
//
// Two speeds: while no miss, fill, write-back or store-buffer entry is in flight ("quiet") every request is a
// hit and completes in the next cycle, so a cycle is one pipeline step (advance()) and the SDRAM controller is
// left behind and caught up later; mul/div waits are skipped. Otherwise busy_cycle() steps the caches, the
// arbiter and the controller register by register.
#include "model.h"

#include <algorithm>
#include <cstring>

#include "soc.h"

namespace model {

namespace {
constexpr uint64_t kBoot = 512;                       // cache reset sweep: the first cycle the core can advance
constexpr uint64_t kUartBusy = 10 * 434;              // UART_DIV 434: busy from the accept edge for 10 bits
constexpr uint64_t kUartRx = 9 * 434 + 434 / 2;       // the receiver samples the stop bit (UartDecoder)
constexpr uint32_t kRamMask = 0x1FFFFFFu;             // caches and controller ignore address bits above 32 MB
constexpr uint16_t kRefi = 780;                       // refresh timer reload: 50 MHz * 64 ms / 4096 - 1

// decoder flags (rv32_decode.sv; an illegal instruction has none and rd = 0)
enum : uint16_t { kU1 = 1, kU2 = 2, kLd = 4, kSt = 8, kBr = 16, kJal = 32, kJalr = 64, kCall = 128, kRet = 256,
                  kMd = 512, kMret = 1024, kCsr = 2048, kCsrW = 4096 };
enum : uint8_t { kBkBranch, kBkJump, kBkCall, kBkRet };            // BTB entry kinds (rv32_pkg.sv)
enum : uint8_t { kPwr, kIref, kImrs, kIdle, kAct, kWburst };       // sdram_ctrl.sv states
}  // namespace

Machine::Dec Machine::decode(uint32_t insn) {
    const uint32_t op = insn & 0x7F, rd = (insn >> 7) & 31, f3 = (insn >> 12) & 7, rs1 = (insn >> 15) & 31,
                   rs2 = (insn >> 20) & 31, f7 = insn >> 25, csr = insn >> 20;
    const bool link = rd == 1 || rd == 5;
    uint16_t f = 0;
    bool legal = false, wr = false;
    switch (op) {
    case 0x37: case 0x17: legal = wr = true; break;
    case 0x6F: legal = wr = true; f = kJal | (link ? kCall : 0); break;
    case 0x67:
        legal = f3 == 0; wr = true;
        f = kJalr | kU1 | (link ? kCall : 0) | (rd == 0 && (rs1 == 1 || rs1 == 5) ? kRet : 0);
        break;
    case 0x63: legal = f3 != 2 && f3 != 3; f = kBr | kU1 | kU2; break;
    case 0x03: legal = f3 <= 2 || f3 == 4 || f3 == 5; wr = true; f = kLd | kU1; break;
    case 0x23: legal = f3 <= 2; f = kSt | kU1 | kU2; break;
    case 0x13: legal = f3 == 1 ? f7 == 0 : f3 == 5 ? (f7 == 0 || f7 == 0x20) : true; wr = true; f = kU1; break;
    case 0x33:
        wr = true; f = kU1 | kU2;
        if (f7 == 1) { legal = true; f |= kMd; }
        else legal = f7 == 0 || (f7 == 0x20 && (f3 == 0 || f3 == 5));
        break;
    case 0x0F: legal = f3 <= 1; break;
    case 0x73:
        if (f3 == 0) {
            legal = insn == 0x73 || insn == 0x100073 || insn == 0x30200073 || insn == 0x10500073;
            if (insn == 0x30200073) f = kMret;
        } else if (f3 != 4) {
            const bool w = (f3 & 3) == 1 || rs1 != 0;
            const bool exists = csr == 0x300 || csr == 0x301 || csr == 0x304 || csr == 0x305 ||
                                (csr >= 0x340 && csr <= 0x344) || csr == 0xB00 || csr == 0xB02 || csr == 0xB80 ||
                                csr == 0xB82 || csr == 0xC00 || csr == 0xC02 || csr == 0xC80 || csr == 0xC82 ||
                                (csr >= 0xF11 && csr <= 0xF14);
            legal = exists && !(w && (csr >> 10) == 3);
            wr = true;
            f = kCsr | (w ? kCsrW : 0) | ((f3 & 4) ? 0 : kU1);
        }
        break;
    default: break;
    }
    if ((insn & 3) != 3 || !legal) return Dec{0, 0, uint8_t(rs1), uint8_t(rs2)};
    return Dec{f, uint8_t(wr ? rd : 0), uint8_t(rs1), uint8_t(rs2)};
}

Machine::Machine() : iss_([] { iss::Config c; c.sys_flags = 0; return c; }()) {
    iss_.mmio_read = [this](uint32_t wa) { return mmio_read(wa); };
    const Dec d0 = decode(0);
    for (auto& e : dcache_) { e.insn = 0; e.d = d0; }
    reset(RAM_BASE);
}

void Machine::reset(uint32_t entry) {
    iss_.reset(entry);
    now_ = 0; ev_ = 0; busy_ = false;
    f_valid_ = d_valid_ = e_valid_ = false;
    f_pc_ = entry;
    btbq_ = Btb{}; d_ = e_ = Slot{}; m_ = w_ = Back{};
    md_done_ = 0; seq_ = 0;
    memset(btb_, 0, sizeof btb_); memset(bht_, 0, sizeof bht_); memset(ras_, 0, sizeof ras_);
    ras_sp_ = ras_cp_ = 0;
    cyc_off_ = minstret_ = 0;
    memset(&ic_, 0, sizeof ic_);          // tags invalid: the state right after the reset sweep
    memset(&dc_, 0, sizeof dc_);
    memset(&sd_, 0, sizeof sd_);
    sd_.state = kPwr; sd_.cnt = 10000;    // power-up wait, 200 us
    prio_d_ = false;
    uart_ready_ = ser_at_ = 0; ser_pending_ = false;
    keys_.clear(); kr_ = kn_ = 0; last_pop_ = key_drive_ = 0;
    flips_ = tohost_val_ = exit_code_ = 0; halted_ = false; fault_.clear();
    for (Cnt* c : {&instret_, &ic_access_, &ic_miss_, &dc_access_, &dc_miss_, &branch_, &branch_miss_, &jump_,
                   &jump_miss_, &flip_})
        *c = Cnt();
}

Counters Machine::counters() const {
    const uint64_t c = now_;
    return Counters{c, instret_.at(c), ic_access_.at(c), ic_miss_.at(c), dc_access_.at(c), dc_miss_.at(c),
                    branch_.at(c), branch_miss_.at(c), jump_.at(c), jump_miss_.at(c), flip_.at(c)};
}

void Machine::fail(const std::string& why) {   // the machine cannot go on: hand over the byte still on the line
    fault_ = why;
    ev_ |= EvFault;
    flush_serial();
}

// ---------------------------------------------------------------------------- run loop

unsigned Machine::run(uint64_t until) {
    ev_ = 0;
    while (now_ < until && !ev_) {
        if (busy_) { busy_cycle(); continue; }
        if (e_valid_ && (e_.d.f & kMd) && now_ < md_done_) {        // ex_busy: nothing else moves
            ic_.pend = dc_.pend = false;
            now_ = std::min(until, md_done_);
            continue;
        }
        if (now_ < kBoot) { now_ = std::min(until, kBoot); continue; }
        advance();
        now_++;
    }
    if (ser_pending_ && ser_at_ <= now_) flush_serial();
    return ev_;
}

Machine::Dec Machine::decode_at(uint32_t pc, uint32_t& insn) {
    memcpy(&insn, iss_.ram() + (pc & (kRamMask & ~3u)), 4);
    auto& e = dcache_[(pc >> 2) & 0x7FFF];
    if (e.insn != insn) { e.insn = insn; e.d = decode(insn); }
    return e.d;
}

// One step of the pipeline in a cycle with advance = 1 (rv32_core.sv, rv32_bpred.sv, rv32_csr.sv): the
// instruction in EX executes (ISS) and issues its D-port request, the IF instruction gets its prediction,
// the next fetch is issued, every stage moves; the predictor is updated after this cycle's lookups.
void Machine::advance() {
    const uint64_t c = now_;
    const Slot& e = e_;
    bool redirect = false, trap = false, taken = false, load_use = false;
    uint32_t ex_npc = 0;
    uint8_t cp_next = ras_cp_;
    Back nm{false, false, false, 0, 0};
    dc_.pend = false;
    if (e_valid_) {                                                   // commit
        uint32_t ovv = 0;
        const uint32_t* ov = nullptr;
        if (e.d.f & kCsr) {                                           // counter reads: the RTL's values
            ov = &ovv;
            switch (e.insn >> 20) {
            case 0xB00: case 0xC00: ovv = uint32_t(c + cyc_off_); break;
            case 0xB80: case 0xC80: ovv = uint32_t((c + cyc_off_) >> 32); break;
            case 0xB02: case 0xC02: ovv = uint32_t(minstret_); break;
            case 0xB82: case 0xC82: ovv = uint32_t(minstret_ >> 32); break;
            default: ov = nullptr;
            }
        }
        const iss::Retire r = iss_.step(ov);
        if (!r.valid) { fail(iss_.stop_message()); return; }
        if (r.pc != e.pc) { fail("model lost the program path"); return; }
        trap = r.trap;
        taken = (e.d.f & kBr) ? r.taken : (e.d.f & (kJal | kJalr)) != 0;
        const bool mis = taken != e.pt || (taken && ((r.next_pc ^ e.ptgt) >> 2) != 0);
        redirect = trap || (e.d.f & kMret) || mis;
        ex_npc = r.next_pc;
        if (e.d.f & (kBr | kJal | kJalr)) {
            Cnt& n = (e.d.f & kBr) ? branch_ : jump_;
            n.add(c);
            if (mis && !trap) ((e.d.f & kBr) ? branch_miss_ : jump_miss_).add(c);
        }
        nm = Back{true, trap, !trap && r.mem_wstrb && (r.mem_addr & ~3u) == watch_, uint8_t(seq_ & 3), r.mem_wdata};
        if (trace_) ring_[seq_ & 3] = r;
        seq_++;
        if (!trap) {
            if (e.d.f & (kCall | kRet)) cp_next = (cp_next + ((e.d.f & kCall) ? 1 : 7)) & 7;
            if (e.d.f & kCsrW) csr_write(r, e.insn);
            else minstret_++;
            if (e.d.f & (kLd | kSt)) {
                if (r.mem_addr >> 31) d_issue(r.mem_addr, e.d.f & kSt);
                else if (e.d.f & kSt) mmio_store(r);                  // MMIO loads: in mmio_read()
            }
        }
        load_use = d_valid_ && (e.d.f & kLd) && e.d.rd &&
                   (((d_.d.f & kU1) && d_.d.rs1 == e.d.rd) || ((d_.d.f & kU2) && d_.d.rs2 == e.d.rd));
    }
    const bool fetch_go = !(load_use && !redirect);

    // IF: prediction for the instruction there (BTB entry read at its fetch, BHT and RAS now), next fetch
    const bool hit = btbq_.valid && btbq_.tag == ((f_pc_ >> 10) & 0xFFF);
    const uint8_t cnt = bht_[(f_pc_ >> 2) & 1023];
    const bool pt = hit && (btbq_.kind != kBkBranch || (cnt & 2));
    const uint32_t ptgt = btbq_.kind == kBkRet ? ras_[ras_sp_] << 2 : (f_pc_ & 0xFF800000u) | (btbq_.tgt << 2);
    const uint32_t npc = redirect ? ex_npc : !f_valid_ ? f_pc_ : pt ? ptgt : f_pc_ + 4;
    if (redirect) {
        ras_sp_ = cp_next;
        if ((e.d.f & kCall) && !trap) ras_[cp_next] = ((e.pc >> 2) + 1) & 0x3FFFFFFF;
    } else if (fetch_go && f_valid_ && hit) {
        if (btbq_.kind == kBkCall) { ras_sp_ = (ras_sp_ + 1) & 7; ras_[ras_sp_] = ((f_pc_ >> 2) + 1) & 0x3FFFFFFF; }
        else if (btbq_.kind == kBkRet) ras_sp_ = (ras_sp_ - 1) & 7;
    }
    ras_cp_ = cp_next;
    const Btb next_q = btb_[(npc >> 2) & 255];                        // read before this edge's update
    if (nm.v) {                                                       // predictor update from EX
        const bool cf = e.d.f & (kBr | kJal | kJalr);
        if (cf ? taken && !trap : e.pt) {
            Btb& b = btb_[(e.pc >> 2) & 255];
            b.valid = cf;
            b.kind = (e.d.f & kBr) ? kBkBranch : (e.d.f & kRet) ? kBkRet : (e.d.f & kCall) ? kBkCall : kBkJump;
            b.tag = (e.pc >> 10) & 0xFFF;
            b.tgt = (ex_npc >> 2) & 0x1FFFFF;
        }
        if ((e.d.f & kBr) && !trap)
            bht_[(e.pc >> 2) & 1023] = taken ? (e.pcnt == 3 ? 3 : e.pcnt + 1) : (e.pcnt ? e.pcnt - 1 : 0);
    }

    // every stage moves; WB retires (the trace shows it in cycle c + 1)
    if (w_.v) {
        if (!w_.trap) instret_.add(c);
        if (w_.watch) { tohost_val_ = w_.wdata; ev_ |= EvTohost; }
        if (trace_) { last_ = ring_[w_.k]; ev_ |= EvRetire; }
    }
    w_ = m_;
    m_ = nm;
    const bool to_ex = d_valid_ && !redirect && !load_use;
    if (to_ex) {
        e_ = d_;
        if (e_.d.f & kMd) {                                           // rv32_muldiv.sv: cycles in EX
            const uint64_t t = c + 1;
            const uint32_t f3 = (e_.insn >> 12) & 7;
            uint32_t a = iss_.reg(e_.d.rs1), b = iss_.reg(e_.d.rs2);
            if (f3 < 4) md_done_ = t + 2;
            else if (!b) md_done_ = t + 1;
            else {
                if (!(f3 & 1)) { if (int32_t(a) < 0) a = -a; if (int32_t(b) < 0) b = -b; }
                const int za = a ? __builtin_clz(a) : 32, zb = __builtin_clz(b);
                md_done_ = t + 2 + (zb < za ? 0 : zb - za + 1);
            }
        }
    }
    e_valid_ = to_ex;
    if (!fetch_go) { ic_.pend = false; return; }                      // load-use: IF and ID hold
    d_valid_ = f_valid_ && !redirect;
    if (d_valid_) {
        d_.pc = f_pc_; d_.d = decode_at(f_pc_, d_.insn);
        d_.ptgt = ptgt; d_.pcnt = cnt; d_.pt = pt;
    }
    f_valid_ = true;
    f_pc_ = npc;
    btbq_ = next_q;
    i_issue(npc);
}

// A Zicsr instruction that wrote its CSR: a write to mcycle[h] / minstret[h] replaces that cycle's increment
// (rv32_csr.sv).
void Machine::csr_write(const iss::Retire& r, uint32_t insn) {
    const uint64_t c = now_, m = c + cyc_off_;
    switch (insn >> 20) {
    case 0xB00: cyc_off_ = ((m & ~0xFFFFFFFFull) | r.csr_wdata) - (c + 1); break;
    case 0xB80: cyc_off_ = ((uint64_t(r.csr_wdata) << 32) | (m & 0xFFFFFFFFull)) - (c + 1); break;
    case 0xB02: minstret_ = (minstret_ & ~0xFFFFFFFFull) | r.csr_wdata; return;
    case 0xB82: minstret_ = (uint64_t(r.csr_wdata) << 32) | (minstret_ & 0xFFFFFFFFull); return;
    default: break;
    }
    minstret_++;
}

// A store to MMIO, performed at the accept edge c + 1 (mmio.sv).
void Machine::mmio_store(const iss::Retire& r) {
    const uint64_t c = now_;
    switch (r.mem_addr & ~3u) {
    case UART_TX:
        flush_serial();
        if (c >= uart_ready_) {
            uart_ready_ = c + 1 + kUartBusy;
            ser_byte_ = uint8_t(r.mem_wdata); ser_at_ = c + 1 + kUartRx; ser_pending_ = true;
        }
        break;
    case SYS_EXIT:
        if (!halted_) ev_ |= EvHalted;
        halted_ = true; exit_code_ = r.mem_wdata;
        iss_.clear_stop();              // the RTL keeps executing
        break;
    case VID_FLIP:
        flips_++; flip_.add(c); ev_ |= EvFrame;
        if (on_frame) on_frame(c + 1);
        break;
    default: break;
    }
}

void Machine::flush_serial() {
    if (!ser_pending_) return;
    ser_pending_ = false;
    if (on_serial) on_serial(ser_byte_, ser_at_);
}

// ---------------------------------------------------------------------------- devices (mmio.sv)

uint32_t Machine::mmio_read(uint32_t wa) {
    const uint64_t c = now_;            // the load is in EX: rd_mux of this cycle
    switch (wa) {
    case UART_STATUS: return c >= uart_ready_;
    case SYS_CLK_HZ: return 50000000;
    case KBD_DATA: case KBD_STATUS:
        kbd_apply(c);
        if (wa == KBD_STATUS || !kn_) return kn_ != 0;
        {
            const uint32_t v = KBD_DATA_VALID | kq_[kr_];
            kr_ = (kr_ + 1) & 15; kn_--; last_pop_ = c + 1;
            return v;
        }
    case VID_FLIP: return flips_;
    case VID_STATUS: {                  // vid_timing.sv: counters step every other cycle from reset
        const uint64_t p = c >> 1, line = (p / 800) % 525;
        const uint64_t frames = p >= 392000 ? (p - 392000) / 420000 + 1 : 0;   // vsync starts at line 490
        return uint32_t(frames & 0xFFFF) << 16 | (line >= 40 && line < 440 ? 0 : VID_STATUS_VBLANK);
    }
    default: return 0;                  // SYS_FLAGS (FAST = 0), write-only and unmapped registers
    }
}

void Machine::key(uint64_t edge, uint16_t ev) { keys_.push_back({edge, ev}); }

void Machine::push_key(uint16_t ev) {
    const uint64_t drive = std::max(now_, key_drive_);
    key_drive_ = drive + 1;
    key(drive + 1, ev);
}

// kbd.sv: events enter the 16-entry FIFO at their edge unless it is full in the cycle before (a pop at the
// same edge does not make room).
void Machine::kbd_apply(uint64_t c) {
    while (!keys_.empty() && keys_.front().first <= c) {
        const auto k = keys_.front();
        keys_.pop_front();
        if (kn_ + (last_pop_ == k.first) < 16) { kq_[(kr_ + kn_) & 15] = k.second; kn_++; }
    }
}

// ---------------------------------------------------------------------------- caches (icache.sv, dcache.sv)

void Machine::i_issue(uint32_t addr) {
    const uint32_t a = (addr & kRamMask) >> 2;
    ic_.pend = ic_.first = true;
    ic_.a = a;
    ic_.hit = ic_.tag[(a >> 3) & 255] == (0x1000 | (a >> 11));
    ic_access_.add(now_);
    if (!ic_.hit) busy_ = true;
}

void Machine::d_issue(uint32_t addr, bool store) {
    const uint32_t a = (addr & kRamMask) >> 2, idx = (a >> 3) & 511;
    dc_.pend = dc_.first = true;
    dc_.own = false;
    dc_.a = a; dc_.we = store;
    dc_.vtag = dc_.tag[idx];
    dc_.hit = dc_.vtag == (0x800 | (a >> 12));
    dc_access_.add(now_);
    if (!dc_.hit) busy_ = true;
    else if (store) dc_.dirty[idx] = 1;   // set at the end of the lookup; nothing reads it before
}

void Machine::busy_cycle() {
    const uint64_t c = now_;
    if (sd_.t < c) sd_catch_up(c);
    Sdram& s = sd_;
    // arbiter (mem_arb.sv) and controller outputs
    const bool ireq = ic_.lp_req, dreq = dc_.lp_req, sel_d = dreq && (!ireq || prio_d_), bk_we = sel_d && dc_.lp_we;
    const bool can = s.state == kIdle && s.cnt == 0, do_ref = can && s.ref_pend;
    const bool gnt = can && !s.ref_pend && (ireq || dreq) && (!bk_we || !s.rd_drain);
    const bool wnext = (s.state == kAct && s.we_l) || s.state == kWburst;
    const bool rv = s.rdp & 8, irv = rv && !(s.ridp & 8), drv = rv && (s.ridp & 8);

    // I-cache: the held request in LOOKUP/WAIT
    bool iok = true, istart = false;
    if (ic_.pend) {
        const uint32_t w = ic_.a & 7;
        const bool in_fill = ic_.fill_on && (ic_.a >> 3) == ic_.fill_line;
        iok = in_fill ? ((ic_.fill_mask >> w) & 1) || (irv && ic_.fill_w == w) : ic_.hit;
        if (!in_fill && !ic_.hit) {
            if (ic_.first) ic_miss_.add(c);
            istart = !ic_.fill_on;
        }
    }
    // D-cache: the held request
    bool dok = true, dstart = false, vdirty = false, store_hit = false;
    if (dc_.pend) {
        const uint32_t w = dc_.a & 7, line = dc_.a >> 3;
        const bool in_fill = dc_.fill_on && line == dc_.fill_line;
        if (dc_.vx_on) dok = false;
        else if (dc_.own || (in_fill && !dc_.we))
            dok = ((dc_.fill_mask >> w) & 1) || (in_fill && drv && dc_.fill_w == w);
        else dok = dc_.hit && !(dc_.we && dc_.sb_v);
        if (!dc_.own && !in_fill && !dc_.hit) {
            if (dc_.first) dc_miss_.add(c);
            vdirty = (dc_.vtag & 0x800) && dc_.dirty[line & 511];
            dstart = !dc_.fill_on && !dc_.sb_v && (!dc_.wb_full || (!vdirty && !dc_.lp_req && line != dc_.wb_line));
        }
        store_hit = dok && dc_.we && !dc_.own;
    }

    // edge: I-cache fill
    if (istart) {
        ic_.fill_on = ic_.lp_req = true;
        ic_.fill_line = ic_.a >> 3; ic_.fill_w = ic_.a & 7; ic_.fill_cnt = ic_.fill_mask = 0;
        ic_.tag[(ic_.a >> 3) & 255] = 0x1000 | (ic_.a >> 11);
    } else {
        if (gnt && !sel_d) ic_.lp_req = false;
        if (irv && ic_.fill_on) {
            ic_.fill_mask |= 1 << ic_.fill_w;
            ic_.fill_w = (ic_.fill_w + 1) & 7;
            if (ic_.fill_cnt == 7) ic_.fill_on = false;
            ic_.fill_cnt = (ic_.fill_cnt + 1) & 7;
        }
    }
    // edge: D-cache fill, victim copy, write buffer, store buffer
    const bool dfill0 = dc_.fill_on, dreq0 = dc_.lp_req, wbf0 = dc_.wb_full, vx0 = dc_.vx_on, sb0 = dc_.sb_v;
    if (dstart) {
        const uint32_t w = dc_.a & 7, idx = (dc_.a >> 3) & 511;
        dc_.own = dc_.fill_on = dc_.lp_req = true; dc_.lp_we = false;
        dc_.fill_line = dc_.a >> 3; dc_.fill_w = w; dc_.fill_cnt = dc_.fill_mask = 0;
        if (vdirty) {
            dc_.vx_on = dc_.wb_full = true;
            dc_.vx_p = ((w >> 1) + 1) & 3;
            dc_.wb_line = (uint32_t(dc_.vtag & 0x7FF) << 9) | idx;
        }
        dc_.tag[idx] = 0x800 | (dc_.a >> 12);
        dc_.dirty[idx] = dc_.we;
    }
    if (vx0) {
        if (((dc_.vx_p + 1) & 3) == ((dc_.a >> 1) & 3)) dc_.vx_on = false;
        dc_.vx_p = (dc_.vx_p + 1) & 3;
    }
    if (gnt && sel_d) dc_.lp_req = false;
    else if (wbf0 && !dc_.wb_asked && !dreq0 && !dfill0 && !dstart) dc_.lp_req = dc_.lp_we = dc_.wb_asked = true;
    if (store_hit && dfill0) dc_.sb_v = true;
    else if (sb0 && !dfill0) dc_.sb_v = false;
    if (wnext) {
        if (dc_.wb_ptr == 7) dc_.wb_full = dc_.wb_asked = false;
        dc_.wb_ptr = (dc_.wb_ptr + 1) & 7;
    }
    if (drv && dfill0) {
        dc_.fill_mask |= 1 << dc_.fill_w;
        dc_.fill_w = (dc_.fill_w + 1) & 7;
        if (dc_.fill_cnt == 7) dc_.fill_on = false;
        dc_.fill_cnt = (dc_.fill_cnt + 1) & 7;
    }

    // core: a completed request leaves the cache READY unless the pipeline steps and sends the next one
    if (iok) ic_.pend = false;
    if (dok) dc_.pend = false;
    ic_.first = dc_.first = false;
    if (iok && dok && !(e_valid_ && (e_.d.f & kMd) && c < md_done_)) advance();

    sd_edge(gnt, do_ref, bk_we, sel_d);
    if (gnt) prio_d_ = !sel_d;
    now_ = c + 1;
    busy_ = ic_.fill_on || ic_.lp_req || (ic_.pend && !ic_.hit) || dc_.fill_on || dc_.lp_req || dc_.wb_full ||
            dc_.sb_v || dc_.vx_on || (dc_.pend && !dc_.hit) || s.rdp || s.rd_ins || s.state == kAct || s.state == kWburst;
}

// ---------------------------------------------------------------------------- SDRAM controller (sdram_ctrl.sv)
// 50 MHz: tRP = tRCD = 1, tRAS = tRC = tRFC = 3, CL 2, BL 8; READY wait after a READ 8, after a write burst 2.

void Machine::sd_edge(bool gnt, bool do_ref, bool bk_we, bool sel_d) {
    Sdram& s = sd_;
    const bool ref_inc = s.init_done && s.ref_timer == 0 && s.ref_pend != 7;
    const bool rd_go = s.state == kAct && !s.we_l, rd_more = s.rd_ins != 0, rd_id0 = s.rd_id;
    s.ref_timer = s.init_done && s.ref_timer ? s.ref_timer - 1 : kRefi;
    switch (s.state) {
    case kPwr: if (s.cnt) s.cnt--; else { s.iref = 7; s.state = kIref; } break;       // PRECHARGE ALL
    case kIref: if (s.cnt) s.cnt--; else { s.cnt = 2; if (s.iref) s.iref--; else s.state = kImrs; } break;   // 8 x REF
    case kImrs: if (s.cnt) s.cnt--; else { s.cnt = 1; s.init_done = true; s.state = kIdle; } break;           // MRS
    case kIdle:
        if (s.cnt) s.cnt--;
        else if (do_ref) s.cnt = 2;                                                   // AUTO REFRESH
        else if (gnt) { s.we_l = bk_we; s.id_l = sel_d; s.state = kAct; }             // ACTIVE
        break;
    case kAct:                                                                        // READ / WRITE + AP
        if (s.we_l) { s.wbeat = 1; s.state = kWburst; }
        else { s.cnt = 8; s.rd_id = s.id_l; s.state = kIdle; }
        break;
    default:
        if (s.wbeat == 7) { s.cnt = 2; s.state = kIdle; }
        s.wbeat = (s.wbeat + 1) & 7;
        break;
    }
    if (ref_inc && !do_ref) s.ref_pend++;
    else if (do_ref && !ref_inc) s.ref_pend--;
    if (rd_go) { s.rd_ins = 7; s.rd_drain = 9; }
    else { if (s.rd_ins) s.rd_ins--; if (s.rd_drain) s.rd_drain--; }
    s.rdp = ((s.rdp << 1) | (rd_go || rd_more)) & 15;                                 // beat pipe: CL + 2 stages
    s.ridp = ((s.ridp << 1) | (rd_go ? s.id_l : rd_id0)) & 15;
    s.t++;
}

// The controller with no request and no read in flight, from sd_.t to `to`: countdowns and refreshes only,
// skipped in closed form between refresh events.
void Machine::sd_catch_up(uint64_t to) {
    Sdram& s = sd_;
    while (s.t < to) {
        uint64_t k = to - s.t;
        if (s.state == kIdle) {
            k = std::min<uint64_t>(k, s.ref_timer);             // ref_inc in the cycle the timer is 0
            if (s.ref_pend) k = std::min<uint64_t>(k, s.cnt);   // do_ref in the cycle cnt is 0
        } else if (s.state == kPwr && s.t) {
            k = std::min<uint64_t>(k, s.cnt);
        } else {
            k = 0;
        }
        if (!k) { sd_edge(false, s.state == kIdle && !s.cnt && s.ref_pend, false, false); continue; }
        s.cnt = uint16_t(s.cnt > k ? s.cnt - k : 0);
        s.ref_timer = s.state == kIdle ? uint16_t(s.ref_timer - k) : kRefi;
        s.rd_drain = uint8_t(s.rd_drain > k ? s.rd_drain - k : 0);
        s.t += k;
    }
}

}  // namespace model
