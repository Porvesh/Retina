// Single-threaded tests for CamSim (the deterministic frame source).
//
// Same dependency-free harness as the buffer suites. These pin down the two
// things everything downstream relies on: determinism (same seed → same frames)
// and correct frame metadata (seq, timestamps, size-per-format, content shape).

#include <cstdint>
#include <cstdio>
#include <vector>

#include "retina/sim/camsim.hpp"

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

// ─── fault injection ─────────────────────────────────────────────────────────

// No faults → no drops, contiguous seqs (the default, made explicit).
static void test_no_faults_no_drops() {
    CamSim cam(8, 8, PixelFormat::GRAY8);
    uint64_t last = 0;
    for (int n = 0; n < 100; ++n) {
        Frame f = cam.next();
        CHECK(f.seq == last + 1);   // strictly contiguous
        last = f.seq;
    }
    CHECK(cam.frames_dropped() == 0);
    CHECK(cam.frames_emitted() == 100);
}

// A drop rate produces seq GAPS: emitted frames stay strictly increasing, some
// frame numbers are skipped, and the accounting adds up (dropped + emitted ==
// the last tick number).
static void test_drop_rate_makes_gaps() {
    CamSim cam(8, 8, PixelFormat::GRAY8, /*seed=*/42, /*fps=*/30, {/*drop=*/0.5, /*jitter=*/0});
    uint64_t last = 0, gaps = 0;
    for (int n = 0; n < 200; ++n) {
        Frame f = cam.next();
        CHECK(f.seq > last);             // never repeats or goes backwards
        if (f.seq > last + 1) ++gaps;    // a skipped frame number = a capture drop
        last = f.seq;
    }
    CHECK(cam.frames_emitted() == 200);
    CHECK(cam.frames_dropped() > 0);          // ~half the ticks dropped
    CHECK(gaps > 0);                          // gaps are actually visible
    CHECK(cam.frames_dropped() + cam.frames_emitted() == last);  // accounting closes
}

// Same seed + same faults → byte-for-byte identical stream (seqs and timestamps).
static void test_faults_are_deterministic() {
    const CamSim::Faults faults{0.3, 5000};
    CamSim a(8, 8, PixelFormat::GRAY8, /*seed=*/7, /*fps=*/30, faults);
    CamSim b(8, 8, PixelFormat::GRAY8, /*seed=*/7, /*fps=*/30, faults);
    for (int n = 0; n < 100; ++n) {
        Frame fa = a.next();
        Frame fb = b.next();
        CHECK(fa.seq == fb.seq);
        CHECK(fa.capture_ts == fb.capture_ts);   // jitter is deterministic too
    }
    CHECK(a.frames_dropped() == b.frames_dropped());
}

// Jitter stays within [0, max]: each frame's timestamp is its scheduled tick
// time plus an offset no larger than the configured bound.
static void test_jitter_is_bounded() {
    const uint64_t dt = 1'000'000'000ull / 30;
    const uint64_t max_jitter = 3000;
    CamSim cam(8, 8, PixelFormat::GRAY8, /*seed=*/1, /*fps=*/30, {/*drop=*/0.0, max_jitter});
    bool any_jitter = false;
    for (int n = 0; n < 100; ++n) {
        Frame f = cam.next();
        const uint64_t scheduled = f.seq * dt;   // no drops, so seq == tick
        CHECK(f.capture_ts >= scheduled);
        CHECK(f.capture_ts <= scheduled + max_jitter);
        if (f.capture_ts != scheduled) any_jitter = true;
    }
    CHECK(any_jitter);   // jitter actually happens
}

// Positive drift makes the camera's clock run AHEAD of the true schedule, and
// the lead grows over time (proportional to elapsed time, i.e. to seq).
static void test_drift_scales_timestamps() {
    const uint64_t dt  = 1'000'000'000ull / 30;
    const double   ppm = 1000.0;   // +1000 ppm = 0.1% fast
    CamSim cam(8, 8, PixelFormat::GRAY8, /*seed=*/1, /*fps=*/30,
               {/*drop=*/0.0, /*jitter=*/0, ppm});

    uint64_t prev_lead = 0;
    for (int n = 1; n <= 100; ++n) {
        Frame f = cam.next();                 // no drops → seq == tick == n
        const uint64_t truth = f.seq * dt;
        CHECK(f.capture_ts >= truth);          // running fast → ahead of truth
        const uint64_t lead = f.capture_ts - truth;
        CHECK(lead >= prev_lead);              // and the lead never shrinks
        prev_lead = lead;
    }
    CHECK(prev_lead > 0);                      // drift actually moved the clock
}

// Two cams with different drift diverge, and the gap grows with time — this is
// exactly the desync M2's aligner has to cope with.
static void test_relative_drift_diverges() {
    CamSim slow(8, 8, PixelFormat::GRAY8, /*seed=*/1, /*fps=*/30,
                {0.0, 0, /*drift=*/-500.0});   // 500 ppm slow
    CamSim fast(8, 8, PixelFormat::GRAY8, /*seed=*/1, /*fps=*/30,
                {0.0, 0, /*drift=*/+500.0});   // 500 ppm fast

    uint64_t gap_early = 0, gap_late = 0;
    for (int n = 1; n <= 200; ++n) {
        Frame a = slow.next();
        Frame b = fast.next();
        const uint64_t gap = b.capture_ts - a.capture_ts;  // fast leads slow
        if (n == 10)  gap_early = gap;
        if (n == 200) gap_late  = gap;
    }
    CHECK(gap_late > gap_early);   // divergence accumulates over time
}

int main() {
    struct { const char* name; void (*fn)(); } cases[] = {
        {"seq_and_timestamps",     test_seq_and_timestamps},
        {"size_per_format",        test_size_per_format},
        {"geometry_and_format",    test_geometry_and_format},
        {"content_shape",          test_content_shape},
        {"same_seed_identical",    test_same_seed_identical},
        {"different_seed_differs", test_different_seed_differs},
        {"no_faults_no_drops",     test_no_faults_no_drops},
        {"drop_rate_makes_gaps",   test_drop_rate_makes_gaps},
        {"faults_are_deterministic", test_faults_are_deterministic},
        {"jitter_is_bounded",      test_jitter_is_bounded},
        {"drift_scales_timestamps", test_drift_scales_timestamps},
        {"relative_drift_diverges", test_relative_drift_diverges},
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
