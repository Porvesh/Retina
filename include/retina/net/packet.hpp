#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "retina/net/encoded_frame.hpp"

namespace retina {

// RTP-style framing for the M3 network hop.
//
// A real EncodedFrame is far bigger than one UDP datagram, so it is FRAGMENTED
// into Packets that each fit an MTU, sent independently, and reassembled on the
// far side. Once packets travel a lossy link, losing any one fragment leaves a
// HOLE in its frame — the frame can't be reassembled unless FEC recovers the
// missing packet. That is the failure mode the rest of M3 defends against.
//
// Each packet self-describes (like an RTP header): a stream-wide packet sequence
// (for loss/reorder detection and FEC grouping), the frame it belongs to, its
// fragment index/count, a marker for the last fragment, and the frame-level
// metadata (type, timestamp, dependency) so the receiver can reason about a
// frame from any of its packets.
struct Packet {
    uint64_t             stream_seq = 0;   // monotonic across ALL packets sent
    uint64_t             frame_seq  = 0;   // which frame this fragment belongs to
    uint32_t             frag_idx   = 0;   // 0 .. frag_count-1
    uint32_t             frag_count = 1;   // fragments in this frame
    bool                 marker     = false; // true on the last fragment
    FrameType            type       = FrameType::I;
    uint64_t             capture_ts = 0;
    uint64_t             depends_on = 0;
    std::vector<uint8_t> payload;          // this fragment's slice of the bytes

    // FEC routing (see fec.hpp). Data packets carry the group they belong to;
    // a parity packet has is_parity=true and fec_k = #data packets it protects.
    // The reassembler and jitter buffer ignore these fields.
    bool                 is_parity  = false;
    uint64_t             fec_group  = 0;
    uint32_t             fec_k      = 0;
};

// Splits EncodedFrames into MTU-sized packets, numbering every packet with a
// stream-wide monotonic sequence.
class Packetizer {
public:
    explicit Packetizer(uint32_t mtu_payload = 1200) : mtu_(mtu_payload ? mtu_payload : 1) {}

    std::vector<Packet> packetize(const EncodedFrame& ef) {
        const std::size_t total = ef.bytes.size();
        // Even a zero-byte frame is one (empty) packet, so it can be "received".
        const uint32_t frag_count =
            total == 0 ? 1u : static_cast<uint32_t>((total + mtu_ - 1) / mtu_);

        std::vector<Packet> out;
        out.reserve(frag_count);
        for (uint32_t i = 0; i < frag_count; ++i) {
            Packet p;
            p.stream_seq = stream_seq_++;
            p.frame_seq  = ef.seq;
            p.frag_idx   = i;
            p.frag_count = frag_count;
            p.marker     = (i + 1 == frag_count);
            p.type       = ef.type;
            p.capture_ts = ef.capture_ts;
            p.depends_on = ef.depends_on;
            const std::size_t off = static_cast<std::size_t>(i) * mtu_;
            const std::size_t len = std::min<std::size_t>(mtu_, total - off);
            p.payload.assign(ef.bytes.begin() + static_cast<std::ptrdiff_t>(off),
                             ef.bytes.begin() + static_cast<std::ptrdiff_t>(off + len));
            out.push_back(std::move(p));
        }
        return out;
    }

    uint64_t packets_sent() const { return stream_seq_; }

private:
    uint32_t mtu_;
    uint64_t stream_seq_ = 0;
};

// Reassembles packets back into EncodedFrames. Tolerates out-of-order and
// duplicate fragments; a frame missing any fragment simply never completes and
// stays pending (whether to give up on it is the receiver's policy, M3-D/E).
class Reassembler {
public:
    // Returns the completed frame when this packet was its final missing
    // fragment, else nullopt.
    std::optional<EncodedFrame> offer(const Packet& p) {
        if (p.frag_idx >= p.frag_count) return std::nullopt;   // malformed

        Partial& part = partials_[p.frame_seq];
        if (part.frags.empty()) {                              // first fragment seen
            part.frag_count = p.frag_count;
            part.type       = p.type;
            part.capture_ts = p.capture_ts;
            part.depends_on = p.depends_on;
            part.frags.resize(p.frag_count);
            part.have.assign(p.frag_count, false);
        }
        if (!part.have[p.frag_idx]) {                          // ignore duplicates
            part.have[p.frag_idx]  = true;
            part.frags[p.frag_idx] = p.payload;
            ++part.received;
        }
        if (part.received != part.frag_count) return std::nullopt;

        EncodedFrame ef;
        ef.seq        = p.frame_seq;
        ef.capture_ts = part.capture_ts;
        ef.type       = part.type;
        ef.depends_on = part.depends_on;
        for (const auto& fr : part.frags)
            ef.bytes.insert(ef.bytes.end(), fr.begin(), fr.end());
        partials_.erase(p.frame_seq);
        ++completed_;
        return ef;
    }

    uint64_t completed() const { return completed_; }
    std::size_t pending() const { return partials_.size(); }
    bool is_pending(uint64_t frame_seq) const { return partials_.count(frame_seq) != 0; }

private:
    struct Partial {
        uint32_t                          frag_count = 0;
        uint32_t                          received   = 0;
        FrameType                         type       = FrameType::I;
        uint64_t                          capture_ts = 0;
        uint64_t                          depends_on = 0;
        std::vector<std::vector<uint8_t>> frags;      // by frag_idx
        std::vector<bool>                 have;       // for duplicate detection
    };

    std::map<uint64_t, Partial> partials_;
    uint64_t                    completed_ = 0;
};

} // namespace retina
