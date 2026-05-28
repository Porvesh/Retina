#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "retina/packet.hpp"

namespace retina {

// JitterBuffer: the receiver-side buffer that turns a jittery, reordered packet
// arrival into a smooth, in-order playout — trading a little latency for far
// fewer stutters.
//
// Each arriving packet is held for a playout delay measured from ITS arrival.
// The head (lowest stream_seq present) is released once it has been held that
// long; holding it also gives a lower-numbered straggler that window to arrive
// and overtake it (reordering). If the packet before the head never shows up in
// time, releasing the head jumps the gap — each skipped sequence is a STUTTER (a
// playout discontinuity). A packet that arrives after its slot already played
// out is too late: dropped, not replayed.
//
// The latency-vs-stutter knob is the delay: longer waits catch more stragglers
// (fewer stutters) at the cost of more latency. "Adaptive" moves that knob with
// measured jitter — RFC 3550's smoothed interarrival estimate — so a calm link
// stays low-latency and a jittery one widens automatically.
class JitterBuffer {
public:
    struct Config {
        uint64_t base_delay_ns = 0;              // minimum playout delay
        uint64_t max_delay_ns  = UINT64_MAX;     // cap on the adaptive delay
        double   jitter_gain   = 4.0;            // delay = base + gain * est_jitter
        bool     adaptive      = true;           // false => fixed base_delay
    };

    JitterBuffer() : JitterBuffer(Config{}) {}
    explicit JitterBuffer(Config cfg) : cfg_(cfg), target_delay_ns_(cfg.base_delay_ns) {}

    // Offer a packet that the channel delivered at arrival_ns.
    void push(const Packet& p, uint64_t arrival_ns) {
        // RFC 3550-style smoothed interarrival jitter, off the carried timestamp.
        const int64_t transit =
            static_cast<int64_t>(arrival_ns) - static_cast<int64_t>(p.capture_ts);
        if (have_prev_transit_) {
            const int64_t d  = transit - prev_transit_;
            const double  ad = static_cast<double>(d < 0 ? -d : d);
            jitter_est_ += (ad - jitter_est_) / 16.0;
        }
        prev_transit_      = transit;
        have_prev_transit_ = true;
        recompute_delay();

        // A packet for a slot already played out is too late to use.
        if (have_released_ && p.stream_seq <= last_released_seq_) {
            ++dropped_late_;
            return;
        }
        buf_[p.stream_seq] = Entry{p, arrival_ns};
    }

    // Release, in stream_seq order, every packet ripe at now_ns.
    std::vector<Packet> pop(uint64_t now_ns) {
        std::vector<Packet> out;
        while (!buf_.empty()) {
            auto           it      = buf_.begin();        // lowest seq present
            const uint64_t arrival = it->second.arrival;
            if (arrival + target_delay_ns_ > now_ns) break;   // head not ripe yet

            const uint64_t seq = it->first;
            if (have_released_ && seq > last_released_seq_ + 1)
                stutters_ += (seq - last_released_seq_ - 1);   // jumped a gap

            const uint64_t hold = now_ns - arrival;
            hold_sum_ += hold;
            ++hold_count_;
            if (hold > hold_max_) hold_max_ = hold;

            out.push_back(std::move(it->second.pkt));
            last_released_seq_ = seq;
            have_released_     = true;
            ++released_;
            buf_.erase(it);
        }
        return out;
    }

    // End-of-run: release everything remaining, in order.
    std::vector<Packet> flush() { return pop(UINT64_MAX); }

    // ---- stats / observability ----
    uint64_t current_delay_ns()  const { return target_delay_ns_; }
    double   jitter_estimate_ns() const { return jitter_est_; }
    uint64_t released()          const { return released_; }
    uint64_t stutters()          const { return stutters_; }
    uint64_t dropped_late()      const { return dropped_late_; }
    std::size_t pending()        const { return buf_.size(); }
    uint64_t max_hold_ns()       const { return hold_max_; }
    double   mean_hold_ns()      const {
        return hold_count_ ? static_cast<double>(hold_sum_) / static_cast<double>(hold_count_) : 0.0;
    }

private:
    struct Entry { Packet pkt; uint64_t arrival; };

    void recompute_delay() {
        if (!cfg_.adaptive) { target_delay_ns_ = cfg_.base_delay_ns; return; }
        double v = static_cast<double>(cfg_.base_delay_ns) + cfg_.jitter_gain * jitter_est_;
        if (v < static_cast<double>(cfg_.base_delay_ns)) v = static_cast<double>(cfg_.base_delay_ns);
        uint64_t t = static_cast<uint64_t>(v);
        if (t > cfg_.max_delay_ns) t = cfg_.max_delay_ns;
        target_delay_ns_ = t;
    }

    Config                    cfg_;
    std::map<uint64_t, Entry> buf_;             // stream_seq -> packet + arrival
    uint64_t                  target_delay_ns_;

    // adaptive jitter estimate
    double  jitter_est_        = 0.0;
    int64_t prev_transit_      = 0;
    bool    have_prev_transit_ = false;

    // playout tracking
    uint64_t last_released_seq_ = 0;
    bool     have_released_     = false;

    // stats
    uint64_t released_     = 0;
    uint64_t stutters_     = 0;
    uint64_t dropped_late_ = 0;
    uint64_t hold_sum_     = 0;
    uint64_t hold_count_   = 0;
    uint64_t hold_max_     = 0;
};

} // namespace retina
