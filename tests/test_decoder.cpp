// Single-threaded tests for Decoder (the M3 receiver dependency model).
//
// Pins the I/P decodability rules and the headline M3-D fact: a lost I-frame
// orphans its whole GOP, while a lost P orphans only the frames that chain off
// it — until the next keyframe. Ends with an end-to-end pass through the pieces
// built so far (Encoder -> Packetizer -> drop -> Reassembler -> Decoder).

#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

#include "retina/decoder.hpp"
#include "retina/encoder.hpp"
#include "retina/packet.hpp"

using namespace retina;

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("  FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static EncodedFrame ef(uint64_t seq, FrameType type, uint64_t depends_on,
                       uint64_t capture_ts = 0) {
    EncodedFrame e;
    e.seq        = seq;
    e.type       = type;
    e.depends_on = depends_on;
    e.capture_ts = capture_ts;
    return e;
}

// An I-frame decodes on its own; the P's that chain off it decode too.
static void test_gop_decodes_clean() {
    Decoder dec;
    CHECK(dec.decode(ef(1, FrameType::I, 0)).decodable);
    CHECK(dec.decode(ef(2, FrameType::P, 1)).decodable);
    CHECK(dec.decode(ef(3, FrameType::P, 2)).decodable);
    CHECK(dec.decode(ef(4, FrameType::P, 3)).decodable);
    CHECK(dec.decoded() == 4);
    CHECK(dec.undecodable() == 0);
}

// The headline: a lost I-frame (never offered) orphans every P in its GOP, and
// the stream heals only at the next I.
static void test_lost_i_kills_gop() {
    Decoder dec;
    // frame 1 (I) LOST — never decoded.
    CHECK(!dec.decode(ef(2, FrameType::P, 1)).decodable);   // dep 1 missing
    CHECK(!dec.decode(ef(3, FrameType::P, 2)).decodable);   // dep 2 not decoded
    CHECK(!dec.decode(ef(4, FrameType::P, 3)).decodable);   // dep 3 not decoded
    CHECK(dec.decode(ef(5, FrameType::I, 0)).decodable);    // keyframe heals
    CHECK(dec.decode(ef(6, FrameType::P, 5)).decodable);
    CHECK(dec.undecodable() == 3);
    CHECK(dec.decoded() == 2);
}

// A lost P orphans only the frames after it in the GOP, not the I before it.
static void test_lost_p_kills_rest_of_gop() {
    Decoder dec;
    CHECK(dec.decode(ef(1, FrameType::I, 0)).decodable);    // I ok
    // frame 2 (P) LOST.
    CHECK(!dec.decode(ef(3, FrameType::P, 2)).decodable);   // dep 2 missing
    CHECK(!dec.decode(ef(4, FrameType::P, 3)).decodable);   // dep 3 not decoded
    CHECK(dec.decode(ef(5, FrameType::I, 0)).decodable);    // next I heals
    CHECK(dec.decoded() == 2);        // I1, I5
    CHECK(dec.undecodable() == 2);    // P3, P4
}

// Glass-to-glass latency = now - capture_ts, aggregated over decodable frames.
static void test_latency_measured() {
    Decoder dec;
    dec.decode(ef(1, FrameType::I, 0, /*capture_ts=*/1000), /*now=*/1500);   // 500
    dec.decode(ef(2, FrameType::P, 1, /*capture_ts=*/2000), /*now=*/2900);   // 900
    CHECK(dec.latency_count() == 2);
    CHECK(dec.latency_min() == 500);
    CHECK(dec.latency_max() == 900);
    CHECK(dec.latency_mean() == 700.0);
}

// End-to-end: encode a GOP, packetize, DROP every packet of the first I-frame,
// reassemble the rest, decode in order -> the whole first GOP is orphaned.
static void test_pipeline_lost_i_frame() {
    Encoder enc({/*gop=*/4, /*i_bytes=*/4000, /*p_bytes=*/400});
    Packetizer pk(/*mtu=*/1200);
    Reassembler re;
    Decoder dec;

    for (uint64_t n = 1; n <= 8; ++n) {   // I1 P2 P3 P4 I5 P6 P7 P8
        Frame f; f.seq = n; f.capture_ts = n * 1000;
        EncodedFrame in = enc.encode(f);
        for (auto& p : pk.packetize(in)) {
            if (in.seq == 1) continue;    // first I-frame fully lost on the link
            if (auto out = re.offer(p)) dec.decode(*out, out->capture_ts + 500);
        }
    }
    CHECK(dec.undecodable() == 3);   // P2, P3, P4 orphaned by the lost I1
    CHECK(dec.decoded() == 4);       // I5, P6, P7, P8 heal and decode
}

int main() {
    struct { const char* name; void (*fn)(); } cases[] = {
        {"gop_decodes_clean",         test_gop_decodes_clean},
        {"lost_i_kills_gop",          test_lost_i_kills_gop},
        {"lost_p_kills_rest_of_gop",  test_lost_p_kills_rest_of_gop},
        {"latency_measured",          test_latency_measured},
        {"pipeline_lost_i_frame",     test_pipeline_lost_i_frame},
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
