#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <random>
#include <vector>

#include "retina/packet.hpp"

namespace retina {

// LossyChannel: an in-process model of a hostile UDP link — the M3 adversary.
//
// It is the packet-level analogue of CamSim's fault injection: seeded and
// deterministic, so a run reproduces exactly. Kept in-process (not real sockets)
// on purpose — the whole of Retina is a deterministic simulation, and a real
// socket would add uncontrolled nondeterminism. A real sendto/recvfrom backend
// could later slot in behind the same send()/deliver() shape.
//
// The link is driven by a logical clock the caller supplies (a discrete-event
// model, no wall clock): send(pkt, send_ns) offers a packet; deliver(now_ns)
// returns every packet that has arrived by now_ns, in arrival order.
//
// Impairments:
//   - drop_prob       : each packet is discarded with this probability.
//   - base_latency_ns : fixed one-way propagation delay.
//   - jitter_ns       : + uniform[0, jitter_ns] extra delay per packet. This is
//                       ALSO what produces reordering — a low-jitter packet can
//                       overtake an earlier high-jitter one (the physical cause,
//                       so no separate reorder knob is needed).
//   - bandwidth_bps   : 0 = unlimited; else the link serializes, so a burst
//                       queues and later packets pick up transmission delay —
//                       congestion that grows latency.
class LossyChannel {
public:
    struct Config {
        double   drop_prob       = 0.0;
        uint64_t base_latency_ns = 0;
        uint64_t jitter_ns       = 0;
        uint64_t bandwidth_bps   = 0;   // 0 = unlimited
    };

    LossyChannel(Config cfg, uint64_t seed) : cfg_(cfg), rng_(seed) {}

    // Offer a packet to the link at logical time send_ns. May be dropped;
    // otherwise it is scheduled to arrive at a computed time.
    void send(const Packet& p, uint64_t send_ns) {
        ++sent_;
        if (cfg_.drop_prob > 0.0 && next_unit() < cfg_.drop_prob) {
            ++dropped_;
            return;
        }

        // Bandwidth: the link transmits one packet at a time. A packet can't
        // start until the link is free, and takes size*8/bw to clock out.
        uint64_t departure = send_ns;
        uint64_t tx        = 0;
        if (cfg_.bandwidth_bps > 0) {
            if (departure < link_free_ns_) departure = link_free_ns_;
            tx = (static_cast<uint64_t>(p.payload.size()) * 8ull * 1'000'000'000ull) /
                 cfg_.bandwidth_bps;
            link_free_ns_ = departure + tx;
        }

        uint64_t arrival = departure + tx + cfg_.base_latency_ns;
        if (cfg_.jitter_ns > 0)
            arrival += static_cast<uint64_t>(next_unit() *
                                             static_cast<double>(cfg_.jitter_ns + 1));

        inflight_.emplace(arrival, p);
    }

    // Return, in arrival order, every packet that has arrived by now_ns.
    std::vector<Packet> deliver(uint64_t now_ns) {
        std::vector<Packet> out;
        while (!inflight_.empty() && inflight_.begin()->first <= now_ns) {
            Packet p = inflight_.begin()->second;
            inflight_.erase(inflight_.begin());

            // Out-of-order relative to the highest stream_seq delivered so far.
            if (have_delivered_ && p.stream_seq < max_delivered_seq_) ++reordered_;
            if (!have_delivered_ || p.stream_seq > max_delivered_seq_)
                max_delivered_seq_ = p.stream_seq;
            have_delivered_ = true;

            out.push_back(std::move(p));
            ++delivered_;
        }
        return out;
    }

    // Deliver everything still in flight (end-of-run drain).
    std::vector<Packet> flush() { return deliver(UINT64_MAX); }

    // ---- stats ----
    uint64_t    sent()      const { return sent_; }
    uint64_t    dropped()   const { return dropped_; }
    uint64_t    delivered() const { return delivered_; }
    uint64_t    reordered() const { return reordered_; }
    std::size_t inflight()  const { return inflight_.size(); }

private:
    // Uniform [0,1) drawn from the engine directly (portable-deterministic).
    double next_unit() {
        return static_cast<double>(rng_() >> 11) * (1.0 / 9007199254740992.0);
    }

    Config                      cfg_;
    std::mt19937_64             rng_;
    std::multimap<uint64_t, Packet> inflight_;   // arrival_ns -> packet (ordered)
    uint64_t                    link_free_ns_ = 0;

    uint64_t sent_              = 0;
    uint64_t dropped_           = 0;
    uint64_t delivered_         = 0;
    uint64_t reordered_         = 0;
    uint64_t max_delivered_seq_ = 0;
    bool     have_delivered_    = false;
};

} // namespace retina
