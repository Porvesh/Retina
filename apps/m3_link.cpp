// M3 "link" demo — the whole lossy-network stack, end to end.
//
//   CamSim -> Encoder(I/P) -> Packetizer -> FEC -> [LossyChannel] -> FEC decode
//          -> JitterBuffer -> Reassembler -> Decoder
//                                   ^ keyframe request <- feedback link <-'
//
// It is a deterministic discrete-event simulation (a logical ns clock, no real
// sockets or wall clock), so every number below reproduces run to run. It prints
// the three artifacts M3 is "done when" it can produce:
//   1. a glass-to-glass latency distribution,
//   2. an FEC recovery-vs-overhead curve,
//   3. a jitter-buffer latency-vs-stutter tradeoff.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "retina/camsim.hpp"
#include "retina/channel.hpp"
#include "retina/decoder.hpp"
#include "retina/encoder.hpp"
#include "retina/fec.hpp"
#include "retina/feedback.hpp"
#include "retina/jitter_buffer.hpp"
#include "retina/packet.hpp"

using namespace retina;

namespace {

constexpr uint32_t FPS         = 60;
constexpr uint64_t INTERVAL_NS = 1'000'000'000ull / FPS;

struct Params {
    int      num_frames       = 600;      // 10 s at 60 fps
    uint64_t seed             = 1;
    double   drop_prob        = 0.05;
    uint64_t base_latency_ns  = 2'000'000; // 2 ms
    uint64_t jitter_ns        = 3'000'000; // 3 ms
    uint64_t bandwidth_bps    = 0;         // unlimited
    bool     fec_on           = true;
    uint32_t fec_k_p          = 16;
    uint32_t fec_k_i          = 4;         // protect I harder
    uint64_t jb_base_delay_ns = 8'000'000; // 8 ms
    bool     jb_adaptive      = true;
    bool     feedback_on      = true;
    uint64_t feedback_delay_ns = 5'000'000; // one-way
};

struct Metrics {
    uint64_t frames_sent       = 0;
    uint64_t decoded           = 0;
    uint64_t undecodable       = 0;
    uint64_t lat_min           = 0;
    uint64_t lat_max           = 0;
    double   lat_mean          = 0.0;
    uint64_t stutters          = 0;
    uint64_t fec_recovered     = 0;
    double   fec_overhead      = 0.0;
    uint64_t keyframe_requests = 0;
    std::vector<uint64_t> latencies;   // ns, per decodable frame
};

Metrics run_link(const Params& p) {
    CamSim           cam(64, 64, PixelFormat::GRAY8, p.seed, FPS);
    Encoder          enc({/*gop=*/30, /*i_bytes=*/4000, /*p_bytes=*/400});
    Packetizer       pk(/*mtu=*/1200);
    FecEncoder       fec({p.fec_k_p, p.fec_k_i});
    LossyChannel     ch({p.drop_prob, p.base_latency_ns, p.jitter_ns, p.bandwidth_bps},
                        p.seed);
    FecDecoder       fdec;
    JitterBuffer     jb({p.jb_base_delay_ns, UINT64_MAX, 4.0, p.jb_adaptive});
    Reassembler      re;
    Decoder          dec;
    KeyframeRequester req;
    FeedbackLink     back(p.feedback_delay_ns);

    Metrics m;

    // Receiver pipeline: drain the channel up to now, run FEC recovery, buffer
    // for playout, reassemble, decode, and feed decode results back.
    auto receive = [&](uint64_t now) {
        for (const Packet& pkt : ch.deliver(now)) {
            std::vector<Packet> ready;
            if (!pkt.is_parity) ready.push_back(pkt);
            if (p.fec_on)
                for (const Packet& r : fdec.offer(pkt)) ready.push_back(r);
            for (const Packet& q : ready) jb.push(q, now);
        }
        for (const Packet& q : jb.pop(now)) {
            if (q.is_parity) continue;
            if (auto ef = re.offer(q)) {
                Decoder::Result r = dec.decode(*ef, now);
                if (r.decodable) m.latencies.push_back(r.latency_ns);
                if (p.feedback_on && req.observe(r, now)) back.send(now, now);
            }
        }
    };

    // Discrete-event clock stepped finely (1 ms) so the receiver processes
    // arrivals as they trickle in — coarser stepping would hide reordering that
    // the jitter buffer is meant to absorb. Frames are injected when their
    // capture time is reached.
    const uint64_t end  = static_cast<uint64_t>(p.num_frames) * INTERVAL_NS + 20 * INTERVAL_NS;
    const uint64_t step = 1'000'000;   // 1 ms receiver granularity
    int      n         = 0;
    bool     fec_flushed = false;
    bool  have_pending = false;
    Frame pending;
    for (uint64_t now = 0; now <= end; now += step) {
        // Inject every frame whose capture time has been reached.
        for (;;) {
            if (!have_pending) {
                if (n >= p.num_frames) break;
                pending      = cam.next();     // capture_ts is deterministic
                have_pending = true;
            }
            if (pending.capture_ts > now) break;   // not due yet

            const uint64_t send_ns = pending.capture_ts;
            for (uint64_t tok : back.deliver(send_ns)) { (void)tok; enc.request_keyframe(); }
            EncodedFrame ef = enc.encode(pending);
            ++m.frames_sent;
            for (const Packet& pkt : pk.packetize(ef)) {
                if (p.fec_on) { for (const Packet& w : fec.encode(pkt)) ch.send(w, send_ns); }
                else          { ch.send(pkt, send_ns); }
            }
            have_pending = false;
            ++n;
        }
        receive(now);
        if (n >= p.num_frames && !fec_flushed) {   // trailing FEC parity
            if (p.fec_on) for (const Packet& w : fec.finish()) ch.send(w, now);
            fec_flushed = true;
        }
    }

    // Drain anything still in flight after the clock ends.
    for (const Packet& pkt : ch.flush()) {
        std::vector<Packet> ready;
        if (!pkt.is_parity) ready.push_back(pkt);
        if (p.fec_on) for (const Packet& r : fdec.offer(pkt)) ready.push_back(r);
        for (const Packet& q : ready) jb.push(q, end);
    }
    for (const Packet& q : jb.flush()) {
        if (q.is_parity) continue;
        if (auto ef = re.offer(q)) {
            Decoder::Result r = dec.decode(*ef, end);
            if (r.decodable) m.latencies.push_back(r.latency_ns);
        }
    }

    m.decoded           = dec.decoded();
    m.undecodable       = dec.undecodable();
    m.lat_min           = dec.latency_min();
    m.lat_max           = dec.latency_max();
    m.lat_mean          = dec.latency_mean();
    m.stutters          = jb.stutters();
    m.fec_recovered     = fdec.recovered();
    m.fec_overhead      = fec.overhead();
    m.keyframe_requests = req.requests();
    return m;
}

// ASCII histogram of latencies (in milliseconds).
void print_latency_histogram(const std::vector<uint64_t>& ns, int buckets = 12) {
    if (ns.empty()) { std::printf("  (no decodable frames)\n"); return; }
    uint64_t lo = ns[0], hi = ns[0];
    for (uint64_t v : ns) { lo = v < lo ? v : lo; hi = v > hi ? v : hi; }
    const double lo_ms = lo / 1e6, hi_ms = hi / 1e6;
    const double width = (hi > lo) ? static_cast<double>(hi - lo) / buckets : 1.0;

    std::vector<uint64_t> counts(buckets, 0);
    for (uint64_t v : ns) {
        int b = (hi > lo) ? static_cast<int>((v - lo) / width) : 0;
        if (b >= buckets) b = buckets - 1;
        ++counts[b];
    }
    uint64_t peak = 0;
    for (uint64_t c : counts) peak = c > peak ? c : peak;

    for (int b = 0; b < buckets; ++b) {
        const double b_lo = (lo + b * width) / 1e6;
        const int bar = peak ? static_cast<int>(50.0 * counts[b] / peak) : 0;
        std::printf("  %6.2f ms | %-50.*s %llu\n", b_lo,
                    bar, "##################################################",
                    (unsigned long long)counts[b]);
    }
    std::printf("  range %.2f..%.2f ms over %zu frames\n", lo_ms, hi_ms, ns.size());
}

}  // namespace

int main() {
    // ---- Artifact 1: glass-to-glass latency distribution (full stack) ----
    std::printf("=== M3 link: full stack (5%% loss, 3 ms jitter, FEC + feedback) ===\n\n");
    Metrics base = run_link(Params{});
    std::printf("frames sent:        %llu\n", (unsigned long long)base.frames_sent);
    std::printf("decoded:            %llu\n", (unsigned long long)base.decoded);
    std::printf("undecodable:        %llu\n", (unsigned long long)base.undecodable);
    std::printf("FEC recovered pkts: %llu\n", (unsigned long long)base.fec_recovered);
    std::printf("FEC overhead:       %.1f%%\n", 100.0 * base.fec_overhead);
    std::printf("keyframe requests:  %llu\n", (unsigned long long)base.keyframe_requests);
    std::printf("jitter stutters:    %llu\n", (unsigned long long)base.stutters);
    std::printf("latency: min %.2f  mean %.2f  max %.2f ms\n\n",
                base.lat_min / 1e6, base.lat_mean / 1e6, base.lat_max / 1e6);
    std::printf("glass-to-glass latency distribution:\n");
    print_latency_histogram(base.latencies);

    // ---- Artifact 2: FEC recovery vs overhead (feedback off to isolate FEC) ----
    std::printf("\n=== FEC recovery vs overhead (8%% loss, no feedback) ===\n\n");
    std::printf("  %-4s %-10s %-9s %-12s %-10s\n",
                "k", "overhead", "decoded", "undecodable", "recovered");
    std::printf("  ---- ---------- --------- ------------ ----------\n");
    const uint32_t ks[] = {64, 32, 16, 8, 4, 2};
    for (uint32_t k : ks) {
        Params p;
        p.drop_prob   = 0.08;
        p.feedback_on = false;      // isolate FEC's contribution
        p.fec_k_p = k;
        p.fec_k_i = k;
        Metrics m = run_link(p);
        std::printf("  %-4u %-9.1f%% %-9llu %-12llu %-10llu\n",
                    k, 100.0 / k, (unsigned long long)m.decoded,
                    (unsigned long long)m.undecodable,
                    (unsigned long long)m.fec_recovered);
    }
    std::printf("  (smaller k => more parity => more overhead, fewer frames lost)\n");

    // ---- Artifact 3: jitter buffer latency vs stutter (no loss, fixed jitter) ----
    std::printf("\n=== Jitter buffer: latency vs stutter (8 ms jitter, no loss) ===\n\n");
    std::printf("  %-14s %-16s %-10s\n", "delay (ms)", "mean latency (ms)", "stutters");
    std::printf("  -------------- ---------------- ----------\n");
    const uint64_t delays_ms[] = {0, 2, 4, 8, 16, 32};
    for (uint64_t d : delays_ms) {
        Params p;
        p.drop_prob        = 0.0;
        p.jitter_ns        = 8'000'000;
        p.fec_on           = false;
        p.feedback_on      = false;
        p.jb_adaptive      = false;
        p.jb_base_delay_ns = d * 1'000'000;
        Metrics m = run_link(p);
        std::printf("  %-14llu %-16.2f %-10llu\n",
                    (unsigned long long)d, m.lat_mean / 1e6,
                    (unsigned long long)m.stutters);
    }
    std::printf("  (more delay = more latency, but fewer stutters)\n");
    return 0;
}
