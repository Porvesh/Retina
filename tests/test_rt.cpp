// Single-threaded tests for the RT layer (rt::configure) and JitterMeter.
//
// configure() must run and report cleanly on any platform (the RT levers are
// Linux-only, no-ops elsewhere). JitterMeter must measure period deviation:
// zero under perfect timing, and the right max/mean/late counts under jitter.

#include <cstdio>

#include "retina/rt/rt.hpp"

using namespace retina;

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("  FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

// configure() returns a coherent report and matches the compile target.
static void test_configure_reports() {
    rt::Report r = rt::configure({/*prio=*/80, /*cpu=*/-1, /*lock=*/false});
#if defined(__linux__)
    CHECK(r.platform_linux);
#else
    CHECK(!r.platform_linux);                 // Linux-only levers are no-ops here
#endif
    CHECK(!r.realtime_msg.empty());           // always explains what happened
    CHECK(!r.affinity_msg.empty());
}

// Perfect periodic timing -> zero jitter, nothing late.
static void test_perfect_timing_zero_jitter() {
    rt::JitterMeter m(/*target=*/1000);
    for (uint64_t i = 0; i < 100; ++i) m.record(i * 1000);
    CHECK(m.samples() == 99);
    CHECK(m.max_ns() == 0);
    CHECK(m.mean_ns() == 0.0);
    CHECK(m.late() == 0);
}

// Known jittered arrivals produce the expected max/mean/late.
static void test_jitter_stats() {
    rt::JitterMeter m(/*target=*/1000);
    // deltas: 1200(+200,late) 800(-200) 1300(+300,late) 700(-300)
    const uint64_t ts[] = {0, 1200, 2000, 3300, 4000};
    for (uint64_t t : ts) m.record(t);
    CHECK(m.samples() == 4);
    CHECK(m.late() == 2);
    CHECK(m.max_ns() == 300);
    CHECK(m.mean_ns() == 250.0);      // (200+200+300+300)/4
    CHECK(m.bucket(0) == 4);          // all under 10 us
}

// No crash with too few samples.
static void test_insufficient_samples() {
    rt::JitterMeter m(1000);
    CHECK(m.samples() == 0);
    m.record(5);
    CHECK(m.samples() == 0);          // needs a pair to form an interval
    CHECK(m.max_ns() == 0);
}

int main() {
    struct { const char* name; void (*fn)(); } cases[] = {
        {"configure_reports",          test_configure_reports},
        {"perfect_timing_zero_jitter", test_perfect_timing_zero_jitter},
        {"jitter_stats",               test_jitter_stats},
        {"insufficient_samples",       test_insufficient_samples},
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
