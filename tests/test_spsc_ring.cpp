// Single-threaded tests for SpscRing.
//
// Same dependency-free harness as the LatestValue tests. These cover the FIFO
// contract and the drop-on-full policy; the threaded 1P/1C stress + TSan run
// comes next.

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>

#include "retina/buffers/spsc_ring.hpp"

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

static Frame make_frame(uint64_t seq, std::size_t n, uint8_t fill,
                        std::vector<uint8_t>& backing) {
    backing.assign(n, fill);
    Frame f;
    f.seq  = seq;
    f.data = backing.data();
    f.size = static_cast<uint32_t>(n);
    return f;
}

static void publish_seq(SpscRing& r, uint64_t seq) {
    std::vector<uint8_t> back;
    r.publish(make_frame(seq, 8, static_cast<uint8_t>(seq), back));
}

// Pop one frame; write its seq to *out (0 if empty) and confirm its bytes match
// the seq tag. Handle is released at the end of this scope.
static bool pop_seq(SpscRing& r, uint64_t* out) {
    FrameHandle h = r.next();
    if (!h.valid()) { *out = 0; return false; }
    *out = h.frame().seq;
    const uint8_t expect = static_cast<uint8_t>(h.frame().seq & 0xFF);
    for (uint32_t i = 0; i < h.frame().size; ++i)
        if (h.frame().data[i] != expect) return false;   // torn / wrong copy
    return true;
}

// ─── cases ───────────────────────────────────────────────────────────────────

static void test_empty_returns_invalid() {
    SpscRing r(4, 64);
    FrameHandle h = r.next();
    CHECK(!h.valid());
}

// Frames come out oldest-first, in publish order.
static void test_fifo_order() {
    SpscRing r(4, 64);
    publish_seq(r, 1);
    publish_seq(r, 2);
    publish_seq(r, 3);

    uint64_t s = 0;
    CHECK(pop_seq(r, &s) && s == 1);
    CHECK(pop_seq(r, &s) && s == 2);
    CHECK(pop_seq(r, &s) && s == 3);
    CHECK(!pop_seq(r, &s));   // drained
}

// When full, publish() drops the INCOMING frame (drop-not-resend): the queue
// keeps the frames it already had, the new one is lost.
static void test_drop_on_full() {
    SpscRing r(2, 64);   // depth 2
    publish_seq(r, 1);
    publish_seq(r, 2);
    publish_seq(r, 3);   // full → dropped

    uint64_t s = 0;
    CHECK(pop_seq(r, &s) && s == 1);
    CHECK(pop_seq(r, &s) && s == 2);
    CHECK(!pop_seq(r, &s));   // 3 never made it in
}

// After draining, the slots recycle: a fresh batch flows through in order.
// Run well past capacity to prove the cursors wrap cleanly and nothing leaks.
static void test_reuse_after_drain() {
    SpscRing r(3, 64);
    for (uint64_t base = 0; base < 1000; ++base) {
        const uint64_t seq = base + 1;
        publish_seq(r, seq);
        uint64_t s = 0;
        CHECK(pop_seq(r, &s) && s == seq);
    }
    uint64_t s = 0;
    CHECK(!pop_seq(r, &s));
}

// Interleaved produce/consume with a held frame counting toward occupancy.
static void test_interleaved_with_hold() {
    SpscRing r(2, 64);   // depth 2
    publish_seq(r, 1);

    FrameHandle h = r.next();          // hold seq 1 → occupies 1 slot, tail pinned
    CHECK(h.valid() && h.frame().seq == 1);

    publish_seq(r, 2);                 // ok: occ = 2 (held 1 + new 2) == full
    publish_seq(r, 3);                 // full → dropped

    // Nothing else can be popped while we still hold seq 1 (one-borrow rule),
    // so release first.
    { FrameHandle done = std::move(h); }  // release seq 1 → tail advances

    uint64_t s = 0;
    CHECK(pop_seq(r, &s) && s == 2);   // 2 survived
    CHECK(!pop_seq(r, &s));            // 3 was dropped
}

// Calling next() while still holding a frame is a usage error → throws.
static void test_one_borrow_at_a_time() {
    SpscRing r(4, 64);
    publish_seq(r, 1);
    publish_seq(r, 2);

    FrameHandle h = r.next();
    CHECK(h.valid());
    bool threw = false;
    try {
        FrameHandle h2 = r.next();     // still holding h → not allowed
        (void)h2;
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);
}

// The ring is a completeness buffer: latest() is the wrong verb and must throw
// (the base trait's "unsupported" default), not silently return something.
static void test_latest_is_unsupported() {
    SpscRing r(4, 64);
    publish_seq(r, 1);
    bool threw = false;
    try {
        FrameHandle h = r.latest();
        (void)h;
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);
}

int main() {
    struct { const char* name; void (*fn)(); } cases[] = {
        {"empty_returns_invalid",  test_empty_returns_invalid},
        {"fifo_order",             test_fifo_order},
        {"drop_on_full",           test_drop_on_full},
        {"reuse_after_drain",      test_reuse_after_drain},
        {"interleaved_with_hold",  test_interleaved_with_hold},
        {"one_borrow_at_a_time",   test_one_borrow_at_a_time},
        {"latest_is_unsupported",  test_latest_is_unsupported},
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
