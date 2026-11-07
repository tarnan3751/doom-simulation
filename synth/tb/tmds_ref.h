// Reference TMDS encoder (DVI 1.0, section 3.3.3, figure 3-5) and decoder for the
// FPGA test benches. Written from the spec's pseudo-code, independently of the RTL.
#pragma once
#include <cstdint>

namespace tmds {

inline int popcount8(unsigned v) { int n = 0; for (int i = 0; i < 8; i++) n += (v >> i) & 1; return n; }

// Control tokens, q_out[9:0], for {C1, C0} = 00, 01, 10, 11.
constexpr uint16_t kCtl[4] = {0x354 /*1101010100*/, 0x0AB /*0010101011*/,
                              0x154 /*0101010100*/, 0x2AB /*1010101011*/};

struct Encoder {
    int cnt = 0;   // running disparity of the spec (int, no width limit)

    uint16_t encode(bool de, uint8_t d, bool c0, bool c1) {
        if (!de) { cnt = 0; return kCtl[(c1 ? 2 : 0) | (c0 ? 1 : 0)]; }
        // stage 1: q_m
        const int n1d = popcount8(d);
        const bool use_xnor = n1d > 4 || (n1d == 4 && (d & 1) == 0);
        unsigned qm = d & 1;
        for (int i = 1; i < 8; i++) {
            unsigned b = ((qm >> (i - 1)) & 1) ^ ((d >> i) & 1);
            if (use_xnor) b ^= 1;
            qm |= b << i;
        }
        if (!use_xnor) qm |= 1u << 8;
        // stage 2: DC balance
        const int n1 = popcount8(qm & 0xFF), n0 = 8 - n1;
        const unsigned qm8 = (qm >> 8) & 1, low = qm & 0xFF, inv = ~qm & 0xFF;
        unsigned q;
        if (cnt == 0 || n1 == n0) {
            q = ((qm8 ^ 1) << 9) | (qm8 << 8) | (qm8 ? low : inv);
            cnt += qm8 ? (n1 - n0) : (n0 - n1);
        } else if ((cnt > 0 && n1 > n0) || (cnt < 0 && n0 > n1)) {
            q = (1u << 9) | (qm8 << 8) | inv;
            cnt += 2 * int(qm8) + (n0 - n1);
        } else {
            q = (qm8 << 8) | low;
            cnt += -2 * int(qm8 ^ 1) + (n1 - n0);
        }
        return uint16_t(q);
    }
};

// Decoder: returns true for a data symbol (data in *d), false for a control token
// (c1c0 in *ctl); a 10-bit word that is neither sets *bad.
inline bool decode(uint16_t q, uint8_t* d, int* ctl, bool* bad) {
    *bad = false;
    for (int i = 0; i < 4; i++) if (q == kCtl[i]) { *ctl = i; return false; }
    unsigned x = q & 0xFF;
    if ((q >> 9) & 1) x = ~x & 0xFF;
    unsigned out = x & 1;
    for (int i = 1; i < 8; i++) {
        unsigned b = ((x >> i) & 1) ^ ((x >> (i - 1)) & 1);
        if (((q >> 8) & 1) == 0) b ^= 1;
        out |= b << i;
    }
    *d = uint8_t(out);
    return true;
}

}  // namespace tmds
