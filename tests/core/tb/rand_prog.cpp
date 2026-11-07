// rand_prog.cpp - see rand_prog.h.
#include "rand_prog.h"

#include <cstdio>
#include <cstdlib>
#include <random>

namespace {

// ------------------------------------------------------------- RV32 encoders
enum : uint32_t {
    LUI = 0x37, AUIPC = 0x17, JAL = 0x6f, JALR = 0x67, BRANCH = 0x63, LOAD = 0x03,
    STORE = 0x23, OPIMM = 0x13, OP = 0x33, FENCE = 0x0f, SYSTEM = 0x73
};

inline uint32_t R(uint32_t f7, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t rd, uint32_t op) {
    return (f7 & 0x7f) << 25 | (rs2 & 31) << 20 | (rs1 & 31) << 15 | (f3 & 7) << 12 | (rd & 31) << 7 | op;
}
inline uint32_t I(int32_t imm, uint32_t rs1, uint32_t f3, uint32_t rd, uint32_t op) {
    return (uint32_t(imm) & 0xfff) << 20 | (rs1 & 31) << 15 | (f3 & 7) << 12 | (rd & 31) << 7 | op;
}
inline uint32_t S(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3) {
    uint32_t i = uint32_t(imm);
    return ((i >> 5) & 0x7f) << 25 | (rs2 & 31) << 20 | (rs1 & 31) << 15 | (f3 & 7) << 12 | (i & 0x1f) << 7 | STORE;
}
inline uint32_t B(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3) {
    uint32_t i = uint32_t(imm);
    return ((i >> 12) & 1) << 31 | ((i >> 5) & 0x3f) << 25 | (rs2 & 31) << 20 | (rs1 & 31) << 15 |
           (f3 & 7) << 12 | ((i >> 1) & 0xf) << 8 | ((i >> 11) & 1) << 7 | BRANCH;
}
inline uint32_t U(uint32_t imm20, uint32_t rd, uint32_t op) { return (imm20 & 0xfffff) << 12 | (rd & 31) << 7 | op; }
inline uint32_t J(int32_t imm, uint32_t rd) {
    uint32_t i = uint32_t(imm);
    return ((i >> 20) & 1) << 31 | ((i >> 1) & 0x3ff) << 21 | ((i >> 11) & 1) << 20 | ((i >> 12) & 0xff) << 12 |
           (rd & 31) << 7 | JAL;
}
inline uint32_t CSR(uint32_t csr, uint32_t rs1_or_uimm, uint32_t f3, uint32_t rd) {
    return (csr & 0xfff) << 20 | (rs1_or_uimm & 31) << 15 | (f3 & 7) << 12 | (rd & 31) << 7 | SYSTEM;
}

constexpr uint32_t ECALL = 0x00000073, EBREAK = 0x00100073, MRET = 0x30200073, WFI = 0x10500073;
constexpr uint32_t NOP = 0x00000013;

constexpr uint32_t RA = 1, GP = 3, LC1 = 4, T0 = 5, TMP = 6, LC2 = 7, H0 = 30, H1 = 31;
constexpr uint32_t SCRATCH_SIZE = 4096;

int32_t sext12(uint32_t v) { return int32_t((v & 0xfff) ^ 0x800) - 0x800; }

// Tiny assembler with label fixups and two code sections ("near" at the base
// address and "far" 4 MB above it, where BTB index and tag alias with the
// near code). Branches and jal must stay within a section.
class Asm {
public:
    explicit Asm(uint32_t base) { base_[0] = base; base_[1] = base + 0x400000; }
    void section(int s) { cur_ = s; }
    int  cur_section() const { return cur_; }
    int label() { labels_.push_back({-1, -1}); return int(labels_.size()) - 1; }
    void bind(int l) { labels_[size_t(l)] = {cur_, int64_t(code_[cur_].size())}; }
    int  section_of(int l) const { return labels_[size_t(l)].first; }
    void emit(uint32_t w) { code_[cur_].push_back(w); }
    void br(uint32_t f3, uint32_t rs1, uint32_t rs2, int l) { fix_.push_back({cur_, code_[cur_].size(), l, 0, f3, rs1, rs2}); emit(0); }
    void jal(uint32_t rd, int l) { fix_.push_back({cur_, code_[cur_].size(), l, 1, 0, rd, 0}); emit(0); }
    void la(uint32_t rd, int l) { fix_.push_back({cur_, code_[cur_].size(), l, 2, 0, rd, 0}); emit(0); emit(0); }
    void li(uint32_t rd, uint32_t v) {
        uint32_t hi = (v + 0x800) >> 12;
        emit(U(hi, rd, LUI));
        emit(I(sext12(v), rd, 0, rd, OPIMM));
    }
    void finalize() {
        for (auto& f : fix_) {
            auto t = labels_[size_t(f.label)];
            if (t.second < 0) { std::fprintf(stderr, "rand_prog: unbound label\n"); std::abort(); }
            int64_t taddr = int64_t(base_[t.first]) + 4 * t.second;
            int64_t faddr = int64_t(base_[f.sec]) + 4 * int64_t(f.at);
            int32_t off = int32_t(taddr - faddr);
            uint32_t& w = code_[f.sec][f.at];
            if (f.kind == 0) {
                if (off < -4096 || off > 4094) { std::fprintf(stderr, "rand_prog: branch out of range\n"); std::abort(); }
                w = B(off, f.b, f.a, f.f3);
            } else if (f.kind == 1) {
                if (off < -(1 << 20) || off >= (1 << 20)) { std::fprintf(stderr, "rand_prog: jal out of range\n"); std::abort(); }
                w = J(off, f.a);
            } else {
                uint32_t hi = (uint32_t(off) + 0x800) >> 12;
                w = U(hi, f.a, AUIPC);
                code_[f.sec][f.at + 1] = I(sext12(uint32_t(off)), f.a, 0, f.a, OPIMM);
            }
        }
    }
    const std::vector<uint32_t>& code(int s) const { return code_[s]; }
    uint32_t base(int s) const { return base_[s]; }

private:
    struct Fix { int sec; size_t at; int label; int kind; uint32_t f3, a, b; };
    uint32_t base_[2];
    int cur_ = 0;
    std::vector<uint32_t> code_[2];
    std::vector<std::pair<int, int64_t>> labels_;
    std::vector<Fix> fix_;
};

class Gen {
public:
    Gen(uint64_t seed, const RandOptions& o, uint32_t base) : rng_(seed), a_(base), opt_(o) {
        scratch_ = base + 0x100000;
        tohost_  = base + 0x200000;
        for (uint32_t r = 8; r <= 29; r++) data_.push_back(r);
        data_.push_back(2);
    }

    RandProgram run() {
        int init = a_.label();
        handler_ = a_.label();
        a_.jal(0, init);
        // ---- trap handler: skip the trapping instruction ----
        a_.bind(handler_);
        a_.emit(CSR(0x342, 0, 2, H0));        // csrr  x30, mcause
        a_.emit(CSR(0x341, 0, 2, H1));        // csrr  x31, mepc
        a_.emit(I(4, H1, 0, H1, OPIMM));      // addi  x31, x31, 4
        a_.emit(CSR(0x341, H1, 1, 0));        // csrw  mepc, x31
        a_.emit(CSR(0x343, 0, 2, H1));        // csrr  x31, mtval
        a_.emit(CSR(0x340, H0, 1, H0));       // csrrw x30, mscratch, x30
        a_.emit(MRET);

        for (unsigned i = 0; i < opt_.functions; i++) funcs_.push_back(a_.label());
        for (unsigned i = 0; i < opt_.leaves; i++) leaves_.push_back(a_.label());

        // ---- init ----
        a_.bind(init);
        a_.la(TMP, handler_);
        a_.emit(CSR(0x305, TMP, 1, 0));       // csrw mtvec, x6
        a_.li(GP, scratch_ + SCRATCH_SIZE / 2);
        for (uint32_t r : data_) a_.li(r, special());
        a_.li(H0, uint32_t(rng_()));
        a_.li(H1, uint32_t(rng_()));
        a_.li(RA, 0); a_.li(T0, 0); a_.li(LC1, 0); a_.li(LC2, 0); a_.li(TMP, 0);

        // ---- main body ----
        for (unsigned i = 0; i < opt_.items; i++) item(0, Ctx::Main);

        // ---- end: tohost = 1, spin ----
        a_.li(TMP, tohost_);
        a_.li(H1, 1);
        a_.emit(S(0, H1, TMP, 2));
        a_.emit(J(0, 0));

        // ---- functions: odd-numbered level-1 functions live in the far section ----
        for (size_t i = 0; i < funcs_.size(); i++) {
            int f = funcs_[i];
            a_.section(int(i & 1));
            a_.bind(f);
            unsigned n = 2 + rnd(7);
            for (unsigned i = 0; i < n; i++) item(2, Ctx::Func);
            a_.emit(I(0, RA, 0, 0, JALR));    // ret
        }
        a_.section(0);
        for (int f : leaves_) {
            a_.bind(f);
            unsigned n = 1 + rnd(5);
            for (unsigned i = 0; i < n; i++) item(2, Ctx::Leaf);
            a_.emit(I(0, T0, 0, 0, JALR));    // jalr x0, 0(t0)
        }
        a_.finalize();

        RandProgram p;
        p.tohost = tohost_;
        for (int sec = 0; sec < 2; sec++)
            for (size_t i = 0; i < a_.code(sec).size(); i++)
                p.words.push_back({a_.base(sec) + 4 * uint32_t(i), a_.code(sec)[i]});
        for (uint32_t i = 0; i < SCRATCH_SIZE; i++) p.bytes.push_back({scratch_ + i, uint8_t(rng_())});
        return p;
    }

private:
    enum class Ctx { Main, Func, Leaf };

    int far_of(int fl) const {
        for (size_t i = 0; i < funcs_.size(); i++) if (funcs_[i] == fl) return int(i & 1);
        return 0;
    }
    uint32_t rnd(uint32_t n) { return uint32_t(rng_() % n); }
    bool chance(unsigned pct) { return rnd(100) < pct; }

    uint32_t special() {
        static const uint32_t sp[] = {0, 1, 0xffffffffu, 0x80000000u, 0x7fffffffu, 2, 0xfffffffeu, 0x80000001u, 31, 32};
        if (chance(45)) return sp[rnd(sizeof(sp) / sizeof(sp[0]))];
        if (chance(30)) return rnd(64) - 32;
        if (chance(40)) {                       // random magnitude (divide normalisation)
            const uint32_t v = uint32_t(rng_()) >> rnd(32);
            return chance(50) ? v : 0u - v;
        }
        return uint32_t(rng_());
    }
    uint32_t data_reg() { return data_[rnd(uint32_t(data_.size()))]; }
    uint32_t dst() {
        uint32_t r = data_reg();
        recent_.push_back(r);
        if (recent_.size() > 4) recent_.erase(recent_.begin());
        return r;
    }
    uint32_t src() {
        unsigned c = rnd(100);
        if (c < 55 && !recent_.empty()) {
            // most recent with high probability -> distance 1 / 2 / 3 forwarding
            size_t n = recent_.size();
            unsigned k = rnd(100);
            size_t back = k < 50 ? 0 : k < 75 ? 1 : k < 90 ? 2 : 3;
            if (back >= n) back = n - 1;
            return recent_[n - 1 - back];
        }
        if (c < 90) return data_reg();
        return rnd(32);
    }

    // ------------------------------------------------------------- simple ops
    void alu_r(uint32_t rd, uint32_t rs1, uint32_t rs2) {
        static const uint32_t ops[][2] = {{0, 0}, {0, 0x20}, {1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}, {5, 0x20}, {6, 0}, {7, 0}};
        auto& o = ops[rnd(10)];
        a_.emit(R(o[1], rs2, rs1, o[0], rd, OP));
    }
    void alu_i(uint32_t rd, uint32_t rs1) {
        uint32_t f3 = rnd(8);
        if (f3 == 1)      a_.emit(I(int32_t(rnd(32)), rs1, 1, rd, OPIMM));
        else if (f3 == 5) a_.emit(I(int32_t(rnd(32) | (chance(50) ? 0x400 : 0)), rs1, 5, rd, OPIMM));
        else              a_.emit(I(sext12(uint32_t(rng_())), rs1, f3, rd, OPIMM));
    }
    void md(uint32_t rd, uint32_t rs1, uint32_t rs2) { a_.emit(R(1, rs2, rs1, rnd(8), rd, OP)); }
    // explicit corner cases: INT_MIN / -1, x / 0, and the mulh family on extreme values
    void md_corner() {
        uint32_t x = dst(), y = dst();
        switch (rnd(3)) {
            case 0: a_.li(x, 0x80000000u); a_.li(y, 0xffffffffu); break;
            case 1: a_.li(x, special()); a_.li(y, 0); break;
            default: a_.li(x, special()); a_.li(y, special()); break;
        }
        unsigned n = 1 + rnd(3);
        for (unsigned i = 0; i < n; i++) a_.emit(R(1, y, x, chance(70) ? 4 + rnd(4) : rnd(4), dst(), OP));
    }

    // gp-relative load/store of random size; occasionally misaligned (traps)
    void load_gp(uint32_t rd) {
        static const uint32_t f3s[] = {0, 1, 2, 4, 5};
        uint32_t f3 = f3s[rnd(5)];
        uint32_t sz = 1u << (f3 & 3);
        int32_t off = int32_t(rnd(SCRATCH_SIZE - 4)) - int32_t(SCRATCH_SIZE / 2);
        if (!chance(4)) off &= ~int32_t(sz - 1);
        a_.emit(I(off, GP, f3, rd, LOAD));
    }
    void store_gp(uint32_t rs2) {
        uint32_t f3 = rnd(3);
        uint32_t sz = 1u << f3;
        int32_t off = int32_t(rnd(SCRATCH_SIZE - 4)) - int32_t(SCRATCH_SIZE / 2);
        if (!chance(4)) off &= ~int32_t(sz - 1);
        a_.emit(S(off, rs2, GP, f3));
    }
    // address computed from a data register: tests forwarding into the AGU
    void mem_computed(uint32_t base_src) {
        bool st = chance(40);
        uint32_t f3 = st ? rnd(3) : std::vector<uint32_t>{0, 1, 2, 4, 5}[rnd(5)];
        uint32_t sz = 1u << (f3 & 3);
        uint32_t mask = chance(5) ? 0x7ff : (0x7ff & ~(sz - 1));
        a_.emit(I(int32_t(mask), base_src, 7, TMP, OPIMM));        // andi x6, src, mask
        a_.emit(R(0, GP, TMP, 0, TMP, OP));                        // add  x6, x6, gp
        for (unsigned k = chance(40) ? 1 + rnd(2) : 0; k > 0; k--) alu_i(dst(), src());   // distance 2/3
        int32_t off = -int32_t(rnd(SCRATCH_SIZE / 2 / sz) * sz);
        if (st) a_.emit(S(off, src(), TMP, f3));
        else    a_.emit(I(off, TMP, f3, dst(), LOAD));
    }

    void csr_op() {
        static const uint32_t rw[] = {0x340, 0x304, 0x300, 0x343, 0x342, 0x341, 0xB02, 0xB82, 0xB00, 0xB80, 0x344, 0x301};
        static const uint32_t ro[] = {0xC00, 0xC80, 0xC02, 0xC82, 0xF11, 0xF12, 0xF13, 0xF14, 0x305};
        unsigned c = rnd(100);
        uint32_t rd = chance(85) ? dst() : 0;
        if (c < 30) {            // pure read of anything
            bool use_ro = chance(50);
            uint32_t csr = use_ro ? ro[rnd(9)] : rw[rnd(12)];
            uint32_t f3 = chance(50) ? 2 : (chance(50) ? 3 : (chance(50) ? 6 : 7));
            a_.emit(CSR(csr, 0, f3, rd));   // csrrs/csrrc rd, csr, x0  or  csrrsi/ci rd, csr, 0
        } else {
            uint32_t csr = rw[rnd(12)];
            if (csr == 0xB82 && chance(70)) csr = 0x340;   // keep instret mostly small
            uint32_t f3 = 1 + rnd(3);
            if (chance(35)) a_.emit(CSR(csr, rnd(32), f3 | 4, rd));
            else            a_.emit(CSR(csr, src(), f3, rd));
        }
    }

    void trap_op() {
        unsigned c = rnd(24);
        uint32_t rd = data_reg();
        switch (c) {
            case 0: a_.emit(ECALL); break;
            case 1: a_.emit(EBREAK); break;
            case 2: a_.emit(0x00000000u); break;
            case 3: a_.emit(0xffffffffu); break;
            case 4: a_.emit((uint32_t(rng_()) & ~3u) | rnd(3)); break;                 // 16-bit encodings
            case 5: { static const uint32_t o[] = {0x0b, 0x2b, 0x5b, 0x7b, 0x07, 0x27, 0x53, 0x43, 0x57, 0x77};
                      a_.emit((uint32_t(rng_()) & ~0x7fu) | o[rnd(10)]); break; }
            case 6: a_.emit(I(int32_t(0x020 | rnd(32)), src(), 1, rd, OPIMM)); break;  // slli shamt[5]
            case 7: a_.emit(I(int32_t(0x420 | rnd(32)), src(), 5, rd, OPIMM)); break;  // srai shamt[5]
            case 8: a_.emit(R(0x20, src(), src(), 1 + rnd(4) , rd, OP)); break;        // bad funct7 (f3 1..4)
            case 9: a_.emit(R(2 + rnd(30), src(), src(), rnd(8), rd, OP)); break;
            case 10: a_.emit(I(0, src(), 1 + rnd(7), rd, JALR)); break;
            case 11: a_.emit(B(8, src(), src(), 2 + rnd(2))); break;
            case 12: { static const uint32_t f[] = {3, 6, 7}; a_.emit(I(0, GP, f[rnd(3)], rd, LOAD)); break; }
            case 13: a_.emit(S(0, src(), GP, 3 + rnd(5))); break;
            case 14: a_.emit(I(0, 0, 2 + rnd(6), 0, FENCE)); break;
            case 15: { static const uint32_t s[] = {0x10200073u, 0x00200073u, 0x12000073u, 0x000000f3u, 0x00100173u, 0x30200873u};
                       a_.emit(s[rnd(6)]); break; }
            case 16: a_.emit(I(0, src(), 4, rd, SYSTEM)); break;
            case 17: { static const uint32_t u[] = {0x7c0, 0x3a0, 0x180, 0xc01, 0xc81, 0x306, 0x320, 0x310, 0xf15, 0x7b0, 0x105, 0x003, 0x302, 0x303};
                       a_.emit(CSR(u[rnd(14)], src(), 1 + rnd(3), rd)); break; }
            case 18: { static const uint32_t r[] = {0xC00, 0xC80, 0xC02, 0xC82, 0xF11, 0xF12, 0xF13, 0xF14};
                       uint32_t f3 = 1 + rnd(3);
                       uint32_t s = (f3 == 1) ? rnd(32) : 1 + rnd(31);              // always a write
                       a_.emit(CSR(r[rnd(8)], s, chance(50) ? f3 : (f3 | 4), rd)); break; }
            case 19: {   // misaligned load
                uint32_t f3 = std::vector<uint32_t>{1, 2, 5}[rnd(3)];
                int32_t off = int32_t(rnd(1000) * 4) - 2000 + int32_t(f3 == 2 ? 1 + rnd(3) : 1);
                a_.emit(I(off, GP, f3, rd, LOAD)); break; }
            case 20: {   // misaligned store
                uint32_t f3 = 1 + rnd(2);
                int32_t off = int32_t(rnd(1000) * 4) - 2000 + int32_t(f3 == 2 ? 1 + rnd(3) : 1);
                a_.emit(S(off, src(), GP, f3)); break; }
            case 21: {   // taken-or-not branch to a misaligned target: next is always pc+4
                static const int32_t offs[] = {2, 6, 10, -2, -6, 14};
                static const uint32_t f3s[] = {0, 1, 4, 5, 6, 7};
                a_.emit(B(offs[rnd(6)], src(), src(), f3s[rnd(6)])); break; }
            case 22: {   // jal to a misaligned target (always traps, rd not written)
                static const int32_t offs[] = {2, 6, -2, 10};
                a_.emit(J(offs[rnd(4)], chance(30) ? RA : rd)); break; }
            default: {   // jalr: misaligned target (trap) or bit 0 cleared to the next instruction
                // only a trapping jalr may name x1 as rd (no write happens); the
                // non-trapping ones (imm 8/9) must not clobber a live return address
                static const int32_t imms[] = {2, 3, 6, 7, 10, 11, 8, 9};
                int32_t imm = imms[rnd(8)];
                bool traps = (imm & 2) != 0;
                a_.emit(U(0, TMP, AUIPC));
                a_.emit(I(imm, TMP, 0, (traps && chance(30)) ? RA : rd, JALR)); break; }
        }
    }

    void simple() {
        unsigned c = rnd(100);
        if (c < 22)      alu_r(dst(), src(), src());
        else if (c < 38) alu_i(dst(), src());
        else if (c < 42) a_.emit(U(uint32_t(rng_()), dst(), chance(50) ? LUI : AUIPC));
        else if (c < 48) a_.li(dst(), special());
        else if (c < 57) load_gp(chance(95) ? dst() : 0);
        else if (c < 65) store_gp(src());
        else if (c < 70) mem_computed(src());
        else if (c < 76) md(dst(), src(), src());
        else if (c < 78) md_corner();
        else if (c < 85) csr_op();
        else if (c < 91) trap_op();
        else {
            switch (rnd(6)) {
                case 0: a_.emit(I(0, 0, 0, 0, FENCE)); break;
                case 1: a_.emit(I(0, 0, 1, 0, FENCE)); break;
                case 2: a_.emit(WFI); break;
                case 3: a_.emit(NOP); break;
                case 4: alu_r(0, src(), src()); break;                 // writes x0
                default: a_.emit(I(0, GP, 2, 0, LOAD)); break;         // load to x0
            }
        }
    }

    // a chain of 2..5 instructions, each consuming the previous result
    void chain() {
        uint32_t prev = src();
        unsigned n = 2 + rnd(4);
        for (unsigned i = 0; i < n; i++) {
            uint32_t other = chance(50) ? prev : src();
            uint32_t rd = dst();
            switch (rnd(9)) {
                case 0: case 1: alu_r(rd, prev, other); break;
                case 2: alu_i(rd, prev); break;
                case 3: md(rd, prev, other); break;
                case 4: a_.emit(CSR(0x340, prev, 1, rd)); break;       // csrrw rd, mscratch, prev
                case 5: mem_computed(prev); rd = recent_.back(); break;
                case 6: a_.emit(S(int32_t(rnd(256) * 4) - 1024, prev, GP, 2)); a_.emit(I(int32_t(rnd(256) * 4) - 1024, GP, 2, rd, LOAD)); break;
                case 7: { int l = a_.label(); a_.br(rnd(2) ? 0 : 5, prev, other, l); alu_i(rd, prev); a_.bind(l); break; }
                default: load_gp(rd); break;
            }
            prev = rd;
        }
    }

    // load immediately followed by a consumer of the loaded value
    void load_use() {
        uint32_t rd = dst();
        if (chance(70)) a_.emit(I(int32_t(rnd(512) * 4) - 2048, GP, 2, rd, LOAD));
        else            load_gp(rd);
        switch (rnd(8)) {
            case 0: alu_r(dst(), rd, src()); break;
            case 1: alu_r(dst(), src(), rd); break;
            case 2: a_.emit(S(int32_t(rnd(512) * 4) - 2048, rd, GP, rnd(3) == 0 ? 0 : 2)); break;   // store data
            case 3: mem_computed(rd); break;                                                        // address
            case 4: md(dst(), rd, src()); break;
            case 5: { int l = a_.label(); a_.br(std::vector<uint32_t>{0, 1, 4, 5, 6, 7}[rnd(6)], rd, src(), l); simple(); a_.bind(l); break; }
            case 6: a_.emit(CSR(0x340, rd, 1 + rnd(3), dst())); break;                              // csr source
            default: alu_i(dst(), rd); break;
        }
    }

    // ------------------------------------------------------------- items
    void item(int depth, Ctx ctx) {
        unsigned c = rnd(100);
        if (c < 40) { simple(); return; }
        if (c < 50) { chain(); return; }
        if (c < 57) { load_use(); return; }
        if (c < 66) {                                      // forward conditional branch
            static const uint32_t f3s[] = {0, 1, 4, 5, 6, 7};
            int l = a_.label();
            a_.br(f3s[rnd(6)], src(), src(), l);
            unsigned n = rnd(5);
            for (unsigned i = 0; i < n; i++) simple();
            a_.bind(l);
            return;
        }
        if (c < 69) {                                      // unconditional forward jal over junk
            int l = a_.label();
            a_.jal(0, l);
            if (chance(50)) a_.emit(0); else simple();
            a_.bind(l);
            return;
        }
        if (c < 72) {                                      // backward hop: jal fwd, branch back
            int la = a_.label(), lb = a_.label(), lc = a_.label();
            a_.jal(0, lb);
            a_.bind(la);
            simple();
            a_.jal(0, lc);
            a_.bind(lb);
            simple();
            uint32_t r = src();
            static const uint32_t always[] = {0, 5, 7};    // beq/bge/bgeu r, r: always taken
            a_.br(always[rnd(3)], r, r, la);
            a_.bind(lc);
            return;
        }
        if (c < 82 && ctx == Ctx::Main && depth < 2) {    // counted loop
            uint32_t lc = depth == 0 ? LC1 : LC2;
            a_.li(lc, 1 + rnd(depth == 0 ? 8 : 4));
            int top = a_.label();
            a_.bind(top);
            unsigned n = 2 + rnd(depth == 0 ? 6 : 4);
            for (unsigned i = 0; i < n; i++) item(depth + 1, ctx);
            a_.emit(I(-1, lc, 0, lc, OPIMM));
            a_.br(1, lc, 0, top);                           // bne lc, x0, top
            return;
        }
        if (c < 89 && ctx != Ctx::Leaf) {                 // call
            bool leaf = (ctx == Ctx::Func) || chance(30);
            int target = leaf ? leaves_[rnd(uint32_t(leaves_.size()))] : funcs_[rnd(uint32_t(funcs_.size()))];
            uint32_t link = leaf ? T0 : RA;
            bool near = a_.section_of(target) == a_.cur_section() || a_.section_of(target) < 0;
            // labels of not-yet-emitted functions: near iff even level-1 index or a leaf
            if (a_.section_of(target) < 0) near = (a_.cur_section() == 0) && (leaf || far_of(target) == 0);
            if (near && chance(60)) a_.jal(link, target);
            else { a_.la(TMP, target); a_.emit(I(0, TMP, 0, link, JALR)); }
            return;
        }
        if (c < 92) {                                      // indirect jump over an illegal word
            int l = a_.label();
            a_.la(TMP, l);
            a_.emit(I(0, TMP, 0, 0, JALR));
            a_.emit(0xffffffffu);
            a_.bind(l);
            return;
        }
        if (c < 94) {                                      // jump through a value loaded from memory
            int l = a_.label();
            int32_t off = int32_t(rnd(512) * 4) - 2048;
            uint32_t r = dst();
            a_.la(TMP, l);
            a_.emit(S(off, TMP, GP, 2));
            a_.emit(I(off, GP, 2, r, LOAD));
            a_.emit(I(0, r, 0, 0, JALR));
            a_.emit(0);
            a_.bind(l);
            return;
        }
        if (c < 96) {                                      // mret to a computed target
            int l = a_.label();
            a_.la(TMP, l);
            a_.emit(CSR(0x341, TMP, 1, 0));                // csrw mepc, x6
            a_.emit(MRET);
            a_.emit(0);
            a_.bind(l);
            return;
        }
        trap_op();
    }

    std::mt19937_64 rng_;
    Asm a_;
    RandOptions opt_;
    uint32_t scratch_ = 0, tohost_ = 0;
    int handler_ = -1;
    std::vector<uint32_t> data_, recent_;
    std::vector<int> funcs_, leaves_;
};

}  // namespace

RandProgram gen_random_program(uint64_t seed, const RandOptions& opt, uint32_t base) {
    Gen g(seed, opt, base);
    return g.run();
}
