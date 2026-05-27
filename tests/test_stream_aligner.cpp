// Single-threaded tests for StreamAligner (the M2 multi-stream time aligner).
//
// Same dependency-free harness as the other suites. Most cases use synthetic
// frames with hand-picked timestamps so the matching policy is pinned exactly;
// one integration case drives it from two drifting CamSims.

#include <cstdint>
#include <cstdio>
#include <optional>

#include "retina/camsim.hpp"
#include "retina/stream_aligner.hpp"

using namespace retina;

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("  FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

// A metadata-only frame — capture_ts + seq is all the aligner looks at.
static Frame mk(uint64_t seq, uint64_t ts) {
    Frame f;
    f.seq        = seq;
    f.capture_ts = ts;
    return f;
}

// Count how many tuples are currently drainable.
static int drain(StreamAligner& al) {
    int n = 0;
    while (al.pop()) ++n;
    return n;
}

// Perfectly synchronised streams -> every frame emits, zero skew, none rejected.
static void test_perfect_sync_all_emit() {
    StreamAligner al(2, /*epsilon=*/1000);
    for (uint64_t n = 1; n <= 5; ++n) {
        al.push(0, mk(n, n * 1000));
        al.push(1, mk(n, n * 1000));
    }
    al.flush();
    CHECK(al.emitted() == 5);
    CHECK(al.rejected() == 0);
    CHECK(drain(al) == 5);
}

// A skew under epsilon still aligns; the tuple reports the true spread.
static void test_within_epsilon_emits_with_skew() {
    StreamAligner al(2, /*epsilon=*/1000);
    al.push(0, mk(1, 10'000));
    al.push(1, mk(1, 10'300));   // 300 ns apart, well inside epsilon
    al.flush();
    CHECK(al.emitted() == 1);
    auto t = al.pop();
    CHECK(t.has_value());
    CHECK(t->skew_ns == 300);
    CHECK(t->ref_ts == 10'000);
    CHECK(t->frames[0].capture_ts == 10'000);
    CHECK(t->frames[1].capture_ts == 10'300);
}

// Growing drift eventually pushes the nearest neighbour past epsilon -> those
// reference frames are rejected. Accounting invariant: every reference frame is
// either emitted or rejected once flushed.
static void test_drift_beyond_epsilon_rejects() {
    StreamAligner al(2, /*epsilon=*/1000);
    const int N = 10;
    for (uint64_t n = 1; n <= (uint64_t)N; ++n) {
        al.push(0, mk(n, n * 10'000));                 // reference timeline
        al.push(1, mk(n, n * 10'000 + n * 300));       // stream 1 drifts +300/frame
    }
    al.flush();
    CHECK(al.emitted() > 0);                            // early frames still align
    CHECK(al.rejected() > 0);                           // late frames drift out
    CHECK(al.emitted() + al.rejected() == (uint64_t)N); // every ref decided exactly once
}

// A seq gap on a stream is counted as dropped trigger(s).
static void test_seq_gap_counts_dropped_triggers() {
    StreamAligner al(2, /*epsilon=*/1000);
    al.push(0, mk(1, 1000));
    al.push(0, mk(2, 2000));
    al.push(0, mk(4, 4000));   // seq 3 missing -> one dropped trigger
    al.push(0, mk(7, 7000));   // seq 5,6 missing -> two more
    CHECK(al.dropped_triggers(0) == 3);
    CHECK(al.dropped_triggers(1) == 0);
}

// Three streams align into 3-wide tuples.
static void test_three_streams() {
    StreamAligner al(3, /*epsilon=*/1000);
    for (uint64_t n = 1; n <= 4; ++n) {
        al.push(0, mk(n, n * 1000));
        al.push(1, mk(n, n * 1000 + 100));
        al.push(2, mk(n, n * 1000 - 100));
    }
    al.flush();
    CHECK(al.emitted() == 4);
    auto t = al.pop();
    CHECK(t.has_value());
    CHECK(t->frames.size() == 3);
    CHECK(t->skew_ns == 200);   // spread from -100 to +100
}

// The bracket gate holds an undecided reference until flush forces the call;
// with the matching stream already consumed, that trailing reference rejects.
static void test_flush_decides_trailing() {
    StreamAligner al(2, /*epsilon=*/1000);
    al.push(0, mk(1, 1000));
    al.push(1, mk(1, 1000));
    al.push(0, mk(2, 2000));
    al.push(1, mk(2, 2000));
    al.push(0, mk(3, 3000));   // no partner on stream 1 -> gated, still pending
    CHECK(al.emitted() == 2);  // first two decided online...
    CHECK(al.rejected() == 0); // ...and the third not yet
    al.flush();
    CHECK(al.emitted() == 2);
    CHECK(al.rejected() == 1); // third rejected at flush (stream 1 exhausted)
}

// pop() on an empty aligner yields nullopt.
static void test_empty_pop_is_nullopt() {
    StreamAligner al(2, /*epsilon=*/1000);
    CHECK(!al.pop().has_value());
}

// Integration: two CamSims with opposite drift. A generous epsilon aligns
// everything; a tight one rejects the frames whose drift exceeds it.
static void test_camsim_drift_integration() {
    auto run = [](uint64_t epsilon) {
        CamSim a(8, 8, PixelFormat::GRAY8, /*seed=*/1, /*fps=*/30, {0, 0, /*drift=*/-400});
        CamSim b(8, 8, PixelFormat::GRAY8, /*seed=*/1, /*fps=*/30, {0, 0, /*drift=*/+400});
        StreamAligner al(2, epsilon);
        for (int n = 0; n < 300; ++n) {
            al.push(0, a.next());
            al.push(1, b.next());
        }
        al.flush();
        return al;
    };

    // Drift scales ABSOLUTE time, so at frame n the gap is n * dt * 800 ppm.
    // With dt = 1e9/30 ns that is ~26.7 us/frame, reaching ~8 ms by frame 300.
    // A 10 ms epsilon covers the whole run; a 2 ms epsilon is overtaken partway
    // (around frame 75), so later frames reject.
    StreamAligner loose = run(10'000'000);
    CHECK(loose.emitted() == 300);
    CHECK(loose.rejected() == 0);

    StreamAligner tight = run(2'000'000);
    CHECK(tight.emitted() > 0);
    CHECK(tight.rejected() > 0);
    CHECK(tight.emitted() + tight.rejected() == 300);
}

// A stream that goes dark mid-run must NOT stall the aligner: with a horizon,
// the reference keeps advancing and its frames reject online (before flush), and
// the dead stream is observable via is_stalled / misses.
static void test_dead_stream_rejects_not_stalls() {
    StreamAligner al(2, /*epsilon=*/1000, /*max_wait=*/5000);
    al.push(0, mk(1, 1000));
    al.push(1, mk(1, 1000));            // one clean tuple, then stream 1 dies
    for (uint64_t n = 2; n <= 20; ++n)
        al.push(0, mk(n, n * 1000));    // stream 0 flows well past the horizon

    CHECK(al.emitted() == 1);
    CHECK(al.rejected() > 0);           // decided online — did NOT stall
    CHECK(al.is_stalled(1));            // stream 1 fell past the horizon
    CHECK(al.misses(1) > 0);            // and is blamed for the rejections
    CHECK(!al.is_stalled(0));           // the live reference is healthy
    al.flush();
    CHECK(al.emitted() + al.rejected() == 20);   // every reference decided
}

// A stream that never produces a single frame reads as fully stale, and its
// references reject rather than hang.
static void test_never_producing_stream() {
    StreamAligner al(2, /*epsilon=*/1000, /*max_wait=*/5000);
    for (uint64_t n = 1; n <= 10; ++n)
        al.push(0, mk(n, n * 1000));    // stream 1 silent throughout

    CHECK(al.emitted() == 0);
    CHECK(al.rejected() > 0);           // horizon forces rejections online
    CHECK(al.is_stalled(1));            // last_ts == 0 -> fully stale
    al.flush();
    CHECK(al.rejected() == 10);
}

// Contrast: with NO horizon (batch mode) the same dead stream stalls — nothing
// is decided online, and the stall isn't reported — until flush resolves it.
// This is why the horizon exists.
static void test_no_horizon_stalls_until_flush() {
    StreamAligner al(2, /*epsilon=*/1000);   // max_wait defaults to 0
    al.push(0, mk(1, 1000));
    al.push(1, mk(1, 1000));
    for (uint64_t n = 2; n <= 20; ++n)
        al.push(0, mk(n, n * 1000));

    CHECK(al.emitted() == 1);
    CHECK(al.rejected() == 0);          // waits forever — no online decisions
    CHECK(!al.is_stalled(1));           // no horizon -> stall not reported
    al.flush();                         // only flush breaks the stall
    CHECK(al.emitted() + al.rejected() == 20);
}

int main() {
    struct { const char* name; void (*fn)(); } cases[] = {
        {"perfect_sync_all_emit",        test_perfect_sync_all_emit},
        {"within_epsilon_emits_with_skew", test_within_epsilon_emits_with_skew},
        {"drift_beyond_epsilon_rejects", test_drift_beyond_epsilon_rejects},
        {"seq_gap_counts_dropped_triggers", test_seq_gap_counts_dropped_triggers},
        {"three_streams",                test_three_streams},
        {"flush_decides_trailing",       test_flush_decides_trailing},
        {"empty_pop_is_nullopt",         test_empty_pop_is_nullopt},
        {"dead_stream_rejects_not_stalls", test_dead_stream_rejects_not_stalls},
        {"never_producing_stream",       test_never_producing_stream},
        {"no_horizon_stalls_until_flush", test_no_horizon_stalls_until_flush},
        {"camsim_drift_integration",     test_camsim_drift_integration},
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
