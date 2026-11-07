// video_monitor.h - C++ model of the monitor at the far end of the vid_* pins.
//
// Header-only, standard library only. Call tick() once per clock cycle with the
// vid_* signals of that cycle. On every vid_pix_ce cycle one pixel is consumed:
// the monitor rebuilds complete 640x480 frames (0x00RRGGBB), measures the timing
// (all in pixel-clock units horizontally and in lines vertically) and checks it
// against ARCHITECTURE.md section 7:
//   horizontal 640 active + 16 front porch + 96 sync + 48 back porch = 800
//   vertical   480 active + 10 front porch +  2 sync + 33 back porch = 525
//   sync pulses active low, pixel enable every 2nd clock, colour 0 outside vid_de.
// Lines are delimited by hsync falling edges; the vertical measurements count
// those edges, so a vsync edge is attributed to the line of the next active pixel.
// A frame is the set of active lines between two consecutive vsync falling edges and
// is published (frames()/frame()) at the second one.
//
// The per-cycle path is a counter increment and (on ce) one compare plus a store;
// everything else runs on signal edges only.
#ifndef VIDEO_MONITOR_H
#define VIDEO_MONITOR_H

#include <cstdint>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

struct VideoTiming {                 // expected values (defaults = section 7)
    unsigned ce_period = 2;          // clock cycles per pixel
    unsigned h_active = 640, h_fp = 16, h_sync = 96, h_bp = 48;
    unsigned v_active = 480, v_fp = 10, v_sync = 2,  v_bp = 33;
    unsigned h_total() const { return h_active + h_fp + h_sync + h_bp; }
    unsigned v_total() const { return v_active + v_fp + v_sync + v_bp; }
};

struct VideoMeasured {               // last measured values (0 = not yet seen)
    unsigned ce_period = 0;
    unsigned h_total = 0, h_active = 0, h_fp = 0, h_sync = 0, h_bp = 0;
    unsigned v_total = 0, v_active = 0, v_fp = 0, v_sync = 0, v_bp = 0;
};

class VideoMonitor {
public:
    explicit VideoMonitor(const VideoTiming& t = VideoTiming())
        : exp_(t), W_(t.h_active), H_(t.v_active),
          cur_(size_t(W_) * H_, 0), last_(size_t(W_) * H_, 0) {}

    // One call per clock cycle.
    inline void tick(bool ce, bool hsync, bool vsync, bool de, uint8_t r, uint8_t g, uint8_t b) {
        ++gap_;
        if (!ce) return;
        if (seen_ce_ && gap_ != exp_.ce_period) error("pixel enable spacing %llu clk, expected %u", (unsigned long long)gap_, exp_.ce_period);
        meas_.ce_period = unsigned(gap_);
        seen_ce_ = true;
        gap_ = 0;

        const uint8_t s = uint8_t((hsync ? 1 : 0) | (vsync ? 2 : 0) | (de ? 4 : 0));
        if (s != prev_s_) edges(s);
        if (de) {
            if (row_ >= 0 && row_ < int(H_) && col_ < W_)
                cur_[size_t(row_) * W_ + col_] = (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
            ++col_;
        } else if (r | g | b) {
            error("non-black pixel outside vid_de at pixel %llu", (unsigned long long)px_);
        }
        ++px_;
    }

    uint64_t frames() const { return frames_; }                    // complete frames seen
    const std::vector<uint32_t>& frame() const { return last_; }   // last complete frame, 0x00RRGGBB, row-major
    const VideoMeasured& measured() const { return meas_; }
    uint64_t errors() const { return errors_; }                    // number of deviations seen
    const std::vector<std::string>& error_log() const { return log_; }   // first 32 messages

private:
    void edges(uint8_t s) {
        const uint8_t ch = uint8_t(s ^ prev_s_);
        const bool hs = s & 1, vs = s & 2, de = s & 4;
        // sync asserted (low) while data is active is always wrong
        if (de && (!hs || !vs)) error("sync asserted during active video at pixel %llu", (unsigned long long)px_);

        if (ch & 1) {
            if (!hs) {                                           // hsync falling: new line
                if (have_hs_fall_) {
                    meas_.h_total = unsigned(px_ - t_hs_fall_);
                    if (meas_.h_total != exp_.h_total()) error("line period %u, expected %u", meas_.h_total, exp_.h_total());
                }
                if (de_fell_) {
                    meas_.h_fp = unsigned(px_ - t_de_fall_);
                    if (meas_.h_fp != exp_.h_fp) error("horizontal front porch %u, expected %u", meas_.h_fp, exp_.h_fp);
                    de_fell_ = false;
                }
                bp_pend_ = false;
                have_hs_fall_ = true;
                t_hs_fall_ = px_;
                ++lines_;
            } else {                                             // hsync rising
                if (have_hs_fall_) {
                    meas_.h_sync = unsigned(px_ - t_hs_fall_);
                    if (meas_.h_sync != exp_.h_sync) error("hsync low for %u pixels, expected %u", meas_.h_sync, exp_.h_sync);
                }
                t_hs_rise_ = px_;
                bp_pend_ = have_hs_fall_;
            }
        }
        if (ch & 4) {
            if (de) {                                            // active line starts
                if (bp_pend_) {
                    meas_.h_bp = unsigned(px_ - t_hs_rise_);
                    if (meas_.h_bp != exp_.h_bp) error("horizontal back porch %u, expected %u", meas_.h_bp, exp_.h_bp);
                    bp_pend_ = false;
                }
                t_de_rise_ = px_;
                col_ = 0;
                ++row_;
                last_de_line_ = lines_;
                if (row_ >= int(H_)) error("more than %u active lines in a frame", H_);
            } else {                                             // active line ends
                meas_.h_active = unsigned(px_ - t_de_rise_);
                if (meas_.h_active != exp_.h_active) error("active line %d is %u pixels, expected %u", row_, meas_.h_active, exp_.h_active);
                t_de_fall_ = px_;
                de_fell_ = true;
            }
        }
        if (ch & 2) {
            if (!vs) {                                           // vsync falling: frame boundary
                if (have_vs_fall_) {
                    unsigned rows = unsigned(row_ + 1);
                    meas_.v_active = rows;
                    meas_.v_total = unsigned(lines_ - l_vs_fall_);
                    if (rows) meas_.v_fp = unsigned(lines_ - last_de_line_ - 1);
                    if (rows != exp_.v_active) error("frame has %u active lines, expected %u", rows, exp_.v_active);
                    if (meas_.v_total != exp_.v_total()) error("frame period %u lines, expected %u", meas_.v_total, exp_.v_total());
                    if (rows && meas_.v_fp != exp_.v_fp) error("vertical front porch %u lines, expected %u", meas_.v_fp, exp_.v_fp);
                    if (rows == exp_.v_active) {
                        std::swap(cur_, last_);
                        ++frames_;
                    }
                }
                have_vs_fall_ = true;
                l_vs_fall_ = lines_;
                row_ = -1;
                bp_vpend_ = false;
            } else {                                             // vsync rising
                if (have_vs_fall_) {
                    meas_.v_sync = unsigned(lines_ - l_vs_fall_);
                    if (meas_.v_sync != exp_.v_sync) error("vsync low for %u lines, expected %u", meas_.v_sync, exp_.v_sync);
                    l_vs_rise_ = lines_;
                    bp_vpend_ = true;
                }
            }
        }
        // vertical back porch: first active line after the vsync rise (checked when it starts)
        if ((ch & 4) && de && row_ == 0 && bp_vpend_) {
            meas_.v_bp = unsigned(lines_ - l_vs_rise_);
            if (meas_.v_bp != exp_.v_bp) error("vertical back porch %u lines, expected %u", meas_.v_bp, exp_.v_bp);
            bp_vpend_ = false;
        }
        prev_s_ = s;
    }

    __attribute__((format(printf, 2, 3))) void error(const char* fmt, ...) {
        ++errors_;
        if (log_.size() < 32) {
            char buf[160];
            va_list ap;
            va_start(ap, fmt);
            std::vsnprintf(buf, sizeof buf, fmt, ap);
            va_end(ap);
            log_.emplace_back(buf);
        }
    }

    VideoTiming exp_;
    VideoMeasured meas_;
    unsigned W_, H_;
    std::vector<uint32_t> cur_, last_;
    uint64_t gap_ = 0, px_ = 0, frames_ = 0, errors_ = 0;
    uint64_t lines_ = 0;
    uint64_t t_hs_fall_ = 0, t_hs_rise_ = 0, t_de_rise_ = 0, t_de_fall_ = 0;
    uint64_t l_vs_fall_ = 0, l_vs_rise_ = 0, last_de_line_ = 0;
    unsigned col_ = 0;
    int row_ = -1;
    uint8_t prev_s_ = 3;     // idle: syncs high, de low
    bool seen_ce_ = false, have_hs_fall_ = false, have_vs_fall_ = false;
    bool de_fell_ = false, bp_pend_ = false, bp_vpend_ = false;
    std::vector<std::string> log_;
};

#endif
