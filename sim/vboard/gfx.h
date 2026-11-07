// gfx.h - a tiny software canvas (0x00RRGGBB) with the built-in 8x8 font. No SDL here:
// the whole window image is composed in a Canvas, shown by SDL or written to a PPM.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace vboard {

class Canvas {
public:
    Canvas(int w, int h) : w_(w), h_(h), px_(size_t(w) * h, 0) {}
    int width() const { return w_; }
    int height() const { return h_; }
    uint32_t* data() { return px_.data(); }
    const uint32_t* data() const { return px_.data(); }

    void rect(int x, int y, int w, int h, uint32_t c);            // filled, clipped
    void blit(const uint32_t* src, int sw, int sh, int dx, int dy);
    // Draw text with the top-left at (x,y); returns x after the last glyph. scale = integer zoom.
    int text(int x, int y, const std::string& s, uint32_t c, int scale = 1);
    int text_right(int right_x, int y, const std::string& s, uint32_t c, int scale = 1);
    static int text_width(const std::string& s, int scale = 1) { return int(s.size()) * 8 * scale; }

    bool write_ppm(const std::string& path, std::string& err) const;

private:
    int w_, h_;
    std::vector<uint32_t> px_;
};

}  // namespace vboard
