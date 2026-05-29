// Single-threaded tests for the M3 feedback loop (KeyframeRequester +
// FeedbackLink), ending with a closed-loop recovery: a lost I-frame breaks the
// stream, a keyframe request travels back, and decoding heals at the forced I.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "retina/decoder.hpp"
#include "retina/encoder.hpp"
#include "retina/feedback.hpp"

using namespace retina;

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("  FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static Decoder::Result res(bool decodable, FrameType type) {
    Decoder::Result r;
    r.decodable = decodable;
    r.type      = type;
    return r;
}

// One request per broken stretch: undecodable frames after the first don't
// re-fire until a decodable frame heals the stream.
static void test_requester_fires_once_until_healed() {
    KeyframeRequester req;
    CHECK(req.observe(res(false, FrameType::P), 100));   // first break -> fire
    CHECK(!req.observe(res(false, FrameType::P), 200));  // still broken -> quiet
    CHECK(!req.observe(res(false, FrameType::P), 300));
    CHECK(req.requests() == 1);
    CHECK(!req.observe(res(true, FrameType::I), 400));   // healed
    CHECK(req.observe(res(false, FrameType::P), 500));   // new break -> fire again
    CHECK(req.requests() == 2);
}

// A re-request timeout re-fires if the keyframe hasn't come (request may be lost).
static void test_requester_rerequest_on_timeout() {
    KeyframeRequester req({/*rerequest_timeout_ns=*/1000});
    CHECK(req.observe(res(false, FrameType::P), 0));        // fire at 0
    CHECK(!req.observe(res(false, FrameType::P), 999));     // within timeout
    CHECK(req.observe(res(false, FrameType::P), 1000));     // timeout -> re-fire
    CHECK(req.requests() == 2);
}

// Decodable frames never trigger a request.
static void test_requester_ignores_decodable() {
    KeyframeRequester req;
    for (uint64_t n = 0; n < 10; ++n)
        CHECK(!req.observe(res(true, FrameType::P), n * 100));
    CHECK(req.requests() == 0);
}

// The return path delays a request by its one-way delay.
static void test_feedback_link_delay() {
    FeedbackLink link(/*delay=*/1000);
    link.send(/*token=*/42, /*now=*/0);
    CHECK(link.deliver(999).empty());
    CHECK(link.deliver(1000).size() == 1);
}

// Feedback can be lost too; the books balance.
static void test_feedback_link_drop() {
    FeedbackLink link(/*delay=*/100, /*drop_prob=*/0.5, /*seed=*/7);
    for (uint64_t n = 0; n < 200; ++n) link.send(n, n);
    std::vector<uint64_t> got = link.deliver(UINT64_MAX);
    CHECK(link.dropped() > 0);
    CHECK(link.sent() == 200);
    CHECK(link.dropped() + got.size() == 200);
}

// Closed loop: the initial I-frame is lost, so P-frames are undecodable until a
// keyframe request completes a round trip and the sender inserts an I-frame.
static void test_closed_loop_recovery() {
    Encoder enc({/*gop=*/1000, /*i_bytes=*/4000, /*p_bytes=*/400});  // one natural I
    KeyframeRequester req;
    FeedbackLink      back(/*delay=*/3000);   // one-way feedback delay
    Decoder           dec;

    const uint64_t interval = 1000;   // ns between frames
    const uint64_t fwd      = 500;    // forward one-way delay

    bool     healed = false;
    uint64_t heal_seq = 0;

    for (uint64_t n = 1; n <= 40; ++n) {
        const uint64_t send_ns = n * interval;

        // Sender: apply any keyframe requests that have arrived by now.
        for (auto tok : back.deliver(send_ns)) { (void)tok; enc.request_keyframe(); }

        Frame f; f.seq = n; f.capture_ts = send_ns;
        EncodedFrame ef = enc.encode(f);

        if (n == 1) continue;         // the initial I-frame is lost on the link

        const uint64_t recv_ns = send_ns + fwd;
        Decoder::Result r = dec.decode(ef, recv_ns);
        if (!healed && r.decodable && r.type == FrameType::I) {
            healed   = true;
            heal_seq = r.seq;
        }
        // Receiver: request a keyframe on unrecoverable breakage.
        if (req.observe(r, recv_ns)) back.send(recv_ns, recv_ns);
    }

    CHECK(healed);                       // the stream recovered
    CHECK(req.requests() == 1);          // exactly one request (debounced)
    CHECK(dec.undecodable() > 0);        // there was a real broken stretch
    CHECK(dec.decoded() > 0);            // and healthy frames after healing
    // Request sent at frame 2 (recv 2500), arrives 5500, next send tick is n=6.
    CHECK(heal_seq == 6);
}

int main() {
    struct { const char* name; void (*fn)(); } cases[] = {
        {"requester_fires_once_until_healed", test_requester_fires_once_until_healed},
        {"requester_rerequest_on_timeout",    test_requester_rerequest_on_timeout},
        {"requester_ignores_decodable",       test_requester_ignores_decodable},
        {"feedback_link_delay",               test_feedback_link_delay},
        {"feedback_link_drop",                test_feedback_link_drop},
        {"closed_loop_recovery",              test_closed_loop_recovery},
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
