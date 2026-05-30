// Single-threaded tests for Canvas (the M4 CPU framebuffer).
//
// Drawing primitives, out-of-bounds clipping, grayscale blit with scaling, text
// rendering, PPM output, and checksum determinism.

#include <cstdint>
#include <cstdio>
#include <string>

#include "retina/core/frame.hpp"
#include "retina/viz/canvas.hpp"

using namespace retina;

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("  FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static void test_fill_and_set() {
    Canvas c(4, 4);
    c.fill(10, 20, 30);
    CHECK(c.at(0, 0, 0) == 10 && c.at(3, 3, 2) == 30);
    c.set(1, 1, 200, 201, 202);
    CHECK(c.at(1, 1, 0) == 200 && c.at(1, 1, 1) == 201 && c.at(1, 1, 2) == 202);
}

// Out-of-bounds writes are clipped, not crashes or wraps.
static void test_out_of_bounds_clipped() {
    Canvas c(4, 4);
    c.fill(0, 0, 0);
    c.set(-1, 0, 255, 255, 255);
    c.set(4, 4, 255, 255, 255);
    c.set(100, 100, 255, 255, 255);
    CHECK(c.at(0, 0, 0) == 0);        // nothing leaked in
    CHECK(c.at(3, 3, 0) == 0);
}

static void test_rect() {
    Canvas c(8, 8);
    c.fill(0, 0, 0);
    c.rect(2, 2, 3, 3, 100, 100, 100);
    CHECK(c.at(2, 2, 0) == 100 && c.at(4, 4, 0) == 100);
    CHECK(c.at(1, 1, 0) == 0 && c.at(5, 5, 0) == 0);   // outside the rect
}

// A GRAY8 frame blits pixel-for-pixel at 1:1 scale, as grayscale (r=g=b=value).
static void test_blit_gray_1to1() {
    uint8_t data[4] = {10, 20, 30, 40};   // 2x2
    Frame f;
    f.data = data; f.width = 2; f.height = 2; f.format = PixelFormat::GRAY8;

    Canvas c(2, 2);
    c.blit_gray(f, 0, 0, 2, 2);
    CHECK(c.at(0, 0, 0) == 10 && c.at(0, 0, 1) == 10 && c.at(0, 0, 2) == 10);
    CHECK(c.at(1, 0, 0) == 20);
    CHECK(c.at(0, 1, 0) == 30);
    CHECK(c.at(1, 1, 0) == 40);
}

// Scaling up 2x replicates source pixels (nearest-neighbour).
static void test_blit_gray_scaled() {
    uint8_t data[4] = {10, 20, 30, 40};   // 2x2 -> 4x4
    Frame f;
    f.data = data; f.width = 2; f.height = 2; f.format = PixelFormat::GRAY8;

    Canvas c(4, 4);
    c.blit_gray(f, 0, 0, 4, 4);
    CHECK(c.at(0, 0, 0) == 10 && c.at(1, 1, 0) == 10);   // top-left block
    CHECK(c.at(2, 0, 0) == 20);                          // top-right block
    CHECK(c.at(0, 2, 0) == 30 && c.at(3, 3, 0) == 40);
}

// Text draws lit pixels for a glyph and nothing for a space.
static void test_draw_text() {
    Canvas c(64, 16);
    c.fill(0, 0, 0);
    c.draw_text(0, 0, "A", 255, 255, 255);
    bool any = false;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 6; ++x)
            if (c.at(x, y, 0) == 255) any = true;
    CHECK(any);                                  // 'A' lit some pixels

    Canvas blank(64, 16);
    blank.fill(0, 0, 0);
    blank.draw_text(0, 0, " ", 255, 255, 255);
    bool none = true;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 6; ++x)
            if (blank.at(x, y, 0) != 0) none = false;
    CHECK(none);                                 // space lit nothing
}

// PPM output has a P6 header and the right byte count.
static void test_write_ppm() {
    Canvas c(3, 2);
    c.fill(1, 2, 3);
    const std::string path = "test_canvas_out.ppm";
    CHECK(c.write_ppm(path));

    std::FILE* fp = std::fopen(path.c_str(), "rb");
    CHECK(fp != nullptr);
    if (fp) {
        char hdr[3] = {0};
        std::fread(hdr, 1, 2, fp);
        CHECK(hdr[0] == 'P' && hdr[1] == '6');
        std::fseek(fp, 0, SEEK_END);
        const long size = std::ftell(fp);
        CHECK(size == static_cast<long>(std::string("P6\n3 2\n255\n").size()) + 3 * 2 * 3);
        std::fclose(fp);
        std::remove(path.c_str());
    }
}

// Same drawing -> same checksum; a change -> a different one.
static void test_checksum_deterministic() {
    Canvas a(8, 8), b(8, 8);
    a.fill(5, 6, 7); a.rect(1, 1, 2, 2, 9, 9, 9);
    b.fill(5, 6, 7); b.rect(1, 1, 2, 2, 9, 9, 9);
    CHECK(a.checksum() == b.checksum());
    b.set(0, 0, 100, 100, 100);
    CHECK(a.checksum() != b.checksum());
}

int main() {
    struct { const char* name; void (*fn)(); } cases[] = {
        {"fill_and_set",          test_fill_and_set},
        {"out_of_bounds_clipped", test_out_of_bounds_clipped},
        {"rect",                  test_rect},
        {"blit_gray_1to1",        test_blit_gray_1to1},
        {"blit_gray_scaled",      test_blit_gray_scaled},
        {"draw_text",             test_draw_text},
        {"write_ppm",             test_write_ppm},
        {"checksum_deterministic", test_checksum_deterministic},
    };

    for (auto& c : cases) {
        int before = g_failures;
        c.fn();
        std::printf("[%s] %s\n", (g_failures == before ? "PASS" : "FAIL"), c.name);
    }

    if (g_failures == 0) {
        std::printf("\nAll %zu cases passed.\n", sizeof(cases) / sizeof(cases[0]));
        return 0;
    }
    std::printf("\n%d check(s) failed.\n", g_failures);
    return 1;
}
