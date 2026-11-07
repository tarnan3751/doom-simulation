#include "gfx.h"

#include <cstring>

#include "dfrm.h"
#include "font8x8.h"

namespace vboard {

void Canvas::rect(int x, int y, int w, int h, uint32_t c) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > w_ ? w_ : x + w, y1 = y + h > h_ ? h_ : y + h;
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++) px_[size_t(j) * w_ + i] = c;
}

void Canvas::blit(const uint32_t* src, int sw, int sh, int dx, int dy) {
    for (int j = 0; j < sh; j++) {
        int y = dy + j;
        if (y < 0 || y >= h_) continue;
        int x0 = dx < 0 ? -dx : 0, x1 = dx + sw > w_ ? w_ - dx : sw;
        if (x1 > x0) memcpy(&px_[size_t(y) * w_ + dx + x0], src + size_t(j) * sw + x0, sizeof(uint32_t) * (x1 - x0));
    }
}

int Canvas::text(int x, int y, const std::string& s, uint32_t c, int scale) {
    for (char ch : s) {
        unsigned u = (unsigned char)ch;
        if (u >= 0x20 && u <= 0x7E) {
            const uint8_t* g = kFont8x8[u - 0x20];
            for (int r = 0; r < 8; r++)
                for (int b = 0; b < 8; b++)
                    if (g[r] & (1u << b)) rect(x + b * scale, y + r * scale, scale, scale, c);
        }
        x += 8 * scale;
    }
    return x;
}

int Canvas::text_right(int right_x, int y, const std::string& s, uint32_t c, int scale) {
    return text(right_x - text_width(s, scale), y, s, c, scale);
}

bool Canvas::write_ppm(const std::string& path, std::string& err) const {
    std::vector<uint8_t> rgb(px_.size() * 3);
    for (size_t i = 0; i < rgb.size(); i++) rgb[i] = uint8_t(px_[i / 3] >> (16 - 8 * (i % 3)));
    if (!simc::write_ppm(path, rgb, uint32_t(w_), uint32_t(h_))) { err = "cannot create '" + path + "'"; return false; }
    return true;
}

}  // namespace vboard
