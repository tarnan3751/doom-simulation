// Shared helpers for the peripheral testbenches.
#ifndef TB_COMMON_H
#define TB_COMMON_H
#include <cstdint>
#include <cstdio>
#include <cstdlib>

struct Checker {
    uint64_t checks = 0, fails = 0;
    const char* name;
    explicit Checker(const char* n) : name(n) {}
    void check(bool ok, const char* what, const char* file, int line) {
        ++checks;
        if (!ok) {
            ++fails;
            if (fails <= 20) std::printf("  FAIL %s:%d: %s\n", file, line, what);
        }
    }
    template <class A, class B>
    void eq(A got, B want, const char* what, const char* file, int line) {
        ++checks;
        if (static_cast<uint64_t>(got) != static_cast<uint64_t>(want)) {
            ++fails;
            if (fails <= 20)
                std::printf("  FAIL %s:%d: %s: got 0x%llx want 0x%llx\n", file, line, what,
                            (unsigned long long)got, (unsigned long long)want);
        }
    }
    int finish() const {
        std::printf("%s: %s  (%llu checks, %llu failures)\n", name, fails ? "FAIL" : "PASS",
                    (unsigned long long)checks, (unsigned long long)fails);
        return fails ? 1 : 0;
    }
};
#define CHECK(c, cond) (c).check((cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(c, got, want) (c).eq((got), (want), #got " == " #want, __FILE__, __LINE__)

// MMIO register map (ARCHITECTURE.md section 7) and the start of RAM.
enum : uint32_t {
    UART_TX = 0x10000000, UART_STATUS = 0x10000004,
    SYS_CLK_HZ = 0x10001000, SYS_EXIT = 0x10001004, SYS_FLAGS = 0x10001008,
    KBD_DATA = 0x10002000, KBD_STATUS = 0x10002004,
    VID_FLIP = 0x10003000, VID_STATUS = 0x10003004, VID_PAL = 0x10003400,
    VID_FB = 0x10010000, RAM = 0x80000000
};

// Framebuffer lanes and palette of a model whose top is `mmio` (public_flat_rw arrays).
template <class R> uint8_t fb_byte(R& r, int lane, int idx) {
    switch (lane) {
        case 0: return r.mmio__DOT__u_video__DOT__fb0[idx];
        case 1: return r.mmio__DOT__u_video__DOT__fb1[idx];
        case 2: return r.mmio__DOT__u_video__DOT__fb2[idx];
        default: return r.mmio__DOT__u_video__DOT__fb3[idx];
    }
}
template <class R> uint32_t pal_rgb(R& r, int i) { return r.mmio__DOT__u_video__DOT__palette[i] & 0xFFFFFF; }

// xorshift for deterministic pseudo-random data
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed = 0x9E3779B97F4A7C15ull) : s(seed) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return uint32_t(s >> 16); }
};
#endif
