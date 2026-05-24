// Single-threaded tests for DoubleBuf and TripleBuf (the freshness buffers).
//
// Same dependency-free harness as the other suites. The interesting cases are
// the ones that distinguish the two: under a held frame, TripleBuf keeps taking
// new frames and hands back the ABSOLUTE newest, while DoubleBuf drops the
// incoming frame once its spare is occupied.

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>

#include "retina/double_buffer.hpp"
#include "retina/triple_buffer.hpp"

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

template <class Buf>
static void publish_seq(Buf& b, uint64_t seq) {
    std::vector<uint8_t> back;
    b.publish(make_frame(seq, 8, static_cast<uint8_t>(seq), back));
}

// ─── DoubleBuf ───────────────────────────────────────────────────────────────

static void test_double_empty() {
    DoubleBuf b(64);
    CHECK(!b.latest().valid());
}

static void test_double_round_trip() {
    DoubleBuf b(64);
    publish_seq(b, 42);
    FrameHandle h = b.latest();
    CHECK(h.valid());
    CHECK(h.frame().seq == 42);
    CHECK(h.frame().data[0] == 42);
}

static void test_double_newest_wins() {
    DoubleBuf b(64);
    publish_seq(b, 1);
    publish_seq(b, 2);
    FrameHandle h = b.latest();
    CHECK(h.valid() && h.frame().seq == 2);
}

// The defining behavior: while the consumer holds the newest, the producer can
// place exactly ONE more frame (into the spare); a second publish has nowhere to
// go and is dropped. On release the consumer sees the one that got in, not the
// dropped one.
static void test_double_drops_when_spare_taken() {
    DoubleBuf b(64);
    publish_seq(b, 1);
    FrameHandle h = b.latest();          // holds seq 1 (newest)
    CHECK(h.valid() && h.frame().seq == 1);

    publish_seq(b, 2);                   // goes into the spare slot — ok
    publish_seq(b, 3);                   // no free slot (seq1 held) → DROPPED

    { FrameHandle drop = std::move(h); } // release seq 1

    FrameHandle h2 = b.latest();
    CHECK(h2.valid() && h2.frame().seq == 2);   // 2 made it, 3 was dropped
}

static void test_double_rejects_next() {
    DoubleBuf b(64);
    publish_seq(b, 1);
    bool threw = false;
    try { (void)b.next(); } catch (const std::logic_error&) { threw = true; }
    CHECK(threw);
}

// ─── TripleBuf ───────────────────────────────────────────────────────────────

static void test_triple_empty() {
    TripleBuf b(64);
    CHECK(!b.latest().valid());
}

static void test_triple_round_trip() {
    TripleBuf b(64);
    publish_seq(b, 42);
    FrameHandle h = b.latest();
    CHECK(h.valid());
    CHECK(h.frame().seq == 42);
    CHECK(h.frame().data[0] == 42);
}

static void test_triple_newest_wins() {
    TripleBuf b(64);
    publish_seq(b, 1);
    publish_seq(b, 2);
    publish_seq(b, 3);
    FrameHandle h = b.latest();
    CHECK(h.valid() && h.frame().seq == 3);
}

// The defining contrast with DoubleBuf: the producer keeps accepting frames even
// while the consumer holds one, and on release the consumer gets the ABSOLUTE
// newest — the third buffer never forces a dropped-incoming frame.
static void test_triple_stays_fresh_under_hold() {
    TripleBuf b(64);
    publish_seq(b, 1);
    FrameHandle h = b.latest();          // holds seq 1
    CHECK(h.valid() && h.frame().seq == 1);

    publish_seq(b, 2);                   // all accepted — producer always has
    publish_seq(b, 3);                   //   its private in-progress buffer
    publish_seq(b, 4);

    { FrameHandle drop = std::move(h); } // release seq 1

    FrameHandle h2 = b.latest();
    CHECK(h2.valid() && h2.frame().seq == 4);   // absolute newest, not a stale 2
}

// latest() with no new publish returns the last frame again (freshness = newest
// available, not new-since-last-call).
static void test_triple_repeat_returns_last() {
    TripleBuf b(64);
    publish_seq(b, 7);
    { FrameHandle h = b.latest(); CHECK(h.valid() && h.frame().seq == 7); }
    { FrameHandle h = b.latest(); CHECK(h.valid() && h.frame().seq == 7); }
}

static void test_triple_one_borrow() {
    TripleBuf b(64);
    publish_seq(b, 1);
    FrameHandle h = b.latest();
    bool threw = false;
    try { (void)b.latest(); } catch (const std::logic_error&) { threw = true; }
    CHECK(threw);
}

static void test_triple_rejects_next() {
    TripleBuf b(64);
    publish_seq(b, 1);
    bool threw = false;
    try { (void)b.next(); } catch (const std::logic_error&) { threw = true; }
    CHECK(threw);
}

int main() {
    struct { const char* name; void (*fn)(); } cases[] = {
        {"double_empty",                  test_double_empty},
        {"double_round_trip",             test_double_round_trip},
        {"double_newest_wins",            test_double_newest_wins},
        {"double_drops_when_spare_taken", test_double_drops_when_spare_taken},
        {"double_rejects_next",           test_double_rejects_next},
        {"triple_empty",                  test_triple_empty},
        {"triple_round_trip",             test_triple_round_trip},
        {"triple_newest_wins",            test_triple_newest_wins},
        {"triple_stays_fresh_under_hold", test_triple_stays_fresh_under_hold},
        {"triple_repeat_returns_last",    test_triple_repeat_returns_last},
        {"triple_one_borrow",             test_triple_one_borrow},
        {"triple_rejects_next",           test_triple_rejects_next},
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
