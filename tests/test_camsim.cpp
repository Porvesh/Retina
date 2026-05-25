// Single-threaded tests for CamSim (the deterministic frame source).
//
// Same dependency-free harness as the buffer suites. These pin down the two
// things everything downstream relies on: determinism (same seed → same frames)
// and correct frame metadata (seq, timestamps, size-per-format, content shape).

#include <cstdint>
#include <cstdio>
#include <vector>

#include "retina/camsim.hpp"

using namespace retina;

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("  FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

// seq is 1-based and increments by one; capture_ts advances by the frame
// interval (30 fps → 1e9/30 ns per frame).
static void test_seq_and_timestamps() {
    CamSim cam(8, 8, PixelFormat::GRAY8, /*seed=*/0, /*fps=*/30);
    const uint64_t dt = 1'000'000'000ull / 30;

    Frame a = cam.next();
    CHECK(a.seq == 1);
    CHECK(a.capture_ts == dt);

    Frame b = cam.next();
    CHECK(b.seq == 2);
    CHECK(b.capture_ts == 2 * dt);

    CHECK(cam.frames_emitted() == 2);
}

// Byte count matches the pixel format.
static void test_size_per_format() {
    const uint32_t w = 16, h = 8;
    const std::size_t px = static_cast<std::size_t>(w) * h;   // 128

    CHECK(CamSim(w, h, PixelFormat::GRAY8).next().size == px);
    CHECK(CamSim(w, h, PixelFormat::RGB8).next().size == px * 3);
    CHECK(CamSim(w, h, PixelFormat::NV12).next().size == px + px / 2);
    CHECK(CamSim(w, h, PixelFormat::BAYER_RGGB).next().size == px);
}

// Frame metadata carries the configured geometry/format.
static void test_geometry_and_format() {
    CamSim cam(320, 240, PixelFormat::RGB8);
    Frame f = cam.next();
    CHECK(f.width == 320);
    CHECK(f.height == 240);
    CHECK(f.format == PixelFormat::RGB8);
    CHECK(f.data != nullptr);
}

// Content is the documented deterministic shape: data[0] == (seq+seed)&0xFF and
// data[i] == data[0] + i.
static void test_content_shape() {
    CamSim cam(8, 8, PixelFormat::GRAY8, /*seed=*/5);
    Frame f = cam.next();   // seq 1, seed 5 → base = 6
    CHECK(f.data[0] == static_cast<uint8_t>((1 + 5) & 0xFF));
    bool shaped = true;
    for (uint32_t i = 0; i < f.size; ++i)
        shaped &= (f.data[i] == static_cast<uint8_t>(f.data[0] + i));
    CHECK(shaped);
}

// Same seed → byte-identical frame sequences (the determinism guarantee).
static void test_same_seed_identical() {
    CamSim a(16, 16, PixelFormat::GRAY8, /*seed=*/123);
    CamSim b(16, 16, PixelFormat::GRAY8, /*seed=*/123);
    for (int n = 0; n < 5; ++n) {
        Frame fa = a.next();
        Frame fb = b.next();
        CHECK(fa.seq == fb.seq);
        CHECK(fa.capture_ts == fb.capture_ts);
        CHECK(fa.size == fb.size);
        bool same = true;
        for (uint32_t i = 0; i < fa.size; ++i) same &= (fa.data[i] == fb.data[i]);
        CHECK(same);
    }
}

// Different seed → different content (so the seed actually does something).
static void test_different_seed_differs() {
    CamSim a(8, 8, PixelFormat::GRAY8, /*seed=*/1);
    CamSim b(8, 8, PixelFormat::GRAY8, /*seed=*/2);
    Frame fa = a.next();
    Frame fb = b.next();
    CHECK(fa.data[0] != fb.data[0]);   // base differs by the seed delta
}

int main() {
    struct { const char* name; void (*fn)(); } cases[] = {
        {"seq_and_timestamps",     test_seq_and_timestamps},
        {"size_per_format",        test_size_per_format},
        {"geometry_and_format",    test_geometry_and_format},
        {"content_shape",          test_content_shape},
        {"same_seed_identical",    test_same_seed_identical},
        {"different_seed_differs", test_different_seed_differs},
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
