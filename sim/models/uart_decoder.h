// uart_decoder.h - C++ model of the serial receiver at the far end of the `uart_tx` pin.
//
// Header-only, standard library only. Call tick(level) exactly once per clock cycle
// with the value of the pin during that cycle. Recovers 8N1 bytes for a fixed
// number of clock cycles per bit; checks the framing and, in strict mode, that every
// line transition falls on an exact bit boundary of the frame it belongs to.
#ifndef UART_DECODER_H
#define UART_DECODER_H

#include <cstdint>
#include <cstddef>
#include <functional>
#include <vector>

class UartDecoder {
public:
    explicit UartDecoder(unsigned cycles_per_bit, bool strict_timing = true)
        : cpb_(cycles_per_bit ? cycles_per_bit : 1), strict_(strict_timing) {}

    // Called once per clock cycle with the uart_tx level of that cycle.
    void tick(bool level) {
        if (state_ == IDLE) {
            if (!level && prev_) {            // falling edge: start bit begins this cycle
                state_ = FRAME;
                cnt_ = 1;                     // this cycle is cycle 0 of the frame
                shift_ = 0;
            }
            prev_ = level;
            return;
        }
        // FRAME: cnt_ = index of this cycle within the frame (the first low cycle was cycle 0)
        if (cnt_ != 0 && level != prev_ && strict_ && (cnt_ % cpb_) != 0) ++timing_errors_;
        prev_ = level;
        // sample in the middle of each bit
        if ((cnt_ % cpb_) == cpb_ / 2) {
            unsigned b = cnt_ / cpb_;         // 0 = start, 1..8 data, 9 = stop
            if (b == 0) {
                if (level) { ++framing_errors_; state_ = IDLE; }   // glitch, not a start bit
            } else if (b <= 8) {
                shift_ |= uint8_t((level ? 1u : 0u) << (b - 1));
            } else {
                if (!level) ++framing_errors_;                      // stop bit must be high
                bytes_.push_back(shift_);
                if (on_byte) on_byte(shift_);
                state_ = IDLE;
                return;
            }
        }
        ++cnt_;
    }

    size_t count() const { return bytes_.size(); }
    const std::vector<uint8_t>& bytes() const { return bytes_; }
    uint64_t framing_errors() const { return framing_errors_; }
    uint64_t timing_errors() const { return timing_errors_; }   // transitions off a bit boundary
    void clear() { bytes_.clear(); }

    std::function<void(uint8_t)> on_byte;     // optional, called for every received byte

private:
    enum State { IDLE, FRAME };
    unsigned cpb_;
    bool strict_;
    State state_ = IDLE;
    bool prev_ = true;                         // line idles high
    uint64_t cnt_ = 0;
    uint8_t shift_ = 0;
    std::vector<uint8_t> bytes_;
    uint64_t framing_errors_ = 0, timing_errors_ = 0;
};

#endif
