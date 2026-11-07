// iss.h - instruction-level model of the whole SoC (ARCHITECTURE.md sections 2, 4, 7, 11).
// RV32IM + Zicsr + Zicntr, machine mode, one cycle per instruction.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace iss {

constexpr uint32_t kRamBase = 0x80000000u;
constexpr uint32_t kRamSize = 0x02000000u;  // 32 MB

// Why the machine stopped (it never resumes by itself).
enum class Stop : uint8_t {
    None,
    Exit,        // SYS_EXIT written; exit_code() valid
    BadFetch,    // instruction fetch outside RAM
    BadAccess,   // load/store to an address that is neither RAM nor a mapped MMIO block
    Watch,       // a store hit the address set with set_store_watch()
};

// One executed instruction, in the vocabulary of the RTL retire trace.
struct Retire {
    bool     valid = false;     // an instruction executed (retired or trapped). False if the machine is stopped.
    bool     trap = false;      // it raised an exception: no register/memory effect, next_pc = handler
    uint32_t pc = 0, insn = 0;
    uint8_t  rd = 0;            // 0 = no register written
    uint32_t rd_wdata = 0;      // 0 when rd == 0
    uint32_t mem_addr = 0;      // byte address of a load/store that executed (0 otherwise, also 0 for traps)
    uint8_t  mem_wstrb = 0;     // store byte lanes (0 = not a store)
    uint32_t mem_wdata = 0;     // store data shifted into its lanes; unstrobed lanes are 0
    uint32_t next_pc = 0;
    // Extras beyond the RTL trace:
    uint32_t cause = 0;         // mcause, when trap
    bool     is_load = false;
    bool     override_used = false;  // the caller's override value became the result
    bool     taken = false;          // a jal/jalr, or a branch whose condition held (not set for traps)
    uint32_t csr_wdata = 0;          // the value a Zicsr instruction wrote to its CSR (0 if it wrote none)
};

struct Config {
    uint32_t clk_hz = 50000000;     // SYS_CLK_HZ
    uint32_t sys_flags = 1;         // SYS_FLAGS (bit0 = FAST build)
    bool     illegal_mtval_is_insn = true;   // mtval as the RTL core (and Spike) set it: illegal instruction -> the instruction word, ebreak -> its pc. false: both 0
};

class Iss {
public:
    explicit Iss(const Config& cfg = Config());
    ~Iss();
    Iss(const Iss&) = delete;
    Iss& operator=(const Iss&) = delete;

    // Reset CPU, CSRs, counters and peripherals (keyboard FIFO, flip counter, palette,
    // framebuffer, stop state). RAM contents are kept. PC = pc.
    void reset(uint32_t pc = kRamBase);

    // ---- execution ----
    // Execute exactly one instruction. `override_value` may be supplied for the
    // instructions listed in README.md ("override rules"): MMIO loads and reads of
    // the counter CSRs. It is the value the hardware wrote to rd.
    Retire step(const uint32_t* override_value = nullptr);
    // Fast path: execute up to n instructions with no per-instruction record.
    // Returns the number executed; stops early when the machine stops.
    uint64_t run(uint64_t n);

    // ---- run control ----
    Stop stop_reason() const { return stop_; }
    const std::string& stop_message() const { return stop_msg_; }   // one line, includes pc
    uint32_t exit_code() const { return exit_code_; }               // valid after Stop::Exit
    void clear_stop() { stop_ = Stop::None; stop_msg_.clear(); }    // resume (e.g. after Watch)
    // Stop (Stop::Watch) after any store to the aligned word containing addr.
    void set_store_watch(uint32_t addr) { watch_ = addr & ~3u; }
    void clear_store_watch() { watch_ = 1; }
    uint32_t watch_value() const { return watch_val_; }  // lane-positioned data of the watched store
    uint32_t fault_addr() const { return fault_addr_; }
    uint32_t fault_pc() const { return fault_pc_; }

    // ---- architectural state ----
    uint32_t pc() const { return pc_; }
    void set_pc(uint32_t p) { pc_ = p; }
    uint32_t reg(unsigned i) const { return x_[i & 31]; }
    void set_reg(unsigned i, uint32_t v) { if (i & 31) x_[i & 31] = v; }
    // Non-destructive CSR read; false if the CSR does not exist.
    bool csr_peek(uint32_t csr, uint32_t& value) const;
    uint64_t cycles() const { return cycle_ + cyc_off_; }               // mcycle
    uint64_t instret() const { return cycle_ - ntraps_ + ret_off_; }    // minstret (trapping instructions do not retire)

    // ---- memory backdoor (RAM only) ----
    bool write_mem(uint32_t addr, const void* data, size_t len);
    bool read_mem(uint32_t addr, void* data, size_t len) const;
    uint8_t* ram() { return ram_; }
    const uint8_t* ram() const { return ram_; }

    // ---- peripherals, host side ----
    std::function<void(uint8_t)> on_uart;   // every UART_TX byte
    std::function<void()> on_flip;          // after every VID_FLIP write (snapshot framebuffer()/palette() here)
    // If set, MMIO loads take the register word (aligned address in) from here instead of the ISS's own devices,
    // which then see no reads (the keyboard FIFO is not popped). For a caller with its own device timing; it
    // must not also pass MMIO overrides to step().
    std::function<uint32_t(uint32_t)> mmio_read;
    void push_key(bool pressed, uint8_t code);   // 16-entry FIFO, dropped when full
    size_t kbd_pending() const { return kbd_n_; }
    uint32_t flip_count() const { return flips_; }
    const uint8_t* framebuffer() const { return fb_; }   // 64000 bytes
    const uint32_t* palette() const { return pal_; }     // 256 entries, 0x00RRGGBB

private:
    template <bool REC> uint64_t exec(uint64_t n, Retire* rec, const uint32_t* ov);
    bool csr_access(uint32_t insn, uint32_t rs1val, uint32_t& old, uint32_t& wval, const uint32_t* ov, bool& ovused);
    bool csr_read(uint32_t csr, uint32_t& v) const;
    void csr_write(uint32_t csr, uint32_t v);
    bool mmio_load(uint32_t a, uint32_t f3, uint32_t& out, const uint32_t* ov, bool& ovused);
    bool mmio_store(uint32_t a, uint32_t strb, uint32_t wdata);
    uint32_t mmio_read_word(uint32_t wa);
    void take_trap(uint32_t pc, uint32_t cause, uint32_t tval);
    void fault(Stop why, uint32_t pc, uint32_t addr, const char* what);

    Config cfg_;
    uint8_t* ram_;
    uint32_t x_[32];
    uint32_t pc_ = kRamBase;
    // CSRs
    uint32_t mie_bit_ = 0, mpie_bit_ = 0, mie_ = 0, mtvec_ = 0, mscratch_ = 0, mepc_ = 0, mcause_ = 0, mtval_ = 0;
    uint64_t cycle_ = 0, ntraps_ = 0, cyc_off_ = 0, ret_off_ = 0;
    // run control
    Stop stop_ = Stop::None;
    std::string stop_msg_;
    uint32_t exit_code_ = 0, watch_ = 1, watch_val_ = 0, fault_addr_ = 0, fault_pc_ = 0;
    // peripherals
    uint16_t kbd_[16];
    uint32_t kbd_head_ = 0, kbd_n_ = 0;
    uint32_t flips_ = 0;
    uint8_t fb_[64000];
    uint32_t pal_[256];
};

}  // namespace iss
