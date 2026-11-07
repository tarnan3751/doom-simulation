// dfrm.h - the frame dump format "DFRM" (ARCHITECTURE.md section 13), header only: the writer the engines use
// and the reader of framecmp, dfrm2ppm and the tests.
//   header 16 bytes: "DFRM", u32 version = 1, u32 width, u32 height (little endian); then per frame
//   768 bytes palette (R,G,B x 256) and width*height palette indices. The engines write 320x200.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace simc {

class DfrmWriter {
public:
    ~DfrmWriter() { close(); }
    bool open(const std::string& path, std::string& err) {
        close();
        const uint32_t h[4] = {0x4D524644u, 1, 320, 200};   // "DFRM", version, width, height (the host is little endian)
        f_ = fopen(path.c_str(), "wb");
        if (!f_ || fwrite(h, 1, 16, f_) != 16) { err = "cannot create '" + path + "'"; close(); return false; }
        frames_ = 0;
        return true;
    }
    bool write_frame(const uint8_t* pal_rgb, const uint8_t* idx) {   // 768 palette bytes, 64000 indices
        if (!f_ || fwrite(pal_rgb, 1, 768, f_) != 768 || fwrite(idx, 1, 64000, f_) != 64000) return false;
        frames_++;
        return true;
    }
    bool write_frame_xrgb(const uint32_t* pal, const uint8_t* idx) {   // palette entries 0x00RRGGBB
        uint8_t p[768];
        for (int i = 0; i < 768; i++) p[i] = uint8_t(pal[i / 3] >> (16 - 8 * (i % 3)));
        return write_frame(p, idx);
    }
    void close() { if (f_) fclose(f_); f_ = nullptr; }
    uint64_t frames() const { return frames_; }

private:
    FILE* f_ = nullptr;
    uint64_t frames_ = 0;
};

struct DfrmFrame {
    std::vector<uint8_t> pal, idx;   // 768 bytes R,G,B per entry; width*height indices
    void to_rgb(std::vector<uint8_t>& rgb) const {   // 3 bytes per pixel after palette lookup
        rgb.resize(idx.size() * 3);
        for (size_t i = 0; i < idx.size(); i++) memcpy(&rgb[3 * i], &pal[3 * size_t(idx[i])], 3);
    }
};

class DfrmReader {
public:
    ~DfrmReader() { if (f_) fclose(f_); }
    // "" on success, otherwise "<path>: <reason>". A trailing partial record is not a frame (truncated()).
    std::string open(const std::string& path) {
        f_ = fopen(path.c_str(), "rb");
        if (!f_) return path + ": cannot open";
        uint8_t h[16];
        if (fread(h, 1, sizeof h, f_) != sizeof h || memcmp(h, "DFRM", 4) != 0) return path + ": not a DFRM file";
        auto le32 = [&](int o) { return uint32_t(h[o]) | h[o + 1] << 8 | h[o + 2] << 16 | uint32_t(h[o + 3]) << 24; };
        if (le32(4) != 1) return path + ": unsupported DFRM version";
        w_ = le32(8); h_ = le32(12);
        if (w_ == 0 || h_ == 0 || w_ > 4096 || h_ > 4096) return path + ": bad dimensions";
        fseek(f_, 0, SEEK_END);
        size_t data = size_t(ftell(f_)) - 16;
        record_ = 768 + size_t(w_) * h_;
        frames_ = data / record_;
        truncated_ = data % record_ != 0;
        fseek(f_, 16, SEEK_SET);
        return "";
    }
    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }
    size_t frames() const { return frames_; }
    bool truncated() const { return truncated_; }
    bool read(size_t n, DfrmFrame& fr) {   // frame n (random access); false past the end or on an I/O error
        if (n >= frames_ || (n != next_ && fseeko(f_, off_t(16 + n * record_), SEEK_SET) != 0)) return false;
        fr.pal.resize(768);
        fr.idx.resize(size_t(w_) * h_);
        if (fread(fr.pal.data(), 1, 768, f_) != 768 || fread(fr.idx.data(), 1, fr.idx.size(), f_) != fr.idx.size()) return false;
        next_ = n + 1;
        return true;
    }

private:
    FILE* f_ = nullptr;
    uint32_t w_ = 0, h_ = 0;
    size_t record_ = 0, frames_ = 0, next_ = 0;
    bool truncated_ = false;
};

inline bool write_ppm(const std::string& path, const std::vector<uint8_t>& rgb, uint32_t w, uint32_t h) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    bool ok = fwrite(rgb.data(), 1, rgb.size(), f) == rgb.size();
    return fclose(f) == 0 && ok;
}

}  // namespace simc
