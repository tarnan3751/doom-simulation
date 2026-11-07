// framecmp - compare two DFRM frame dumps pixel for pixel (RGB after palette lookup).
//
// usage: framecmp [-o PREFIX] [-q] [--limit N] A.dfrm B.dfrm
//   --limit N  compare only the first N frames of each file (both must hold at least N)
//   -o PREFIX  for the first differing frame write PREFIX_a.ppm, PREFIX_b.ppm and
//              PREFIX_diff.ppm (differing pixels in red over a dimmed copy of A)
//   -q         only the verdict line
// exit status: 0 = same number of frames and all identical, 1 = they differ, 2 = usage/I-O error
#include "dfrm.h"   // sim/common

#include <algorithm>
#include <cstdlib>

static int usage() {
    std::fprintf(stderr, "usage: framecmp [-o PREFIX] [-q] [--limit N] A.dfrm B.dfrm\n");
    return 2;
}

int main(int argc, char **argv) {
    std::string prefix;
    bool quiet = false;
    long limit = -1;
    std::vector<std::string> files;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) prefix = argv[++i];
        else if (a == "-q") quiet = true;
        else if (a == "--limit" && i + 1 < argc) limit = std::atol(argv[++i]);
        else if (!a.empty() && a[0] == '-') return usage();
        else files.push_back(a);
    }
    if (files.size() != 2) return usage();

    simc::DfrmReader ra, rb;
    std::string err = ra.open(files[0]);
    if (err.empty()) err = rb.open(files[1]);
    if (!err.empty()) { std::fprintf(stderr, "framecmp: %s\n", err.c_str()); return 2; }
    if (ra.width() != rb.width() || ra.height() != rb.height()) {
        std::printf("DIFFERENT: frame sizes differ (%ux%u vs %ux%u)\n", ra.width(), ra.height(), rb.width(), rb.height());
        return 1;
    }
    const uint32_t W = ra.width(), H = ra.height();
    size_t na = ra.frames(), nb = rb.frames();
    if (limit >= 0) {
        if (na < (size_t)limit || nb < (size_t)limit) {
            std::printf("DIFFERENT: --limit %ld but A has %zu and B has %zu frames\n", limit, na, nb);
            return 1;
        }
        na = nb = (size_t)limit;
    }
    size_t common = std::min(na, nb);

    size_t identical = 0, differing = 0, first = (size_t)-1;
    uint64_t total_px = 0;
    uint32_t bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    uint64_t first_px = 0;
    size_t first_pal_only = 0;           // frames whose palette differs but pixels (RGB) are the same: counted identical
    simc::DfrmFrame fa, fb;
    std::vector<uint8_t> rga, rgb;

    for (size_t n = 0; n < common; n++) {
        if (!ra.read(n, fa) || !rb.read(n, fb)) { std::fprintf(stderr, "framecmp: read error at frame %zu\n", n); return 2; }
        fa.to_rgb(rga); fb.to_rgb(rgb);
        if (rga == rgb) {
            identical++;
            if (fa.pal != fb.pal || fa.idx != fb.idx) first_pal_only++;
            continue;
        }
        differing++;
        uint64_t cnt = 0;
        uint32_t x0 = W, y0 = H, x1 = 0, y1 = 0;
        for (uint32_t y = 0; y < H; y++)
            for (uint32_t x = 0; x < W; x++) {
                size_t o = 3 * ((size_t)y * W + x);
                if (std::memcmp(&rga[o], &rgb[o], 3) != 0) {
                    cnt++;
                    x0 = std::min(x0, x); x1 = std::max(x1, x);
                    y0 = std::min(y0, y); y1 = std::max(y1, y);
                }
            }
        total_px += cnt;
        if (first == (size_t)-1) {
            first = n; first_px = cnt; bx0 = x0; by0 = y0; bx1 = x1; by1 = y1;
            if (!prefix.empty()) {
                std::vector<uint8_t> diff(rga.size());
                for (size_t i = 0; i < (size_t)W * H; i++) {
                    bool d = std::memcmp(&rga[3 * i], &rgb[3 * i], 3) != 0;
                    for (int c = 0; c < 3; c++)
                        diff[3 * i + c] = d ? (c == 0 ? 255 : 0) : (uint8_t)(rga[3 * i + c] / 3);
                }
                if (!simc::write_ppm(prefix + "_a.ppm", rga, W, H) || !simc::write_ppm(prefix + "_b.ppm", rgb, W, H) ||
                    !simc::write_ppm(prefix + "_diff.ppm", diff, W, H)) {
                    std::fprintf(stderr, "framecmp: cannot write %s_*.ppm\n", prefix.c_str());
                    return 2;
                }
            }
        }
    }

    bool same_count = (na == nb);
    bool ok = same_count && differing == 0;
    if (!quiet) {
        std::printf("A: %s  %zu frames%s\n", files[0].c_str(), na, ra.truncated() ? "  (trailing partial record ignored)" : "");
        std::printf("B: %s  %zu frames%s\n", files[1].c_str(), nb, rb.truncated() ? "  (trailing partial record ignored)" : "");
        std::printf("compared %zu frames: %zu identical, %zu differing\n", common, identical, differing);
        if (identical && first_pal_only)
            std::printf("note: %zu identical frames differ in raw palette/index bytes but not in RGB\n", first_pal_only);
        if (first != (size_t)-1)
            std::printf("first differing frame: %zu, %llu pixels, bbox x=%u..%u y=%u..%u\n", first,
                        (unsigned long long)first_px, bx0, bx1, by0, by1);
        if (differing)
            std::printf("total differing pixels: %llu\n", (unsigned long long)total_px);
        if (!same_count)
            std::printf("frame count mismatch: A has %zu, B has %zu\n", na, nb);
    }
    std::printf("%s\n", ok ? "IDENTICAL" : "DIFFERENT");
    return ok ? 0 : 1;
}
