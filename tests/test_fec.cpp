// Single-threaded tests for XOR FEC (FecEncoder + FecDecoder).
//
// Pins single-loss recovery (whole packet, all fields), the two-loss limit,
// parity-loss tolerance, overhead accounting, protect-I-harder, and an
// end-to-end repair through the packetizer/reassembler.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "retina/encoder.hpp"
#include "retina/fec.hpp"
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

static Packet mkpkt(uint64_t seq, FrameType type = FrameType::P,
                    uint32_t payload_size = 100) {
    Packet p;
    p.stream_seq = seq;
    p.frame_seq  = seq;
    p.type       = type;
    p.capture_ts = seq * 1000;
    p.depends_on = seq ? seq - 1 : 0;
    p.payload.resize(payload_size);
    for (uint32_t i = 0; i < payload_size; ++i)
        p.payload[i] = static_cast<uint8_t>((seq + i) & 0xFF);
    return p;
}

static bool same_core(const Packet& a, const Packet& b) {
    return a.stream_seq == b.stream_seq && a.frame_seq == b.frame_seq &&
           a.frag_idx == b.frag_idx && a.frag_count == b.frag_count &&
           a.marker == b.marker && a.type == b.type &&
           a.capture_ts == b.capture_ts && a.depends_on == b.depends_on &&
           a.payload == b.payload;
}

// No loss: the decoder recovers nothing (parity goes unused).
static void test_no_loss_no_recovery() {
    FecEncoder enc({/*k_p=*/4, /*k_i=*/4});
    FecDecoder dec;
    for (uint64_t n = 0; n < 8; ++n)
        for (auto& p : enc.encode(mkpkt(n))) dec.offer(p);
    for (auto& p : enc.finish()) dec.offer(p);
    CHECK(dec.recovered() == 0);
}

// A single lost data packet in a group is reconstructed byte-for-byte.
static void test_single_loss_recovered() {
    FecEncoder enc({/*k_p=*/4, /*k_i=*/4});
    std::vector<Packet> wire;
    for (uint64_t n = 0; n < 4; ++n)
        for (auto& p : enc.encode(mkpkt(n))) wire.push_back(p);
    // wire = [d0 d1 d2 d3 parity]. Drop d2, feed the rest.
    Packet original_d2 = mkpkt(2);

    FecDecoder dec;
    std::vector<Packet> recovered;
    for (auto& p : wire) {
        if (!p.is_parity && p.stream_seq == 2) continue;   // lost on the link
        for (auto& r : dec.offer(p)) recovered.push_back(r);
    }
    CHECK(dec.recovered() == 1);
    CHECK(recovered.size() == 1);
    CHECK(same_core(recovered[0], original_d2));           // whole packet back
}

// Two losses in the same group cannot be recovered by XOR.
static void test_two_losses_unrecoverable() {
    FecEncoder enc({/*k_p=*/4, /*k_i=*/4});
    std::vector<Packet> wire;
    for (uint64_t n = 0; n < 4; ++n)
        for (auto& p : enc.encode(mkpkt(n))) wire.push_back(p);

    FecDecoder dec;
    for (auto& p : wire) {
        if (!p.is_parity && (p.stream_seq == 1 || p.stream_seq == 2)) continue;
        dec.offer(p);
    }
    CHECK(dec.recovered() == 0);
}

// Losing only the parity packet is harmless: all data arrived, nothing to do.
static void test_parity_loss_ok() {
    FecEncoder enc({/*k_p=*/4, /*k_i=*/4});
    std::vector<Packet> wire;
    for (uint64_t n = 0; n < 4; ++n)
        for (auto& p : enc.encode(mkpkt(n))) wire.push_back(p);

    FecDecoder dec;
    for (auto& p : wire) {
        if (p.is_parity) continue;   // parity lost; data intact
        dec.offer(p);
    }
    CHECK(dec.recovered() == 0);
}

// Overhead accounting: k=4 over 12 data packets -> 3 parity -> 25% overhead.
static void test_overhead_accounting() {
    FecEncoder enc({/*k_p=*/4, /*k_i=*/4});
    for (uint64_t n = 0; n < 12; ++n) enc.encode(mkpkt(n));
    enc.finish();
    CHECK(enc.data_packets() == 12);
    CHECK(enc.parity_packets() == 3);
    CHECK(enc.overhead() == 0.25);
}

// Protect I harder: the same packet count yields more parity for I-frame packets
// (smaller k) than for P-frame packets (larger k).
static void test_protect_i_harder() {
    FecEncoder ei({/*k_p=*/8, /*k_i=*/4});
    for (uint64_t n = 0; n < 8; ++n) ei.encode(mkpkt(n, FrameType::I));
    ei.finish();

    FecEncoder ep({/*k_p=*/8, /*k_i=*/4});
    for (uint64_t n = 0; n < 8; ++n) ep.encode(mkpkt(n, FrameType::P));
    ep.finish();

    CHECK(ei.parity_packets() == 2);   // 8 / k_i(4)
    CHECK(ep.parity_packets() == 1);   // 8 / k_p(8)
    CHECK(ei.parity_packets() > ep.parity_packets());
}

// End-to-end: a real I-frame loses one fragment. Without FEC the frame can't
// reassemble; with FEC the fragment is recovered and the frame completes.
static void test_pipeline_repairs_frame() {
    Encoder enc({/*gop=*/8, /*i_bytes=*/4000, /*p_bytes=*/400});
    Packetizer pk(/*mtu=*/1200);              // I-frame -> 4 packets
    Frame f; f.seq = 1; f.capture_ts = 1000;
    EncodedFrame iframe = enc.encode(f);      // the I-frame
    std::vector<Packet> pkts = pk.packetize(iframe);
    const uint64_t drop_seq = pkts[1].stream_seq;   // lose the 2nd fragment

    // Without FEC: reassembler never completes the frame.
    {
        Reassembler re;
        for (auto& p : pkts)
            if (p.stream_seq != drop_seq) re.offer(p);
        CHECK(re.completed() == 0);
        CHECK(re.is_pending(1));
    }

    // With FEC: the lost fragment is reconstructed, the frame completes.
    {
        FecEncoder fenc({/*k_p=*/8, /*k_i=*/8});
        FecDecoder fdec;
        Reassembler re;
        std::vector<Packet> wire;
        for (auto& p : pkts) for (auto& w : fenc.encode(p)) wire.push_back(w);
        for (auto& w : fenc.finish()) wire.push_back(w);

        for (auto& w : wire) {
            if (!w.is_parity && w.stream_seq == drop_seq) continue;   // lost
            std::vector<Packet> to_reassemble = fdec.offer(w);        // recovered?
            if (!w.is_parity) to_reassemble.insert(to_reassemble.begin(), w);
            for (auto& p : to_reassemble) re.offer(p);
        }
        CHECK(fdec.recovered() == 1);
        CHECK(re.completed() == 1);   // frame repaired
    }
}

int main() {
    struct { const char* name; void (*fn)(); } cases[] = {
        {"no_loss_no_recovery",     test_no_loss_no_recovery},
        {"single_loss_recovered",   test_single_loss_recovered},
        {"two_losses_unrecoverable", test_two_losses_unrecoverable},
        {"parity_loss_ok",          test_parity_loss_ok},
        {"overhead_accounting",     test_overhead_accounting},
        {"protect_i_harder",        test_protect_i_harder},
        {"pipeline_repairs_frame",  test_pipeline_repairs_frame},
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
