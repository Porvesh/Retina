// Single-threaded tests for the RTP-style framing (Packetizer + Reassembler).
//
// Pins fragmentation math, order-independent reassembly, duplicate tolerance,
// the missing-fragment hole (the loss failure mode), and metadata round-trip.

#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

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

// Build an EncodedFrame with a deterministic gradient payload of `size` bytes.
static EncodedFrame mkef(uint64_t seq, FrameType type, uint64_t depends_on,
                         uint32_t size) {
    EncodedFrame ef;
    ef.seq        = seq;
    ef.capture_ts = seq * 1000;
    ef.type       = type;
    ef.depends_on = depends_on;
    ef.bytes.resize(size);
    for (uint32_t i = 0; i < size; ++i)
        ef.bytes[i] = static_cast<uint8_t>((seq + i) & 0xFF);
    return ef;
}

// A frame smaller than the MTU is a single marked packet, round-tripping whole.
static void test_single_packet_frame() {
    Packetizer pk(/*mtu=*/1200);
    EncodedFrame in = mkef(1, FrameType::P, 0, 400);
    std::vector<Packet> pkts = pk.packetize(in);
    CHECK(pkts.size() == 1);
    CHECK(pkts[0].frag_count == 1);
    CHECK(pkts[0].marker == true);

    Reassembler re;
    auto out = re.offer(pkts[0]);
    CHECK(out.has_value());
    CHECK(out->bytes == in.bytes);
}

// A frame larger than the MTU splits into ceil(size/mtu) packets; only the last
// carries the marker; reassembly in order rebuilds the exact bytes.
static void test_multi_packet_frame() {
    Packetizer pk(/*mtu=*/1200);
    EncodedFrame in = mkef(7, FrameType::I, 0, 4000);   // -> 1200,1200,1200,400
    std::vector<Packet> pkts = pk.packetize(in);
    CHECK(pkts.size() == 4);
    CHECK(pkts[0].payload.size() == 1200);
    CHECK(pkts[3].payload.size() == 400);
    CHECK(pkts[0].marker == false);
    CHECK(pkts[3].marker == true);

    Reassembler re;
    std::optional<EncodedFrame> out;
    for (auto& p : pkts) out = re.offer(p);   // last offer completes it
    CHECK(out.has_value());
    CHECK(out->bytes == in.bytes);
    CHECK(out->bytes.size() == 4000);
}

// Fragments arriving out of order still reassemble correctly (only the final
// missing fragment triggers completion, whichever order it arrives in).
static void test_reassemble_out_of_order() {
    Packetizer pk(/*mtu=*/1000);
    EncodedFrame in = mkef(3, FrameType::I, 0, 3500);   // 4 packets
    std::vector<Packet> pkts = pk.packetize(in);

    Reassembler re;
    std::optional<EncodedFrame> out;
    for (auto it = pkts.rbegin(); it != pkts.rend(); ++it)   // reverse order
        out = re.offer(*it);
    CHECK(out.has_value());
    CHECK(out->bytes == in.bytes);
}

// A missing fragment leaves a permanent hole: the frame never completes and
// stays pending. This is exactly the loss FEC must repair.
static void test_missing_fragment_incomplete() {
    Packetizer pk(/*mtu=*/1000);
    EncodedFrame in = mkef(5, FrameType::I, 0, 3500);   // 4 packets
    std::vector<Packet> pkts = pk.packetize(in);

    Reassembler re;
    for (std::size_t i = 0; i < pkts.size(); ++i)
        if (i != 2) CHECK(!re.offer(pkts[i]).has_value());   // drop fragment 2
    CHECK(re.completed() == 0);
    CHECK(re.is_pending(5));
    CHECK(re.pending() == 1);
}

// A duplicated fragment is ignored: the frame completes exactly once.
static void test_duplicate_fragment_ignored() {
    Packetizer pk(/*mtu=*/1000);
    EncodedFrame in = mkef(9, FrameType::I, 0, 2500);   // 3 packets
    std::vector<Packet> pkts = pk.packetize(in);

    Reassembler re;
    CHECK(!re.offer(pkts[0]).has_value());
    CHECK(!re.offer(pkts[0]).has_value());   // duplicate of fragment 0
    CHECK(!re.offer(pkts[1]).has_value());
    auto out = re.offer(pkts[2]);
    CHECK(out.has_value());
    CHECK(out->bytes == in.bytes);
    CHECK(re.completed() == 1);
}

// The stream-wide packet sequence is contiguous across successive frames — that
// monotonic numbering is what the receiver uses to detect loss and reorder.
static void test_stream_seq_monotonic_across_frames() {
    Packetizer pk(/*mtu=*/1000);
    std::vector<Packet> a = pk.packetize(mkef(1, FrameType::I, 0, 2500));  // 3
    std::vector<Packet> b = pk.packetize(mkef(2, FrameType::P, 1, 500));   // 1
    uint64_t expect = 0;
    for (auto& p : a) CHECK(p.stream_seq == expect++);
    for (auto& p : b) CHECK(p.stream_seq == expect++);
    CHECK(pk.packets_sent() == 4);
}

// Frame-level metadata survives the fragment round-trip.
static void test_metadata_preserved() {
    Packetizer pk(/*mtu=*/1000);
    EncodedFrame in = mkef(42, FrameType::P, 41, 2500);
    std::vector<Packet> pkts = pk.packetize(in);
    Reassembler re;
    std::optional<EncodedFrame> out;
    for (auto& p : pkts) out = re.offer(p);
    CHECK(out.has_value());
    CHECK(out->seq == 42);
    CHECK(out->type == FrameType::P);
    CHECK(out->depends_on == 41);
    CHECK(out->capture_ts == 42 * 1000);
}

// Integration: a real Encoder GOP fragments and reassembles losslessly.
static void test_encoder_roundtrip() {
    Encoder enc({/*gop=*/4, /*i_bytes=*/4000, /*p_bytes=*/400});
    Packetizer pk(/*mtu=*/1200);
    Reassembler re;
    for (uint64_t n = 1; n <= 12; ++n) {
        Frame f; f.seq = n; f.capture_ts = n * 1000;
        EncodedFrame in = enc.encode(f);
        std::optional<EncodedFrame> out;
        for (auto& p : pk.packetize(in)) out = re.offer(p);
        CHECK(out.has_value());
        CHECK(out->bytes == in.bytes);
        CHECK(out->type == in.type);
    }
    CHECK(re.completed() == 12);
    CHECK(re.pending() == 0);
}

int main() {
    struct { const char* name; void (*fn)(); } cases[] = {
        {"single_packet_frame",          test_single_packet_frame},
        {"multi_packet_frame",           test_multi_packet_frame},
        {"reassemble_out_of_order",      test_reassemble_out_of_order},
        {"missing_fragment_incomplete",  test_missing_fragment_incomplete},
        {"duplicate_fragment_ignored",   test_duplicate_fragment_ignored},
        {"stream_seq_monotonic_across_frames", test_stream_seq_monotonic_across_frames},
        {"metadata_preserved",           test_metadata_preserved},
        {"encoder_roundtrip",            test_encoder_roundtrip},
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
