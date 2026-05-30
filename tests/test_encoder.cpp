// Single-threaded tests for Encoder (the M3 GOP / frame-type model).
//
// Same dependency-free harness. These pin the GOP pattern, the I/P dependency
// chain (including across CamSim seq gaps), modelled sizes, determinism, and the
// keyframe-request hook the feedback channel will use.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "retina/sim/camsim.hpp"
#include "retina/net/encoder.hpp"

using namespace retina;

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("  FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static Frame mk(uint64_t seq, uint64_t ts = 0) {
    Frame f;
    f.seq        = seq;
    f.capture_ts = ts;
    return f;
}

// GOP cadence: I, then (gop-1) P's, repeating.
static void test_gop_pattern() {
    Encoder enc({/*gop=*/4, /*i_bytes=*/4000, /*p_bytes=*/400});
    const FrameType want[] = {
        FrameType::I, FrameType::P, FrameType::P, FrameType::P,
        FrameType::I, FrameType::P, FrameType::P, FrameType::P,
        FrameType::I,
    };
    for (uint64_t n = 0; n < 9; ++n)
        CHECK(enc.encode(mk(n + 1)).type == want[n]);
    CHECK(enc.i_count() == 3);
    CHECK(enc.p_count() == 6);
}

// I-frames carry no reference; each P depends on the previously-coded picture.
static void test_dependency_chain() {
    Encoder enc({/*gop=*/3, 4000, 400});
    EncodedFrame a = enc.encode(mk(1));   // I
    EncodedFrame b = enc.encode(mk(2));   // P -> 1
    EncodedFrame c = enc.encode(mk(3));   // P -> 2
    EncodedFrame d = enc.encode(mk(4));   // I (GOP wrap)
    CHECK(a.type == FrameType::I && a.depends_on == 0);
    CHECK(b.type == FrameType::P && b.depends_on == 1);
    CHECK(c.type == FrameType::P && c.depends_on == 2);
    CHECK(d.type == FrameType::I && d.depends_on == 0);
}

// Modelled sizes: I-frames big, P-frames small.
static void test_modelled_sizes() {
    Encoder enc({/*gop=*/4, /*i_bytes=*/4000, /*p_bytes=*/400});
    CHECK(enc.encode(mk(1)).bytes.size() == 4000);   // I
    CHECK(enc.encode(mk(2)).bytes.size() == 400);    // P
}

// The P dependency points at the previous CODED frame's seq even when CamSim
// dropped capture ticks (seq gaps) — the chain follows emission order.
static void test_dependency_survives_seq_gaps() {
    Encoder enc({/*gop=*/10, 4000, 400});
    EncodedFrame a = enc.encode(mk(1));    // I
    EncodedFrame b = enc.encode(mk(4));    // P (seqs 2,3 dropped upstream) -> 1
    EncodedFrame c = enc.encode(mk(9));    // P (5..8 dropped) -> 4
    CHECK(b.depends_on == 1);
    CHECK(c.depends_on == 4);
}

// Same source stream -> byte-identical encoded stream (determinism).
static void test_deterministic() {
    CamSim a(8, 8, PixelFormat::GRAY8, /*seed=*/3);
    CamSim b(8, 8, PixelFormat::GRAY8, /*seed=*/3);
    Encoder ea({/*gop=*/5, 4000, 400}), eb({/*gop=*/5, 4000, 400});
    for (int n = 0; n < 20; ++n) {
        EncodedFrame x = ea.encode(a.next());
        EncodedFrame y = eb.encode(b.next());
        CHECK(x.seq == y.seq && x.type == y.type && x.depends_on == y.depends_on);
        CHECK(x.bytes == y.bytes);
    }
}

// request_keyframe() forces the next frame to an I and restarts the GOP.
static void test_keyframe_request_forces_i() {
    Encoder enc({/*gop=*/10, 4000, 400});
    CHECK(enc.encode(mk(1)).type == FrameType::I);   // start
    CHECK(enc.encode(mk(2)).type == FrameType::P);
    enc.request_keyframe();
    EncodedFrame k = enc.encode(mk(3));
    CHECK(k.type == FrameType::I && k.depends_on == 0);
    // GOP restarted here: the next frame is a P off the forced keyframe.
    EncodedFrame p = enc.encode(mk(4));
    CHECK(p.type == FrameType::P && p.depends_on == 3);
}

int main() {
    struct { const char* name; void (*fn)(); } cases[] = {
        {"gop_pattern",                  test_gop_pattern},
        {"dependency_chain",             test_dependency_chain},
        {"modelled_sizes",               test_modelled_sizes},
        {"dependency_survives_seq_gaps", test_dependency_survives_seq_gaps},
        {"deterministic",                test_deterministic},
        {"keyframe_request_forces_i",    test_keyframe_request_forces_i},
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
