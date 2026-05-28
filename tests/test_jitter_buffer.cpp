// Single-threaded tests for JitterBuffer (the M3 adaptive playout buffer).
//
// Pins reordering, the hold-then-release delay, stutter accounting on a gap,
// late-packet drop, and the adaptive delay tracking measured jitter. Ends with
// a channel integration proving jittery/reordered arrivals come out in order.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "retina/channel.hpp"
#include "retina/jitter_buffer.hpp"

using namespace retina;

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("  FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static Packet mkpkt(uint64_t stream_seq, uint64_t capture_ts = 0) {
    Packet p;
    p.stream_seq = stream_seq;
    p.frame_seq  = stream_seq;
    p.capture_ts = capture_ts;
    return p;
}

// Out-of-order arrivals, once ripe, come out in stream_seq order with no stutter.
static void test_reorders_in_order() {
    JitterBuffer jb({/*base=*/100, UINT64_MAX, /*gain=*/0.0, /*adaptive=*/false});
    jb.push(mkpkt(2), 0);
    jb.push(mkpkt(0), 0);
    jb.push(mkpkt(3), 0);
    jb.push(mkpkt(1), 0);
    std::vector<Packet> out = jb.pop(100);   // all held 100 ns, now ripe
    CHECK(out.size() == 4);
    for (uint64_t n = 0; n < 4; ++n) CHECK(out[n].stream_seq == n);
    CHECK(jb.stutters() == 0);
}

// The delay holds a packet until base_delay has elapsed since its arrival.
static void test_delay_holds_then_releases() {
    JitterBuffer jb({/*base=*/1000, UINT64_MAX, 0.0, /*adaptive=*/false});
    jb.push(mkpkt(0), /*arrival=*/0);
    CHECK(jb.pop(999).empty());
    CHECK(jb.pop(1000).size() == 1);
}

// A missing sequence that never arrives becomes a stutter when the buffer jumps
// the gap; a copy arriving after that is too late and is dropped, not recounted.
static void test_gap_is_stutter_and_late_dropped() {
    JitterBuffer jb({/*base=*/100, UINT64_MAX, 0.0, /*adaptive=*/false});
    jb.push(mkpkt(0), 0);
    jb.push(mkpkt(2), 0);          // seq 1 missing
    std::vector<Packet> out = jb.pop(200);
    CHECK(out.size() == 2);        // 0 and 2 released
    CHECK(out[0].stream_seq == 0 && out[1].stream_seq == 2);
    CHECK(jb.stutters() == 1);     // the skipped seq 1

    jb.push(mkpkt(1), 300);        // arrives after its slot played out
    CHECK(jb.pop(500).empty());    // dropped, not replayed
    CHECK(jb.stutters() == 1);     // not double-counted
    CHECK(jb.dropped_late() == 1);
}

// Adaptive delay grows on a jittery stream and stays tight on a calm one.
static void test_adaptive_delay_tracks_jitter() {
    JitterBuffer calm({/*base=*/0, UINT64_MAX, /*gain=*/4.0, /*adaptive=*/true});
    JitterBuffer jittery({/*base=*/0, UINT64_MAX, /*gain=*/4.0, /*adaptive=*/true});
    for (uint64_t n = 0; n < 50; ++n) {
        const uint64_t cap = n * 1000;
        calm.push(mkpkt(n, cap), cap + 500);                          // steady transit
        jittery.push(mkpkt(n, cap), cap + 500 + (n % 2 ? 900 : 0));   // alternating
    }
    CHECK(calm.jitter_estimate_ns() < jittery.jitter_estimate_ns());
    CHECK(jittery.current_delay_ns() > calm.current_delay_ns());
}

// With adaptive off, the delay never moves regardless of jitter.
static void test_non_adaptive_fixed_delay() {
    JitterBuffer jb({/*base=*/750, UINT64_MAX, 4.0, /*adaptive=*/false});
    for (uint64_t n = 0; n < 20; ++n)
        jb.push(mkpkt(n, n * 1000), n * 1000 + (n % 3) * 1234);
    CHECK(jb.current_delay_ns() == 750);
}

// Integration: a jittery, reordering channel feeds the buffer; what the buffer
// releases is monotonic in stream_seq (in order for the reassembler/decoder).
static void test_channel_integration_reorders() {
    LossyChannel ch({/*drop=*/0.0, /*lat=*/1000, /*jitter=*/5000, /*bw=*/0}, /*seed=*/5);
    JitterBuffer jb({/*base=*/6000, UINT64_MAX, 4.0, /*adaptive=*/true});

    const uint64_t step = 100;   // packets sent 100 ns apart
    for (uint64_t n = 0; n < 300; ++n) ch.send(mkpkt(n, n * step), n * step);

    // Drive both clocks forward together, deliver -> buffer -> playout.
    std::vector<uint64_t> played;
    for (uint64_t now = 0; now <= 300 * step + 20000; now += step) {
        for (auto& p : ch.deliver(now)) jb.push(p, now);
        for (auto& p : jb.pop(now)) played.push_back(p.stream_seq);
    }
    for (auto& p : jb.flush()) played.push_back(p.stream_seq);

    CHECK(played.size() == 300);
    bool monotonic = true;
    for (std::size_t i = 1; i < played.size(); ++i)
        if (played[i] <= played[i - 1]) monotonic = false;
    CHECK(monotonic);                    // channel reordered; buffer fixed it
    CHECK(ch.reordered() > 0);           // there really was reordering to fix
}

int main() {
    struct { const char* name; void (*fn)(); } cases[] = {
        {"reorders_in_order",              test_reorders_in_order},
        {"delay_holds_then_releases",      test_delay_holds_then_releases},
        {"gap_is_stutter_and_late_dropped", test_gap_is_stutter_and_late_dropped},
        {"adaptive_delay_tracks_jitter",   test_adaptive_delay_tracks_jitter},
        {"non_adaptive_fixed_delay",       test_non_adaptive_fixed_delay},
        {"channel_integration_reorders",   test_channel_integration_reorders},
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
