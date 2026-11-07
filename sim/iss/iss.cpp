#include "iss.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "soc.h"

#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

namespace iss {

namespace {
constexpr uint32_t kMisa = 0x40001100u;  // MXL=1 (32-bit), I, M

// Exception causes
constexpr uint32_t kCauseFetchMisaligned = 0, kCauseIllegal = 2, kCauseBreakpoint = 3,
                   kCauseLoadMisaligned = 4, kCauseStoreMisaligned = 6, kCauseEcallM = 11;

inline bool is_counter_csr(uint32_t c) {
    return c == 0xB00 || c == 0xB80 || c == 0xB02 || c == 0xB82 || c == 0xC00 || c == 0xC80 || c == 0xC02 ||
           c == 0xC82;
}

inline bool mmio_block_mapped(uint32_t a) {
    uint32_t blk = a >> 12;
    return blk == (UART_BASE >> 12) || blk == (SYS_BASE >> 12) || blk == (KBD_BASE >> 12) ||
           blk == (VID_BASE >> 12) || (a >> 16) == (VID_FB_BASE >> 16);
}
}  // namespace

Iss::Iss(const Config& cfg) : cfg_(cfg) {
    ram_ = static_cast<uint8_t*>(calloc(kRamSize, 1));
    if (!ram_) { fprintf(stderr, "iss: cannot allocate RAM\n"); abort(); }
    reset();
}

Iss::~Iss() { free(ram_); }

void Iss::reset(uint32_t pc) {
    memset(x_, 0, sizeof x_);
    pc_ = pc;
    mie_bit_ = mpie_bit_ = 0;
    mie_ = mtvec_ = mscratch_ = mepc_ = mcause_ = mtval_ = 0;
    cycle_ = ntraps_ = cyc_off_ = ret_off_ = 0;
    stop_ = Stop::None;
    stop_msg_.clear();
    exit_code_ = 0;
    watch_val_ = fault_addr_ = fault_pc_ = 0;
    kbd_head_ = kbd_n_ = 0;
    flips_ = 0;
    memset(fb_, 0, sizeof fb_);
    memset(pal_, 0, sizeof pal_);
}

bool Iss::write_mem(uint32_t addr, const void* data, size_t len) {
    uint64_t off = uint64_t(addr) - kRamBase;
    if (addr < kRamBase || off + len > kRamSize) return false;
    memcpy(ram_ + off, data, len);
    return true;
}

bool Iss::read_mem(uint32_t addr, void* data, size_t len) const {
    uint64_t off = uint64_t(addr) - kRamBase;
    if (addr < kRamBase || off + len > kRamSize) return false;
    memcpy(data, ram_ + off, len);
    return true;
}

void Iss::push_key(bool pressed, uint8_t code) {
    if (kbd_n_ >= 16) return;  // dropped when full
    kbd_[(kbd_head_ + kbd_n_) & 15] = uint16_t((pressed ? 0x100 : 0) | code);
    kbd_n_++;
}

void Iss::fault(Stop why, uint32_t pc, uint32_t addr, const char* what) {
    char b[160];
    if (why == Stop::BadFetch)
        snprintf(b, sizeof b, "%s at pc=0x%08x", what, pc);
    else
        snprintf(b, sizeof b, "%s 0x%08x at pc=0x%08x", what, addr, pc);
    stop_ = why;
    stop_msg_ = b;
    fault_pc_ = pc;
    fault_addr_ = addr;
}

// ---- CSRs -----------------------------------------------------------------

bool Iss::csr_read(uint32_t csr, uint32_t& v) const {
    switch (csr) {
    case 0x300: v = (3u << 11) | (mpie_bit_ << 7) | (mie_bit_ << 3); return true;
    case 0x301: v = kMisa; return true;
    case 0x304: v = mie_; return true;
    case 0x344: v = 0; return true;  // mip: hard-wired zero
    case 0x305: v = mtvec_; return true;
    case 0x340: v = mscratch_; return true;
    case 0x341: v = mepc_; return true;
    case 0x342: v = mcause_; return true;
    case 0x343: v = mtval_; return true;
    case 0xF11: case 0xF12: case 0xF13: case 0xF14: v = 0; return true;
    case 0xB00: case 0xC00: v = uint32_t(cycles()); return true;
    case 0xB80: case 0xC80: v = uint32_t(cycles() >> 32); return true;
    case 0xB02: case 0xC02: v = uint32_t(instret()); return true;
    case 0xB82: case 0xC82: v = uint32_t(instret() >> 32); return true;
    default: return false;
    }
}

bool Iss::csr_peek(uint32_t csr, uint32_t& v) const { return csr_read(csr, v); }

void Iss::csr_write(uint32_t csr, uint32_t v) {
    switch (csr) {
    case 0x300: mie_bit_ = (v >> 3) & 1; mpie_bit_ = (v >> 7) & 1; break;
    case 0x304: mie_ = v; break;
    case 0x305: mtvec_ = v & ~3u; break;  // direct mode only
    case 0x340: mscratch_ = v; break;
    case 0x341: mepc_ = v & ~3u; break;   // IALIGN = 32
    case 0x342: mcause_ = v; break;
    case 0x343: mtval_ = v; break;
    case 0xB00: cyc_off_ = ((cycles() & ~0xFFFFFFFFull) | v) - cycle_; break;
    case 0xB80: cyc_off_ = ((uint64_t(v) << 32) | (cycles() & 0xFFFFFFFFull)) - cycle_; break;
    case 0xB02: ret_off_ = ((instret() & ~0xFFFFFFFFull) | v) - (cycle_ - ntraps_); break;
    case 0xB82: ret_off_ = ((uint64_t(v) << 32) | (instret() & 0xFFFFFFFFull)) - (cycle_ - ntraps_); break;
    default: break;  // misa, mip, mhartid... : writes ignored
    }
}

// Returns false => illegal instruction.
bool Iss::csr_access(uint32_t insn, uint32_t a, uint32_t& old, uint32_t& wval, const uint32_t* ov, bool& ovused) {
    uint32_t csr = insn >> 20, f3 = (insn >> 12) & 7, rs1f = (insn >> 15) & 31, rd = (insn >> 7) & 31;
    uint32_t op = f3 & 3;
    uint32_t src = (f3 & 4) ? rs1f : a;
    bool do_read = !(op == 1 && rd == 0);
    bool do_write = (op == 1) || rs1f != 0;
    uint32_t cur;
    if (!csr_read(csr, cur)) return false;
    if (do_write && (csr >> 10) == 3) return false;  // read-only CSR
    if (do_read && ov && is_counter_csr(csr)) { cur = *ov; ovused = true; }
    old = cur;
    if (do_write) {
        uint32_t nv = op == 1 ? src : op == 2 ? (cur | src) : (cur & ~src);
        csr_write(csr, nv);
        wval = nv;
    }
    return true;
}

void Iss::take_trap(uint32_t pc, uint32_t cause, uint32_t tval) {
    mepc_ = pc;
    mcause_ = cause;
    mtval_ = tval;
    mpie_bit_ = mie_bit_;
    mie_bit_ = 0;
    ntraps_++;
}

// ---- MMIO -----------------------------------------------------------------

uint32_t Iss::mmio_read_word(uint32_t wa) {
    if (mmio_read) return mmio_read(wa);
    switch (wa) {
    case UART_STATUS: return UART_STATUS_TX_READY;
    case SYS_CLK_HZ: return cfg_.clk_hz;
    case SYS_FLAGS: return cfg_.sys_flags;
    case KBD_DATA:
        if (!kbd_n_) return 0;
        {
            uint32_t e = kbd_[kbd_head_];
            kbd_head_ = (kbd_head_ + 1) & 15;
            kbd_n_--;
            return KBD_DATA_VALID | e;
        }
    case KBD_STATUS: return kbd_n_ ? 1 : 0;
    case VID_FLIP: return flips_;
    case VID_STATUS:  // never in an active line; frame counter derived from the clock (800*525 px, 2 clk/px)
        return (uint32_t(cycles() / 840000u) << 16) | VID_STATUS_VBLANK;
    default: break;
    }
    if (wa >= VID_PALETTE_BASE && wa < VID_PALETTE_BASE + 1024) return pal_[(wa - VID_PALETTE_BASE) >> 2];
    if (wa >= VID_FB_BASE && wa < VID_FB_BASE + sizeof fb_) {
        uint32_t w;
        memcpy(&w, fb_ + (wa - VID_FB_BASE), 4);
        return w;
    }
    return 0;  // unmapped register inside a mapped block
}

bool Iss::mmio_load(uint32_t a, uint32_t f3, uint32_t& out, const uint32_t* ov, bool& ovused) {
    uint32_t wa = a & ~3u;
    if (!mmio_block_mapped(wa)) return false;
    uint32_t w;
    if (ov) {
        // Override: the hardware's rd value replaces ours. KBD_DATA stays consistent:
        // the model pops its FIFO iff the hardware returned an event.
        if (wa == KBD_DATA) {
            if (*ov != 0 && kbd_n_) { kbd_head_ = (kbd_head_ + 1) & 15; kbd_n_--; }
        } else {
            (void)mmio_read_word(wa);
        }
        out = *ov;
        ovused = true;
        return true;
    }
    w = mmio_read_word(wa);
    uint32_t sh = (a & 3) * 8;
    switch (f3) {
    case 0: out = uint32_t(int32_t(int8_t(w >> sh))); break;
    case 1: out = uint32_t(int32_t(int16_t(w >> sh))); break;
    case 4: out = (w >> sh) & 0xFF; break;
    case 5: out = (w >> sh) & 0xFFFF; break;
    default: out = w; break;
    }
    return true;
}

bool Iss::mmio_store(uint32_t a, uint32_t strb, uint32_t wd) {
    uint32_t wa = a & ~3u;
    if (!mmio_block_mapped(wa)) return false;
    switch (wa) {
    case UART_TX:
        if ((strb & 1) && on_uart) on_uart(uint8_t(wd));
        return true;
    case SYS_EXIT:
        exit_code_ = wd;
        stop_ = Stop::Exit;
        return true;
    case VID_FLIP:
        flips_++;
        if (on_flip) on_flip();
        return true;
    default: break;
    }
    uint32_t lanes = 0;
    for (int i = 0; i < 4; i++)
        if (strb & (1u << i)) lanes |= 0xFFu << (8 * i);
    if (wa >= VID_PALETTE_BASE && wa < VID_PALETTE_BASE + 1024) {
        uint32_t& p = pal_[(wa - VID_PALETTE_BASE) >> 2];
        p = ((p & ~lanes) | (wd & lanes)) & 0x00FFFFFFu;
    } else if (wa >= VID_FB_BASE && wa < VID_FB_BASE + sizeof fb_) {
        uint32_t off = wa - VID_FB_BASE;
        for (int i = 0; i < 4; i++)
            if (strb & (1u << i)) fb_[off + i] = uint8_t(wd >> (8 * i));
    }
    return true;  // other registers in mapped blocks ignore writes
}

// ---- execution ------------------------------------------------------------

Retire Iss::step(const uint32_t* ov) {
    Retire r;
    exec<true>(1, &r, ov);
    return r;
}

uint64_t Iss::run(uint64_t n) { return exec<false>(n, nullptr, nullptr); }

template <bool REC>
uint64_t Iss::exec(uint64_t n, Retire* rec, const uint32_t* ov) {
    if (stop_ != Stop::None) return 0;
    uint32_t* const x = x_;
    uint8_t* const ram = ram_;
    uint32_t pc = pc_;
    uint64_t cyc = cycle_;
    const uint64_t start = cyc, end = cyc + n;

    while (cyc < end) {
        uint32_t off = pc - kRamBase;
        if (UNLIKELY(off >= kRamSize || (pc & 3))) {
            pc_ = pc;
            cycle_ = cyc;
            fault(Stop::BadFetch, pc, pc, "instruction fetch outside RAM");
            goto out;
        }
        {
            uint32_t insn;
            memcpy(&insn, ram + off, 4);
            const uint32_t rd = (insn >> 7) & 31;
            const uint32_t a = x[(insn >> 15) & 31], b = x[(insn >> 20) & 31];
            uint32_t next = pc + 4, wb = 0;
            bool wr = false, trapped = false, ovused = false, isload = false, tk = false;
            uint32_t tcause = 0, ttval = 0, csrw = 0;
            uint32_t maddr = 0, mwdata = 0, mstrb = 0;
            const int32_t immI = int32_t(insn) >> 20;

            switch (insn & 0x7F) {
            case 0x37: wb = insn & 0xFFFFF000u; wr = true; break;           // LUI
            case 0x17: wb = pc + (insn & 0xFFFFF000u); wr = true; break;    // AUIPC
            case 0x6F: {                                                     // JAL
                uint32_t imm = (uint32_t(int32_t(insn) >> 31) << 20) | (insn & 0xFF000) | ((insn >> 9) & 0x800) |
                               ((insn >> 20) & 0x7FE);
                uint32_t t = pc + imm;
                if (t & 3) { tcause = kCauseFetchMisaligned; ttval = t; trapped = true; break; }
                wb = pc + 4; wr = true; next = t; tk = true;
                break;
            }
            case 0x67: {                                                     // JALR
                if (((insn >> 12) & 7) != 0) { tcause = kCauseIllegal; trapped = true; break; }
                uint32_t t = (a + uint32_t(immI)) & ~1u;
                if (t & 3) { tcause = kCauseFetchMisaligned; ttval = t; trapped = true; break; }
                wb = pc + 4; wr = true; next = t; tk = true;
                break;
            }
            case 0x63: {                                                     // BRANCH
                bool take;
                switch ((insn >> 12) & 7) {
                case 0: take = a == b; break;
                case 1: take = a != b; break;
                case 4: take = int32_t(a) < int32_t(b); break;
                case 5: take = int32_t(a) >= int32_t(b); break;
                case 6: take = a < b; break;
                case 7: take = a >= b; break;
                default: tcause = kCauseIllegal; trapped = true; goto brdone;
                }
                if (take) {
                    uint32_t imm = (uint32_t(int32_t(insn) >> 31) << 12) | ((insn & 0x80) << 4) |
                                   ((insn >> 20) & 0x7E0) | ((insn >> 7) & 0x1E);
                    uint32_t t = pc + imm;
                    if (t & 3) { tcause = kCauseFetchMisaligned; ttval = t; trapped = true; break; }
                    next = t; tk = true;
                }
            brdone:
                break;
            }
            case 0x03: {                                                     // LOAD
                uint32_t f3 = (insn >> 12) & 7;
                if (f3 == 3 || f3 > 5) { tcause = kCauseIllegal; trapped = true; break; }
                uint32_t addr = a + uint32_t(immI);
                uint32_t sz = (f3 & 3) == 0 ? 1 : (f3 & 3) == 1 ? 2 : 4;
                if (addr & (sz - 1)) { tcause = kCauseLoadMisaligned; ttval = addr; trapped = true; break; }
                uint32_t o = addr - kRamBase;
                uint32_t v;
                if (LIKELY(o < kRamSize)) {
                    const uint8_t* p = ram + o;
                    switch (f3) {
                    case 0: v = uint32_t(int32_t(int8_t(*p))); break;
                    case 4: v = *p; break;
                    case 1: { uint16_t h; memcpy(&h, p, 2); v = uint32_t(int32_t(int16_t(h))); break; }
                    case 5: { uint16_t h; memcpy(&h, p, 2); v = h; break; }
                    default: memcpy(&v, p, 4); break;
                    }
                } else if (addr < kRamBase) {
                    pc_ = pc; cycle_ = cyc;
                    if (!mmio_load(addr, f3, v, ov, ovused)) {
                        fault(Stop::BadAccess, pc, addr, "load from unmapped address");
                        goto out;
                    }
                } else {
                    pc_ = pc; cycle_ = cyc;
                    fault(Stop::BadAccess, pc, addr, "load from unmapped address");
                    goto out;
                }
                wb = v; wr = true; isload = true; maddr = addr;
                break;
            }
            case 0x23: {                                                     // STORE
                uint32_t f3 = (insn >> 12) & 7;
                if (f3 > 2) { tcause = kCauseIllegal; trapped = true; break; }
                uint32_t imm = (uint32_t(int32_t(insn) >> 25) << 5) | ((insn >> 7) & 31);
                uint32_t addr = a + imm;
                uint32_t sz = 1u << f3;
                if (addr & (sz - 1)) { tcause = kCauseStoreMisaligned; ttval = addr; trapped = true; break; }
                uint32_t sh = (addr & 3) * 8;
                uint32_t strb = (sz == 4 ? 0xFu : sz == 2 ? 0x3u : 0x1u) << (addr & 3);
                uint32_t wd = (sz == 4 ? b : sz == 2 ? (b & 0xFFFFu) : (b & 0xFFu)) << sh;
                uint32_t o = addr - kRamBase;
                if (LIKELY(o < kRamSize)) {
                    if (f3 == 2) memcpy(ram + o, &b, 4);
                    else if (f3 == 1) memcpy(ram + o, &b, 2);
                    else ram[o] = uint8_t(b);
                } else if (addr < kRamBase) {
                    pc_ = pc; cycle_ = cyc;
                    if (!mmio_store(addr, strb, wd)) {
                        fault(Stop::BadAccess, pc, addr, "store to unmapped address");
                        goto out;
                    }
                } else {
                    pc_ = pc; cycle_ = cyc;
                    fault(Stop::BadAccess, pc, addr, "store to unmapped address");
                    goto out;
                }
                if (UNLIKELY((addr & ~3u) == watch_)) { watch_val_ = wd; stop_ = Stop::Watch; }
                maddr = addr; mstrb = strb; mwdata = wd;
                break;
            }
            case 0x13: {                                                     // OP-IMM
                uint32_t f3 = (insn >> 12) & 7;
                uint32_t imm = uint32_t(immI);
                wr = true;
                switch (f3) {
                case 0: wb = a + imm; break;
                case 2: wb = int32_t(a) < immI; break;
                case 3: wb = a < imm; break;
                case 4: wb = a ^ imm; break;
                case 6: wb = a | imm; break;
                case 7: wb = a & imm; break;
                case 1:
                    if ((insn >> 25) != 0) { wr = false; tcause = kCauseIllegal; trapped = true; break; }
                    wb = a << ((insn >> 20) & 31);
                    break;
                default:
                    if ((insn >> 25) == 0) wb = a >> ((insn >> 20) & 31);
                    else if ((insn >> 25) == 0x20) wb = uint32_t(int32_t(a) >> ((insn >> 20) & 31));
                    else { wr = false; tcause = kCauseIllegal; trapped = true; }
                    break;
                }
                break;
            }
            case 0x33: {                                                     // OP
                uint32_t f3 = (insn >> 12) & 7, f7 = insn >> 25;
                wr = true;
                if (f7 == 0) {
                    switch (f3) {
                    case 0: wb = a + b; break;
                    case 1: wb = a << (b & 31); break;
                    case 2: wb = int32_t(a) < int32_t(b); break;
                    case 3: wb = a < b; break;
                    case 4: wb = a ^ b; break;
                    case 5: wb = a >> (b & 31); break;
                    case 6: wb = a | b; break;
                    default: wb = a & b; break;
                    }
                } else if (f7 == 0x20 && (f3 == 0 || f3 == 5)) {
                    wb = f3 == 0 ? a - b : uint32_t(int32_t(a) >> (b & 31));
                } else if (f7 == 1) {
                    switch (f3) {
                    case 0: wb = a * b; break;
                    case 1: wb = uint32_t((int64_t(int32_t(a)) * int64_t(int32_t(b))) >> 32); break;
                    case 2: wb = uint32_t((int64_t(int32_t(a)) * int64_t(uint64_t(b))) >> 32); break;
                    case 3: wb = uint32_t((uint64_t(a) * uint64_t(b)) >> 32); break;
                    case 4:
                        if (b == 0) wb = ~0u;
                        else if (a == 0x80000000u && b == ~0u) wb = a;
                        else wb = uint32_t(int32_t(a) / int32_t(b));
                        break;
                    case 5: wb = b == 0 ? ~0u : a / b; break;
                    case 6:
                        if (b == 0) wb = a;
                        else if (a == 0x80000000u && b == ~0u) wb = 0;
                        else wb = uint32_t(int32_t(a) % int32_t(b));
                        break;
                    default: wb = b == 0 ? a : a % b; break;
                    }
                } else {
                    wr = false; tcause = kCauseIllegal; trapped = true;
                }
                break;
            }
            case 0x0F: {                                                     // FENCE / FENCE.I: no-ops
                uint32_t f3 = (insn >> 12) & 7;
                if (f3 > 1) { tcause = kCauseIllegal; trapped = true; }
                break;
            }
            case 0x73: {                                                     // SYSTEM
                uint32_t f3 = (insn >> 12) & 7;
                if (f3 == 0) {
                    if (insn == 0x00000073) { tcause = kCauseEcallM; trapped = true; }
                    else if (insn == 0x00100073) { tcause = kCauseBreakpoint; trapped = true; }
                    else if (insn == 0x30200073) { next = mepc_; mie_bit_ = mpie_bit_; mpie_bit_ = 1; }
                    else if (insn == 0x10500073) { /* wfi: no-op */ }
                    else { tcause = kCauseIllegal; trapped = true; }
                } else if (f3 == 4) {
                    tcause = kCauseIllegal; trapped = true;
                } else {
                    pc_ = pc; cycle_ = cyc;
                    uint32_t old;
                    if (csr_access(insn, a, old, csrw, ov, ovused)) { wb = old; wr = true; }
                    else { tcause = kCauseIllegal; trapped = true; }
                }
                break;
            }
            default:
                tcause = kCauseIllegal; trapped = true;
                break;
            }

            if (UNLIKELY(trapped)) {
                if (cfg_.illegal_mtval_is_insn) {
                    if (tcause == kCauseIllegal) ttval = insn;
                    else if (tcause == kCauseBreakpoint) ttval = pc;
                }
                take_trap(pc, tcause, ttval);
                next = mtvec_;
                wr = false; maddr = mwdata = mstrb = 0; isload = false; ovused = false;
            }
            if (wr) { x[rd] = wb; x[0] = 0; }
            if (REC) {
                Retire& r = *rec;
                r.valid = true; r.trap = trapped; r.pc = pc; r.insn = insn;
                r.rd = (wr && rd) ? uint8_t(rd) : 0;
                r.rd_wdata = (wr && rd) ? wb : 0;
                r.mem_addr = maddr; r.mem_wstrb = uint8_t(mstrb); r.mem_wdata = mwdata;
                r.next_pc = next;
                r.cause = trapped ? tcause : 0;
                r.is_load = isload;
                r.override_used = ovused;
                r.taken = tk && !trapped;
                r.csr_wdata = trapped ? 0 : csrw;
            }
            pc = next;
            cyc++;
            if (UNLIKELY(stop_ != Stop::None)) break;
        }
    }
out:
    pc_ = pc;
    cycle_ = cyc;
    return cyc - start;
}

template uint64_t Iss::exec<true>(uint64_t, Retire*, const uint32_t*);
template uint64_t Iss::exec<false>(uint64_t, Retire*, const uint32_t*);

}  // namespace iss
