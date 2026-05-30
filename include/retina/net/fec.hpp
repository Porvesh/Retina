#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "retina/net/packet.hpp"

namespace retina {

// XOR forward error correction — repair a lost packet without a retransmit.
//
// Real-time media never waits for a resend (the resend would miss its playout
// deadline), so it sends REDUNDANCY ahead of time. The simplest scheme: for
// every K data packets, transmit one parity packet = the XOR of the K. If any
// single packet of the K+1 is lost, the receiver reconstructs it by XORing the
// survivors. Two losses in a group are unrecoverable (that is Reed-Solomon's
// job, a later upgrade).
//
// XOR is over the packets' serialized bytes (header + payload), padded to the
// group's longest, so a recovered packet comes back whole — every field, not
// just its payload.
//
// "Protect I-frames harder": an I-frame is catastrophic to lose (it orphans a
// whole GOP), so its packets are grouped with a SMALLER k — more parity per
// data packet, so a loss is likelier to be the only one in its group. That is
// the recovery-vs-overhead knob this scheme exposes.

namespace fec_detail {
inline void put_u64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
inline void put_u32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
inline uint64_t get_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}
inline uint32_t get_u32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
    return v;
}

// Serialize the fields a receiver must reconstruct (not the FEC routing).
inline std::vector<uint8_t> serialize_core(const Packet& p) {
    std::vector<uint8_t> b;
    put_u64(b, p.stream_seq);
    put_u64(b, p.frame_seq);
    put_u32(b, p.frag_idx);
    put_u32(b, p.frag_count);
    b.push_back(p.marker ? 1 : 0);
    b.push_back(static_cast<uint8_t>(p.type));
    put_u64(b, p.capture_ts);
    put_u64(b, p.depends_on);
    put_u32(b, static_cast<uint32_t>(p.payload.size()));
    b.insert(b.end(), p.payload.begin(), p.payload.end());
    return b;
}

inline Packet deserialize_core(const std::vector<uint8_t>& b) {
    Packet p;
    std::size_t o = 0;
    p.stream_seq = get_u64(&b[o]); o += 8;
    p.frame_seq  = get_u64(&b[o]); o += 8;
    p.frag_idx   = get_u32(&b[o]); o += 4;
    p.frag_count = get_u32(&b[o]); o += 4;
    p.marker     = b[o++] != 0;
    p.type       = static_cast<FrameType>(b[o++]);
    p.capture_ts = get_u64(&b[o]); o += 8;
    p.depends_on = get_u64(&b[o]); o += 8;
    const uint32_t len = get_u32(&b[o]); o += 4;
    p.payload.assign(b.begin() + static_cast<std::ptrdiff_t>(o),
                     b.begin() + static_cast<std::ptrdiff_t>(o + len));
    return p;
}

// blob_xor[i] ^= src[i], growing blob_xor to hold src.
inline void xor_into(std::vector<uint8_t>& acc, const std::vector<uint8_t>& src) {
    if (acc.size() < src.size()) acc.resize(src.size(), 0);
    for (std::size_t i = 0; i < src.size(); ++i) acc[i] ^= src[i];
}
}  // namespace fec_detail

// Sender side: passes data packets through (tagging each with its FEC group) and
// emits a parity packet whenever a group closes.
class FecEncoder {
public:
    struct Config {
        uint32_t k_p = 8;   // data packets per parity for P-frame runs
        uint32_t k_i = 4;   // smaller for I-frames => more protection
    };

    FecEncoder() : FecEncoder(Config{}) {}
    explicit FecEncoder(Config cfg) : cfg_(cfg) {}

    // Returns the (tagged) data packet plus any parity packet(s) a completed
    // group produced. Emit order is data-then-parity.
    std::vector<Packet> encode(Packet data) {
        std::vector<Packet> out;

        // Close the current group early if the protection class changes.
        if (!group_.empty() && data.type != group_type_)
            out.push_back(close_group());

        if (group_.empty()) {
            group_type_ = data.type;
            group_k_    = (data.type == FrameType::I) ? cfg_.k_i : cfg_.k_p;
            group_id_   = next_group_id_++;
        }

        data.is_parity = false;
        data.fec_group = group_id_;
        group_.push_back(data);
        ++data_packets_;
        out.push_back(data);

        if (group_.size() >= group_k_) out.push_back(close_group());
        return out;
    }

    // Flush a trailing partial group at end-of-stream.
    std::vector<Packet> finish() {
        std::vector<Packet> out;
        if (!group_.empty()) out.push_back(close_group());
        return out;
    }

    uint64_t data_packets()   const { return data_packets_; }
    uint64_t parity_packets() const { return parity_packets_; }
    double   overhead()       const {
        return data_packets_ ? static_cast<double>(parity_packets_) /
                                   static_cast<double>(data_packets_)
                             : 0.0;
    }

private:
    Packet close_group() {
        Packet parity;
        parity.is_parity = true;
        parity.fec_group = group_id_;
        parity.fec_k     = static_cast<uint32_t>(group_.size());
        for (const auto& d : group_)
            fec_detail::xor_into(parity.payload, fec_detail::serialize_core(d));
        group_.clear();
        ++parity_packets_;
        return parity;
    }

    Config              cfg_;
    std::vector<Packet> group_;
    FrameType           group_type_    = FrameType::I;
    uint32_t            group_k_       = 0;
    uint64_t            group_id_      = 0;
    uint64_t            next_group_id_ = 0;
    uint64_t            data_packets_   = 0;
    uint64_t            parity_packets_ = 0;
};

// Receiver side: buffers each group; when exactly one data packet is missing and
// the parity is present, reconstructs the missing packet.
class FecDecoder {
public:
    // Offer any packet (data or parity). Returns a reconstructed data packet if
    // this offer made a single-loss group recoverable, else empty.
    std::vector<Packet> offer(const Packet& pkt) {
        Group& g = groups_[pkt.fec_group];
        if (pkt.is_parity) {
            g.parity     = pkt;
            g.k          = pkt.fec_k;
            g.has_parity = true;
        } else {
            g.data[pkt.stream_seq] = pkt;
        }

        std::vector<Packet> out;
        if (g.done || !g.has_parity) return out;

        if (g.data.size() >= g.k) {
            g.done = true;                       // all data arrived; parity unused
        } else if (g.data.size() + 1 == g.k) {   // exactly one missing -> recover
            out.push_back(recover(g));
            ++recovered_;
            g.done = true;
        }
        return out;
    }

    uint64_t recovered() const { return recovered_; }

private:
    struct Group {
        std::map<uint64_t, Packet> data;         // by stream_seq
        Packet                     parity;
        uint32_t                   k          = 0;
        bool                       has_parity = false;
        bool                       done       = false;
    };

    static Packet recover(const Group& g) {
        std::vector<uint8_t> blob = g.parity.payload;    // XOR of all k data blobs
        for (const auto& kv : g.data)
            fec_detail::xor_into(blob, fec_detail::serialize_core(kv.second));
        Packet m = fec_detail::deserialize_core(blob);   // the one missing packet
        m.fec_group = g.parity.fec_group;
        m.is_parity = false;
        return m;
    }

    std::map<uint64_t, Group> groups_;
    uint64_t                  recovered_ = 0;
};

} // namespace retina
