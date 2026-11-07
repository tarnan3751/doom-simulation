// dfrm2ppm - export frames of a DFRM dump as PPM images, or tile them into a contact sheet.
//
// usage: dfrm2ppm FILE.dfrm [options]
//   --info               print frame count and the frames where the palette changes
//   --frame N            select frame N
//   --range A-B          select frames A..B (inclusive)
//   --every N            with no --frame/--range: every Nth frame of the whole file;
//                        with --range: every Nth frame of the range
//   --sheet COLS         tile the selected frames into ONE image with COLS columns
//   --down K             shrink by integer K (box average), useful for big sheets
//   -o OUT               single image: output file name.  Several images: prefix, files are OUT_<frame>.ppm
// With no selection option, frame 0 is exported.
#include "dfrm.h"   // sim/common

#include <algorithm>
#include <cstdlib>

static int usage() {
    std::fprintf(stderr,
        "usage: dfrm2ppm FILE.dfrm [--info] [--frame N | --range A-B] [--every N] [--sheet COLS] [--down K] -o OUT\n");
    return 2;
}

static std::vector<uint8_t> shrink(const std::vector<uint8_t> &src, uint32_t w, uint32_t h, uint32_t k,
                                   uint32_t &ow, uint32_t &oh) {
    if (k <= 1) { ow = w; oh = h; return src; }
    ow = w / k; oh = h / k;
    std::vector<uint8_t> out((size_t)ow * oh * 3);
    for (uint32_t y = 0; y < oh; y++)
        for (uint32_t x = 0; x < ow; x++)
            for (int c = 0; c < 3; c++) {
                unsigned sum = 0;
                for (uint32_t dy = 0; dy < k; dy++)
                    for (uint32_t dx = 0; dx < k; dx++)
                        sum += src[3 * ((size_t)(y * k + dy) * w + (x * k + dx)) + c];
                out[3 * ((size_t)y * ow + x) + c] = (uint8_t)(sum / (k * k));
            }
    return out;
}

int main(int argc, char **argv) {
    std::string file, out;
    bool info = false, have_frame = false, have_range = false;
    size_t fr_a = 0, fr_b = 0, every = 0;
    unsigned sheet = 0, down = 1;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> const char * { return i + 1 < argc ? argv[++i] : nullptr; };
        const char *v;
        if (a == "--info") info = true;
        else if (a == "--frame" && (v = next())) { have_frame = true; fr_a = fr_b = strtoull(v, nullptr, 10); }
        else if (a == "--range" && (v = next())) {
            if (sscanf(v, "%zu-%zu", &fr_a, &fr_b) != 2 || fr_b < fr_a) return usage();
            have_range = true;
        }
        else if (a == "--every" && (v = next())) every = strtoull(v, nullptr, 10);
        else if (a == "--sheet" && (v = next())) sheet = (unsigned)strtoul(v, nullptr, 10);
        else if (a == "--down" && (v = next())) down = (unsigned)strtoul(v, nullptr, 10);
        else if (a == "-o" && (v = next())) out = v;
        else if (!a.empty() && a[0] != '-' && file.empty()) file = a;
        else return usage();
    }
    if (file.empty() || down == 0) return usage();

    simc::DfrmReader r;
    std::string err = r.open(file);
    if (!err.empty()) { std::fprintf(stderr, "dfrm2ppm: %s\n", err.c_str()); return 2; }
    const uint32_t W = r.width(), H = r.height();
    const size_t N = r.frames();

    if (info) {
        std::printf("%s: %zu frames, %ux%u\n", file.c_str(), N, W, H);
        simc::DfrmFrame f, prev;
        size_t changes = 0;
        for (size_t n = 0; n < N; n++) {
            if (!r.read(n, f)) return 2;
            if (n == 0 || f.pal != prev.pal) {
                unsigned long sr = 0, sg = 0, sb = 0;
                for (int i = 0; i < 256; i++) { sr += f.pal[3*i]; sg += f.pal[3*i+1]; sb += f.pal[3*i+2]; }
                if (changes < 400)
                    std::printf("palette change at frame %zu: mean RGB %lu %lu %lu\n", n, sr / 256, sg / 256, sb / 256);
                changes++;
            }
            prev = f;
        }
        std::printf("%zu palette changes\n", changes);
        return 0;
    }

    if (out.empty()) return usage();
    if (!have_frame && !have_range) { fr_a = 0; fr_b = every ? N - 1 : 0; if (every) have_range = true; }
    if (fr_b >= N) fr_b = N ? N - 1 : 0;
    if (fr_a >= N) { std::fprintf(stderr, "dfrm2ppm: frame %zu out of range (%zu frames)\n", fr_a, N); return 2; }
    size_t step = every ? every : 1;

    std::vector<size_t> sel;
    for (size_t n = fr_a; n <= fr_b; n += step) sel.push_back(n);

    simc::DfrmFrame f;
    std::vector<uint8_t> rgb;
    uint32_t tw = 0, th = 0;
    if (sheet) {
        unsigned cols = std::min<size_t>(sheet, sel.size());
        unsigned rows = (unsigned)((sel.size() + cols - 1) / cols);
        uint32_t cw = W / down, ch = H / down;
        std::vector<uint8_t> img((size_t)cols * cw * rows * ch * 3, 0);
        for (size_t s = 0; s < sel.size(); s++) {
            if (!r.read(sel[s], f)) return 2;
            f.to_rgb(rgb);
            std::vector<uint8_t> t = shrink(rgb, W, H, down, tw, th);
            unsigned cx = (unsigned)(s % cols), cy = (unsigned)(s / cols);
            for (uint32_t y = 0; y < th; y++)
                std::memcpy(&img[3 * ((size_t)(cy * ch + y) * cols * cw + cx * cw)], &t[3 * (size_t)y * tw], 3 * (size_t)tw);
        }
        if (!simc::write_ppm(out, img, cols * cw, rows * ch)) { std::fprintf(stderr, "dfrm2ppm: cannot write %s\n", out.c_str()); return 2; }
        std::printf("%s: %zu frames in %ux%u tiles\n", out.c_str(), sel.size(), cols, rows);
        for (size_t s = 0; s < sel.size(); s++) std::printf("%s%zu", s ? " " : "frames: ", sel[s]);
        std::printf("\n");
        return 0;
    }
    for (size_t n : sel) {
        if (!r.read(n, f)) return 2;
        f.to_rgb(rgb);
        std::vector<uint8_t> t = shrink(rgb, W, H, down, tw, th);
        char name[64];
        std::snprintf(name, sizeof name, "_%06zu.ppm", n);
        std::string path = sel.size() == 1 ? out : out + name;
        if (!simc::write_ppm(path, t, tw, th)) { std::fprintf(stderr, "dfrm2ppm: cannot write %s\n", path.c_str()); return 2; }
    }
    return 0;
}
