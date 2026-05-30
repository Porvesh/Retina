#pragma once
#include <cstdint>
#include <map>
#include <random>
#include <vector>

#include "retina/net/decoder.hpp"

namespace retina {

// The M3 feedback loop: when loss breaks decoding beyond what FEC can repair,
// the receiver asks the sender for a fresh keyframe (RTP/RTCP's Picture Loss
// Indication), the sender inserts an I-frame, and decoding heals at that frame.
//
// This is deliberately NOT per-packet retransmit: a resent packet would miss its
// playout deadline. Resetting the dependency chain with a keyframe is the
// real-time-correct recovery, at a cost of one round trip plus the wait for the
// next frame.

// Receiver-side policy: decide WHEN to send a keyframe request. Debounced — one
// request outstanding at a time (any decodable frame means the stream healed and
// clears it), with an optional re-request timeout in case the request was lost.
class KeyframeRequester {
public:
    struct Config {
        uint64_t rerequest_timeout_ns = 0;   // 0 = never re-request
    };

    KeyframeRequester() = default;
    explicit KeyframeRequester(Config cfg) : cfg_(cfg) {}

    // Observe a decode result at now_ns; returns true if a request should be sent.
    bool observe(const Decoder::Result& r, uint64_t now_ns) {
        if (r.decodable) {           // chain intact again -> nothing outstanding
            outstanding_ = false;
            return false;
        }
        const bool timed_out =
            cfg_.rerequest_timeout_ns != 0 &&
            now_ns - last_request_ns_ >= cfg_.rerequest_timeout_ns;
        if (!outstanding_ || timed_out) {
            outstanding_     = true;
            last_request_ns_ = now_ns;
            ++requests_;
            return true;
        }
        return false;                // already asked; wait for the keyframe
    }

    uint64_t requests()   const { return requests_; }
    bool     outstanding() const { return outstanding_; }

private:
    Config   cfg_;
    bool     outstanding_     = false;
    uint64_t last_request_ns_ = 0;
    uint64_t requests_        = 0;
};

// The return path a request travels: a one-way delay (half the RTT) with
// optional loss. Small control messages, not frames.
class FeedbackLink {
public:
    FeedbackLink(uint64_t delay_ns, double drop_prob = 0.0, uint64_t seed = 0)
        : delay_ns_(delay_ns), drop_prob_(drop_prob), rng_(seed) {}

    void send(uint64_t token, uint64_t now_ns) {
        ++sent_;
        if (drop_prob_ > 0.0 && next_unit() < drop_prob_) { ++dropped_; return; }
        inflight_.emplace(now_ns + delay_ns_, token);
    }

    // Requests that have arrived at the sender by now_ns.
    std::vector<uint64_t> deliver(uint64_t now_ns) {
        std::vector<uint64_t> out;
        while (!inflight_.empty() && inflight_.begin()->first <= now_ns) {
            out.push_back(inflight_.begin()->second);
            inflight_.erase(inflight_.begin());
        }
        return out;
    }

    uint64_t sent()    const { return sent_; }
    uint64_t dropped() const { return dropped_; }

private:
    double next_unit() {
        return static_cast<double>(rng_() >> 11) * (1.0 / 9007199254740992.0);
    }

    uint64_t                         delay_ns_;
    double                           drop_prob_;
    std::mt19937_64                  rng_;
    std::multimap<uint64_t, uint64_t> inflight_;   // arrival_ns -> token
    uint64_t                         sent_    = 0;
    uint64_t                         dropped_ = 0;
};

} // namespace retina
