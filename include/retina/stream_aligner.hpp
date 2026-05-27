#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "retina/frame.hpp"

namespace retina {

// StreamAligner: matches frames across N independently-drifting camera streams
// into time-aligned tuples — the M2 core.
//
// The problem it solves
// ---------------------
// N cameras are nominally triggered "together", but each has its own clock that
// drifts (see CamSim's drift_ppm), so frame k from cam A and frame k from cam B
// report DIFFERENT capture_ts even though they fired at the same real instant —
// and the gap grows over time. Drops make seq matching useless too: a dropped
// trigger on one stream shifts its seq numbering relative to the others. So we
// must match by TIMESTAMP, within a tolerance, not by seq.
//
// The policy (reference stream + nearest neighbour, one-to-one)
// -------------------------------------------------------------
// Stream 0 is the REFERENCE ("master" camera). For each reference frame, in
// order, we find in every other stream the frame whose capture_ts is closest to
// the reference's. If EVERY nearest neighbour is within epsilon_ns of the
// reference -> emit an aligned tuple. Otherwise the streams have drifted too far
// apart for this frame -> REJECT it. Each frame is consumed by at most one tuple
// (one-to-one), which is the natural stereo/multi-cam model.
//
// Online, with a "wait until bracketed" gate
// -------------------------------------------
// push() buffers frames per stream, then decides every reference frame it now
// can. A reference frame can only be decided once every other stream holds a
// frame with capture_ts >= the reference's: that upper bracket guarantees the
// true nearest neighbour has already arrived (it is either the greatest ts below
// the reference, or the least ts at/above it — both present). Until then we wait
// for more pushes. flush() forces a decision on whatever remains at end-of-run.
//
// Stalls and dead streams (max_wait_ns)
// -------------------------------------
// Waiting for a bracket is unbounded: if a stream goes silent (camera unplugged,
// thread dies), its newest frame freezes below the ever-advancing reference and
// the bracket never completes — so the aligner would stall FOREVER and buffer
// reference frames without bound. max_wait_ns caps that wait: once the newest
// frame seen on any stream is that far past a reference, the reference is decided
// with whatever is present, and a stream with nothing in range REJECTS. A dead
// stream thus turns into a steady flow of rejections at bounded latency, not
// silence. An old-frame prune keeps the live streams bounded too. Callers see
// which stream went dark via misses(id) / staleness(id) / is_stalled(id).
//
// What it does NOT do: retain pixels. Frame::data is a borrowed view that
// dangles once buffered, so the aligner matches purely on metadata (seq,
// capture_ts). A real compositor would pin frame handles here; M2 only needs the
// timing.
class StreamAligner {
public:
    // One aligned set: one frame per stream, frames[i] came from stream i.
    struct AlignedTuple {
        std::vector<Frame> frames;    // index == stream id
        uint64_t           ref_ts;    // the reference (stream 0) capture_ts
        uint64_t           skew_ns;   // spread = max capture_ts - min capture_ts
    };

    // max_wait_ns bounds how long a reference frame waits for a laggard stream.
    // Once the newest frame seen on ANY stream is more than max_wait_ns past a
    // reference, we stop waiting and decide it with whatever is present — a
    // stream that went dark then produces rejections at bounded latency instead
    // of stalling the aligner forever. max_wait_ns == 0 disables the horizon
    // (unbounded wait; fine for bounded batch runs that always flush()).
    StreamAligner(std::size_t num_streams, uint64_t epsilon_ns,
                  uint64_t max_wait_ns = 0)
        : epsilon_ns_(epsilon_ns),
          max_wait_ns_(max_wait_ns),
          streams_(num_streams),
          last_seq_(num_streams, 0),
          dropped_(num_streams, 0),
          last_ts_(num_streams, 0),
          misses_(num_streams, 0) {}

    // Feed one frame from a stream. Detects seq gaps (dropped triggers) and then
    // drains any tuples/rejections that can now be decided.
    void push(std::size_t stream_id, const Frame& f) {
        // Seq-gap detection: a gap means the camera dropped trigger(s) upstream.
        const uint64_t last = last_seq_[stream_id];
        if (last != 0 && f.seq > last + 1)
            dropped_[stream_id] += (f.seq - last - 1);
        last_seq_[stream_id] = f.seq;

        last_ts_[stream_id] = f.capture_ts;
        if (f.capture_ts > newest_seen_) newest_seen_ = f.capture_ts;

        streams_[stream_id].push_back(f);
        pump(/*flushing=*/false);
    }

    // End-of-run: decide every remaining reference frame with whatever is
    // buffered (no more frames are coming, so the bracket gate would stall).
    void flush() { pump(/*flushing=*/true); }

    // Retrieve the next ready aligned tuple, or nullopt if none pending.
    std::optional<AlignedTuple> pop() {
        if (out_.empty()) return std::nullopt;
        AlignedTuple t = std::move(out_.front());
        out_.pop_front();
        return t;
    }

    // ---- metrics (for the M2 histogram / rejection-vs-drift curve) ----
    std::size_t num_streams()                    const { return streams_.size(); }
    uint64_t    epsilon_ns()                     const { return epsilon_ns_; }
    uint64_t    max_wait_ns()                    const { return max_wait_ns_; }
    uint64_t    emitted()                        const { return emitted_; }
    uint64_t    rejected()                       const { return rejected_; }
    uint64_t    dropped_triggers(std::size_t id) const { return dropped_[id]; }

    // ---- per-stream health (how you tell a stream went dark) ----
    // Times this stream failed to supply an in-epsilon frame for a decided
    // reference — climbs steadily while a stream is missing or badly desynced.
    uint64_t misses(std::size_t id) const { return misses_[id]; }

    // How far behind the newest frame seen ANYWHERE this stream has fallen. A
    // stream that never produced reads as fully stale (its last ts is 0).
    uint64_t staleness(std::size_t id) const {
        const uint64_t seen = last_ts_[id];
        return newest_seen_ > seen ? newest_seen_ - seen : 0;
    }

    // True once a stream has fallen more than the horizon behind — i.e. the
    // aligner has given up waiting on it. Meaningless without a max_wait_ns.
    bool is_stalled(std::size_t id) const {
        return max_wait_ns_ != 0 && staleness(id) > max_wait_ns_;
    }

private:
    // Decide as many reference frames as possible. `flushing` bypasses the
    // bracket gate (used only at end-of-run).
    void pump(bool flushing) {
        auto& ref_q = streams_[0];
        while (!ref_q.empty()) {
            const Frame ref = ref_q.front();

            // Gate: normally wait until every other stream brackets the
            // reference (holds a frame at/after it), so the true nearest has
            // arrived. Two escapes stop us waiting forever on a stream that
            // won't deliver: flushing (end of run), or the max-wait horizon —
            // the newest frame seen ANYWHERE is already more than max_wait_ns
            // past this reference, so any laggard is declared too late.
            if (!flushing) {
                bool bracketed = true;
                for (std::size_t j = 1; j < streams_.size(); ++j) {
                    if (streams_[j].empty() ||
                        streams_[j].back().capture_ts < ref.capture_ts) {
                        bracketed = false;
                        break;
                    }
                }
                const bool timed_out =
                    max_wait_ns_ != 0 && newest_seen_ > ref.capture_ts + max_wait_ns_;
                if (!bracketed && !timed_out) return;   // keep waiting
            }

            // Prune frames too old to match this or any later reference (refs
            // only increase, so a frame more than epsilon below ref is dead).
            // Without this, a run that keeps rejecting — e.g. one stream stalled
            // while the others flow — would grow the live streams unbounded.
            for (std::size_t j = 1; j < streams_.size(); ++j) {
                auto& q = streams_[j];
                while (!q.empty() &&
                       q.front().capture_ts + epsilon_ns_ < ref.capture_ts)
                    q.pop_front();
            }

            // Nearest neighbour in each other stream. Every stream is evaluated
            // (no early break) so a failure is attributed to the specific
            // stream(s) that missed — that per-stream miss count is how a caller
            // sees which stream went dark.
            std::vector<Frame>       tuple(streams_.size());
            std::vector<std::size_t> match_idx(streams_.size(), 0);
            tuple[0] = ref;
            bool ok = true;   // every other stream supplied an in-epsilon frame
            for (std::size_t j = 1; j < streams_.size(); ++j) {
                const auto& q = streams_[j];
                if (q.empty()) { ++misses_[j]; ok = false; continue; }
                std::size_t best_k = 0;
                uint64_t    best_d = abs_diff(q[0].capture_ts, ref.capture_ts);
                for (std::size_t k = 1; k < q.size(); ++k) {
                    const uint64_t d = abs_diff(q[k].capture_ts, ref.capture_ts);
                    if (d < best_d) { best_d = d; best_k = k; }
                }
                tuple[j]     = q[best_k];
                match_idx[j] = best_k;
                if (best_d > epsilon_ns_) { ++misses_[j]; ok = false; }
            }

            // The reference frame is decided now either way — consume it.
            ref_q.pop_front();

            if (ok) {
                // Consume matched frames one-to-one: drop everything up to and
                // including the matched index in each other stream.
                uint64_t lo = ref.capture_ts, hi = ref.capture_ts;
                for (std::size_t j = 1; j < streams_.size(); ++j) {
                    const uint64_t t = tuple[j].capture_ts;
                    lo = t < lo ? t : lo;
                    hi = t > hi ? t : hi;
                    auto& q = streams_[j];
                    q.erase(q.begin(),
                            q.begin() + static_cast<std::ptrdiff_t>(match_idx[j] + 1));
                }
                out_.push_back(AlignedTuple{std::move(tuple), ref.capture_ts, hi - lo});
                ++emitted_;
            } else {
                // Drifted past epsilon, or a stream had nothing in range:
                // reject the reference, leave the others for the next one.
                ++rejected_;
            }
        }
    }

    static uint64_t abs_diff(uint64_t a, uint64_t b) {
        return a > b ? a - b : b - a;
    }

    uint64_t                       epsilon_ns_;
    uint64_t                       max_wait_ns_;  // 0 = wait forever (batch mode)
    std::vector<std::deque<Frame>> streams_;      // pending frames per stream
    std::vector<uint64_t>          last_seq_;     // for seq-gap detection
    std::vector<uint64_t>          dropped_;      // dropped triggers per stream
    std::vector<uint64_t>          last_ts_;      // last capture_ts seen per stream
    std::vector<uint64_t>          misses_;       // in-epsilon failures per stream
    std::deque<AlignedTuple>       out_;          // ready tuples awaiting pop()
    uint64_t                       newest_seen_ = 0;  // max capture_ts across all
    uint64_t                       emitted_     = 0;
    uint64_t                       rejected_    = 0;
};

} // namespace retina
