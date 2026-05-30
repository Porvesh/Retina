#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "retina/core/frame.hpp"
#include "retina/viz/font5x7.hpp"

namespace retina {

// Canvas: a tiny CPU RGB framebuffer — the M4 compositor's drawing surface.
//
// No GPU: the compositor blends camera frames and draws a HUD straight into this
// pixel buffer on the CPU, then writes each composed frame to disk as a P6 .ppm
// (the simplest image format, no dependencies). The .ppm sequence stitches into
// a video/gif offline. Deterministic, so a run reproduces byte-for-byte and the
// output is unit-testable.
class Canvas {
public:
    Canvas(int width, int height)
        : w_(width), h_(height), px_(static_cast<std::size_t>(width) * height * 3, 0) {}

    int width()  const { return w_; }
    int height() const { return h_; }

    // Read one channel (0=R,1=G,2=B) at a pixel; 0 if out of bounds.
    uint8_t at(int x, int y, int ch) const {
        if (x < 0 || y < 0 || x >= w_ || y >= h_) return 0;
        return px_[(static_cast<std::size_t>(y) * w_ + x) * 3 + ch];
    }

    void set(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x < 0 || y < 0 || x >= w_ || y >= h_) return;   // clip
        const std::size_t i = (static_cast<std::size_t>(y) * w_ + x) * 3;
        px_[i] = r; px_[i + 1] = g; px_[i + 2] = b;
    }

    void fill(uint8_t r, uint8_t g, uint8_t b) {
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x) set(x, y, r, g, b);
    }

    void rect(int x, int y, int rw, int rh, uint8_t r, uint8_t g, uint8_t b) {
        for (int dy = 0; dy < rh; ++dy)
            for (int dx = 0; dx < rw; ++dx) set(x + dx, y + dy, r, g, b);
    }

    // 1px border around a rectangle.
    void border(int x, int y, int rw, int rh, uint8_t r, uint8_t g, uint8_t b) {
        for (int dx = 0; dx < rw; ++dx) { set(x + dx, y, r, g, b); set(x + dx, y + rh - 1, r, g, b); }
        for (int dy = 0; dy < rh; ++dy) { set(x, y + dy, r, g, b); set(x + rw - 1, y + dy, r, g, b); }
    }

    // Draw a GRAY8 frame scaled (nearest-neighbour) into a rect, as grayscale.
    void blit_gray(const Frame& f, int x, int y, int dst_w, int dst_h) {
        if (f.data == nullptr || f.width == 0 || f.height == 0) return;
        for (int dy = 0; dy < dst_h; ++dy) {
            const uint32_t sy = static_cast<uint32_t>(dy) * f.height / static_cast<uint32_t>(dst_h);
            for (int dx = 0; dx < dst_w; ++dx) {
                const uint32_t sx = static_cast<uint32_t>(dx) * f.width / static_cast<uint32_t>(dst_w);
                const uint8_t v = f.data[static_cast<std::size_t>(sy) * f.width + sx];
                set(x + dx, y + dy, v, v, v);
            }
        }
    }

    void draw_char(int x, int y, char c, uint8_t r, uint8_t g, uint8_t b, int scale = 1) {
        const uint8_t* gl = glyph5x7(c);
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (gl[row] & (1 << (4 - col)))
                    rect(x + col * scale, y + row * scale, scale, scale, r, g, b);
    }

    void draw_text(int x, int y, const std::string& s, uint8_t r, uint8_t g, uint8_t b,
                   int scale = 1) {
        int cx = x;
        for (char c : s) {
            draw_char(cx, y, c, r, g, b, scale);
            cx += 6 * scale;   // 5px glyph + 1px gap
        }
    }

    bool write_ppm(const std::string& path) const {
        std::FILE* fp = std::fopen(path.c_str(), "wb");
        if (!fp) return false;
        std::fprintf(fp, "P6\n%d %d\n255\n", w_, h_);
        std::fwrite(px_.data(), 1, px_.size(), fp);
        std::fclose(fp);
        return true;
    }

    // Order-independent content hash, for deterministic tests.
    uint64_t checksum() const {
        uint64_t h = 1469598103934665603ull;   // FNV-1a
        for (uint8_t b : px_) { h ^= b; h *= 1099511628211ull; }
        return h;
    }

private:
    int                  w_, h_;
    std::vector<uint8_t> px_;   // RGB8, row-major
};

} // namespace retina
