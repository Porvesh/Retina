#pragma once
#include <cstdint>
#include <unordered_set>

#include "retina/net/encoded_frame.hpp"

namespace retina {

// Decoder: the receiver-side dependency model. It does not decode pixels — it
// decides whether each reassembled frame is DECODABLE given the I/P chain, and
// measures glass-to-glass latency.
//
// Rules (backward deps only, so no reorder-for-decode):
//   - An I-frame is always decodable and RESETS the chain: nothing after it
//     depends on anything before it, so the reference set is cleared to just it.
//   - A P-frame is decodable iff the frame it depends on was itself decoded.
//     A missing (lost, never-reassembled) reference therefore orphans every P
//     that chains off it — until the next I-frame heals the stream.
//
// This is the damage M3 fights: lose one P and you glitch a frame; lose one I
// and the whole GOP is undecodable until a keyframe. Feed frames in decode
// order (the jitter buffer, M3-E, guarantees that in the full pipeline).
class Decoder {
public:
    struct Result {
        uint64_t  seq        = 0;
        FrameType type       = FrameType::I;
        bool      decodable  = false;
        uint64_t  latency_ns = 0;   // now_ns - capture_ts, for decodable frames
    };

    // Offer a reassembled frame, at logical arrival time now_ns.
    Result decode(const EncodedFrame& ef, uint64_t now_ns = 0) {
        Result r;
        r.seq  = ef.seq;
        r.type = ef.type;

        if (ef.type == FrameType::I) {
            decoded_.clear();               // chain reset — bounds memory to a GOP
            decoded_.insert(ef.seq);
            r.decodable = true;
        } else {
            r.decodable = decoded_.count(ef.depends_on) != 0;
            if (r.decodable) decoded_.insert(ef.seq);
        }

        if (r.decodable) {
            ++decoded_ok_;
            if (now_ns >= ef.capture_ts) {
                const uint64_t lat = now_ns - ef.capture_ts;
                r.latency_ns = lat;
                lat_sum_ += lat;
                ++lat_count_;
                if (lat < lat_min_) lat_min_ = lat;
                if (lat > lat_max_) lat_max_ = lat;
            }
        } else {
            ++undecodable_;
        }
        return r;
    }

    // ---- stats ----
    uint64_t decoded()     const { return decoded_ok_; }
    uint64_t undecodable() const { return undecodable_; }

    uint64_t latency_count() const { return lat_count_; }
    uint64_t latency_min()   const { return lat_count_ ? lat_min_ : 0; }
    uint64_t latency_max()   const { return lat_max_; }
    double   latency_mean()  const {
        return lat_count_ ? static_cast<double>(lat_sum_) / static_cast<double>(lat_count_) : 0.0;
    }

private:
    std::unordered_set<uint64_t> decoded_;   // seqs decodable in the current GOP
    uint64_t decoded_ok_  = 0;
    uint64_t undecodable_ = 0;

    uint64_t lat_sum_   = 0;
    uint64_t lat_count_ = 0;
    uint64_t lat_min_   = UINT64_MAX;
    uint64_t lat_max_   = 0;
};

} // namespace retina
