// Single-threaded tests for LatestValue.
//
// No external test framework yet — the core is header-only with no deps, so we
// keep the tests dependency-free too: a tiny CHECK macro, a main that runs each
// case and reports a count. CTest just needs the exit code (0 = pass).
//
// These cover *behavior*, not internals (refcounts are private). The slot-reuse
// and pinning cases are the interesting ones: they prove release_slot actually
// decrements (or the pool would exhaust) and that a held handle pins its slot
// against reuse. The multithreaded stress test comes later.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "retina/buffers/latest_value.hpp"

using namespace retina;

// ─── tiny harness ────────────────────────────────────────────────────────────
static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("  FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

// Build a Frame whose bytes are all `fill`, tagged with sequence `seq`.
// Returns {frame, backing-storage}; the caller keeps the storage alive.
static Frame make_frame(uint64_t seq, std::size_t n, uint8_t fill,
                        std::vector<uint8_t>& backing) {
    backing.assign(n, fill);
    Frame f;
    f.seq    = seq;
    f.data   = backing.data();
    f.size   = static_cast<uint32_t>(n);
    f.width  = 4;
    f.height = 4;
    f.format = PixelFormat::GRAY8;
    return f;
}

// ─── cases ───────────────────────────────────────────────────────────────────

// Nothing published → latest() hands back an empty handle.
static void test_empty_buffer() {
    LatestValue lv(1, 64);
    FrameHandle h = lv.latest();
    CHECK(!h.valid());
}

// publish → latest round-trips the metadata and the pixels, and the handle
// views a COPY (the slot's own storage), not the producer's buffer.
static void test_single_round_trip() {
    LatestValue lv(1, 64);
    std::vector<uint8_t> back;
    Frame in = make_frame(7, 16, 0xAB, back);
    lv.publish(in);

    FrameHandle h = lv.latest();
    CHECK(h.valid());
    CHECK(h.frame().seq == 7);
    CHECK(h.frame().size == 16);
    CHECK(h.frame().width == 4 && h.frame().height == 4);
    CHECK(h.frame().data != in.data);   // it's a copy, not the source buffer
    bool bytes_ok = true;
    for (uint32_t i = 0; i < h.frame().size; ++i)
        bytes_ok &= (h.frame().data[i] == 0xAB);
    CHECK(bytes_ok);
}

// latest() always reflects the most recently published frame.
static void test_newest_wins() {
    LatestValue lv(1, 64);
    std::vector<uint8_t> a, b;
    lv.publish(make_frame(1, 8, 0x11, a));
    lv.publish(make_frame(2, 8, 0x22, b));

    FrameHandle h = lv.latest();
    CHECK(h.valid());
    CHECK(h.frame().seq == 2);
    CHECK(h.frame().data[0] == 0x22);
}

// The copy is independent: mutating/destroying the producer's buffer after
// publish does not disturb what the handle sees.
static void test_copy_is_independent() {
    LatestValue lv(1, 64);
    FrameHandle h;
    {
        std::vector<uint8_t> back;
        lv.publish(make_frame(5, 8, 0xCD, back));
        h = lv.latest();
        // scribble over the source before it dies
        for (auto& x : back) x = 0x00;
    }  // `back` destroyed here
    CHECK(h.valid());
    CHECK(h.frame().seq == 5);
    CHECK(h.frame().data[0] == 0xCD);   // still the copy, untouched
}

// Slot recycling: publish+consume far more times than the pool holds. If
// release_slot didn't decrement, the pool (size num_consumers+2 == 3) would
// exhaust after a few frames and publish would start dropping — latest() would
// then return a stale seq and this loop would fail.
static void test_slot_recycling_no_leak() {
    LatestValue lv(1, 32);
    for (uint64_t i = 1; i <= 10000; ++i) {
        std::vector<uint8_t> back;
        lv.publish(make_frame(i, 8, static_cast<uint8_t>(i), back));
        FrameHandle h = lv.latest();
        CHECK(h.valid());
        if (!h.valid() || h.frame().seq != i) {
            std::printf("  (stopped at i=%llu, saw seq=%llu)\n",
                        (unsigned long long)i,
                        (unsigned long long)(h.valid() ? h.frame().seq : 0));
            ++g_failures;
            break;
        }
    }
}

// A held handle pins its slot: the producer must not overwrite a frame a
// consumer is still holding, even while newer frames are published.
static void test_held_handle_is_pinned() {
    // pool = num_consumers + 2 = 4
    LatestValue lv(2, 32);
    std::vector<uint8_t> a, b, c, d;

    lv.publish(make_frame(100, 8, 0x01, a));
    FrameHandle h1 = lv.latest();          // pins the slot holding seq 100
    CHECK(h1.valid() && h1.frame().seq == 100);

    lv.publish(make_frame(101, 8, 0x02, b));
    FrameHandle h2 = lv.latest();          // pins seq 101 (currently newest)
    CHECK(h2.valid() && h2.frame().seq == 101);

    // Two more publishes must still find free slots (4 - 2 pinned = 2 free),
    // and must NOT clobber the slots h1/h2 are holding.
    lv.publish(make_frame(102, 8, 0x03, c));
    lv.publish(make_frame(103, 8, 0x04, d));

    FrameHandle h3 = lv.latest();
    CHECK(h3.valid() && h3.frame().seq == 103);

    // The pinned frames are still intact.
    CHECK(h1.frame().seq == 100 && h1.frame().data[0] == 0x01);
    CHECK(h2.frame().seq == 101 && h2.frame().data[0] == 0x02);
}

// Releasing a handle frees its slot for reuse (RAII path). After h1 above is
// gone the pool is fully available again; a fresh run of the recycling loop on
// the same buffer should sail through.
static void test_release_frees_slot() {
    LatestValue lv(1, 32);   // pool = 3
    {
        std::vector<uint8_t> a;
        lv.publish(make_frame(1, 8, 0x01, a));
        FrameHandle h = lv.latest();
        CHECK(h.valid());
    }  // h released here → slot returned
    // Churn well past pool size; only works if the released slot came back.
    for (uint64_t i = 2; i <= 100; ++i) {
        std::vector<uint8_t> back;
        lv.publish(make_frame(i, 8, static_cast<uint8_t>(i), back));
        FrameHandle h = lv.latest();
        CHECK(h.valid() && h.frame().seq == i);
    }
}

int main() {
    struct { const char* name; void (*fn)(); } cases[] = {
        {"empty_buffer",           test_empty_buffer},
        {"single_round_trip",      test_single_round_trip},
        {"newest_wins",            test_newest_wins},
        {"copy_is_independent",    test_copy_is_independent},
        {"slot_recycling_no_leak", test_slot_recycling_no_leak},
        {"held_handle_is_pinned",  test_held_handle_is_pinned},
        {"release_frees_slot",     test_release_frees_slot},
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
