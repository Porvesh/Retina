// Single-threaded tests for LossyChannel (the M3 impairment shim).
//
// Drives the channel with an explicit logical clock. Pins each impairment in
// isolation — clean delivery, latency, drop statistics, jitter bound, jitter
// -> reorder, bandwidth serialization — plus seed determinism.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "retina/channel.hpp"

using namespace retina;

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("  FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static Packet mkpkt(uint64_t stream_seq, uint32_t payload_size = 100) {
    Packet p;
    p.stream_seq = stream_seq;
    p.frame_seq  = stream_seq;
    p.payload.resize(payload_size);
    return p;
}

// No impairment: every packet delivered, in order, nothing dropped or reordered.
static void test_no_impairment_delivers_all() {
    LossyChannel ch({/*drop=*/0.0, /*lat=*/0, /*jitter=*/0, /*bw=*/0}, /*seed=*/1);
    for (uint64_t n = 0; n < 10; ++n) ch.send(mkpkt(n), n);
    std::vector<Packet> got = ch.deliver(100);
    CHECK(got.size() == 10);
    for (uint64_t n = 0; n < 10; ++n) CHECK(got[n].stream_seq == n);
    CHECK(ch.dropped() == 0);
    CHECK(ch.reordered() == 0);
}

// Base latency shifts arrival: a packet isn't deliverable until send + latency.
static void test_latency_shifts_arrival() {
    LossyChannel ch({0.0, /*lat=*/1000, 0, 0}, /*seed=*/1);
    ch.send(mkpkt(0), /*send_ns=*/0);
    CHECK(ch.deliver(999).empty());        // not yet
    std::vector<Packet> got = ch.deliver(1000);
    CHECK(got.size() == 1);
}

// Drop probability: over many packets ~drop_prob are lost, and the books balance.
static void test_drop_rate_statistics() {
    LossyChannel ch({/*drop=*/0.5, 0, 0, 0}, /*seed=*/42);
    for (uint64_t n = 0; n < 1000; ++n) ch.send(mkpkt(n), n);
    ch.flush();
    CHECK(ch.sent() == 1000);
    CHECK(ch.dropped() > 300 && ch.dropped() < 700);      // ~half, seed-dependent
    CHECK(ch.dropped() + ch.delivered() == 1000);         // nothing vanishes
}

// Jitter stays within [base, base+jitter]: deliver just before the floor gets
// nothing; deliver at the ceiling gets the packet.
static void test_jitter_bounded() {
    LossyChannel ch({0.0, /*lat=*/1000, /*jitter=*/500, 0}, /*seed=*/7);
    ch.send(mkpkt(0), /*send_ns=*/0);
    CHECK(ch.deliver(999).empty());        // below base latency -> impossible
    std::vector<Packet> got = ch.deliver(1500);   // base+jitter ceiling
    CHECK(got.size() == 1);
}

// Jitter reorders: closely-spaced packets with large jitter arrive out of order.
static void test_jitter_causes_reorder() {
    LossyChannel ch({0.0, /*lat=*/0, /*jitter=*/10'000, 0}, /*seed=*/3);
    for (uint64_t n = 0; n < 200; ++n) ch.send(mkpkt(n), n * 100);   // 100 ns apart
    ch.flush();
    CHECK(ch.delivered() == 200);
    CHECK(ch.reordered() > 0);             // large jitter vs spacing -> reorder
}

// Bandwidth serializes a burst: with tx = size ns (bw chosen so), three packets
// sent at t=0 arrive at 1000/2000/3000, not all at once.
static void test_bandwidth_serializes() {
    // 8 Gbps -> size*8*1e9 / 8e9 = size ns per packet. 1000-byte packets -> 1000 ns.
    LossyChannel ch({0.0, /*lat=*/0, /*jitter=*/0, /*bw=*/8'000'000'000ull}, /*seed=*/1);
    for (uint64_t n = 0; n < 3; ++n) ch.send(mkpkt(n, /*payload=*/1000), /*send_ns=*/0);
    CHECK(ch.deliver(1000).size() == 1);   // only the first has clocked out
    CHECK(ch.deliver(3000).size() == 2);   // the other two by t=3000
    CHECK(ch.inflight() == 0);
}

// Same seed + same offered sequence -> identical drop/deliver outcome.
static void test_determinism() {
    auto run = [] {
        LossyChannel ch({/*drop=*/0.3, /*lat=*/100, /*jitter=*/500, 0}, /*seed=*/99);
        for (uint64_t n = 0; n < 500; ++n) ch.send(mkpkt(n), n * 10);
        std::vector<Packet> all = ch.flush();
        std::vector<uint64_t> seqs;
        for (auto& p : all) seqs.push_back(p.stream_seq);
        return std::pair<uint64_t, std::vector<uint64_t>>{ch.dropped(), seqs};
    };
    auto a = run();
    auto b = run();
    CHECK(a.first == b.first);     // same drop count
    CHECK(a.second == b.second);   // same delivery order
}

int main() {
    struct { const char* name; void (*fn)(); } cases[] = {
        {"no_impairment_delivers_all", test_no_impairment_delivers_all},
        {"latency_shifts_arrival",     test_latency_shifts_arrival},
        {"drop_rate_statistics",       test_drop_rate_statistics},
        {"jitter_bounded",             test_jitter_bounded},
        {"jitter_causes_reorder",      test_jitter_causes_reorder},
        {"bandwidth_serializes",       test_bandwidth_serializes},
        {"determinism",                test_determinism},
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
