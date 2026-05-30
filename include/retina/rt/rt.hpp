#pragma once
#include <cstdint>
#include <string>

#if defined(__linux__)
  #include <cerrno>
  #include <cstring>
  #include <pthread.h>
  #include <sched.h>
  #include <sys/mman.h>
#endif

namespace retina {
namespace rt {

// Real-time configuration for a periodic real-time thread — here, the video
// pipeline's compositor loop, which must emit a frame every frame-period or the
// stream stutters. The three levers a hard-RT loop needs are all Linux
// facilities: SCHED_FIFO (preempt everything else), CPU pinning to an isolated
// core (isolcpus), and mlockall (no page faults on the hot path). They are
// guarded under __linux__ and become no-ops elsewhere, so the code compiles and
// runs everywhere (best-effort off Linux) while being correct for the Linux/VM
// where the RT numbers actually matter.

struct Config {
    int  fifo_priority = 80;     // SCHED_FIFO priority (1..99); higher = wins
    int  cpu           = -1;     // pin to this core; -1 = don't pin
    bool lock_memory   = true;   // mlockall(MCL_CURRENT|MCL_FUTURE)
};

struct Report {
    bool        platform_linux = false;
    bool        realtime_ok    = false;
    bool        affinity_ok    = false;
    bool        memlock_ok     = false;
    std::string realtime_msg;
    std::string affinity_msg;
    std::string memlock_msg;
};

// Apply the RT config to the CALLING thread; report what actually took effect.
inline Report configure(const Config& cfg) {
    Report r;
#if defined(__linux__)
    r.platform_linux = true;

    sched_param sp{};
    sp.sched_priority = cfg.fifo_priority;
    const int src = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    r.realtime_ok  = (src == 0);
    r.realtime_msg = src == 0 ? "SCHED_FIFO set" : std::strerror(src);

    if (cfg.cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cfg.cpu, &set);
        const int arc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
        r.affinity_ok  = (arc == 0);
        r.affinity_msg = arc == 0 ? "pinned to core" : std::strerror(arc);
    } else {
        r.affinity_msg = "not requested";
    }

    if (cfg.lock_memory) {
        const int mrc = mlockall(MCL_CURRENT | MCL_FUTURE);
        r.memlock_ok  = (mrc == 0);
        r.memlock_msg = mrc == 0 ? "mlockall ok" : std::strerror(errno);
    } else {
        r.memlock_msg = "not requested";
    }
#else
    (void)cfg;
    r.realtime_msg = "SCHED_FIFO is Linux-only (running best-effort)";
    r.affinity_msg = "CPU pinning is Linux-only";
    r.memlock_msg  = "memory locking skipped off Linux";
#endif
    return r;
}

// JitterMeter: measures how well a periodic loop hits its target period. Fed one
// timestamp per iteration, it tracks the deviation of each actual inter-tick
// interval from the target — that spread IS the deadline jitter you tighten in
// M5. Clock-agnostic (takes ns timestamps), so it's unit-testable with synthetic
// samples and fed the real steady_clock in the demo.
class JitterMeter {
public:
    explicit JitterMeter(uint64_t target_period_ns) : target_(target_period_ns) {}

    void record(uint64_t now_ns) {
        if (have_prev_) {
            const int64_t err = static_cast<int64_t>(now_ns - prev_) -
                                static_cast<int64_t>(target_);
            if (err > 0) ++late_;                       // woke later than target
            const uint64_t a = err < 0 ? static_cast<uint64_t>(-err)
                                       : static_cast<uint64_t>(err);
            ++count_;
            sum_abs_ += a;
            if (a > max_abs_) max_abs_ = a;
            if (a < min_abs_) min_abs_ = a;
            for (int i = 0; i < kBuckets; ++i)
                if (a < kEdges[i]) { ++buckets_[i]; break; }
        }
        prev_      = now_ns;
        have_prev_ = true;
    }

    uint64_t samples()  const { return count_; }
    uint64_t late()     const { return late_; }
    uint64_t min_ns()   const { return count_ ? min_abs_ : 0; }
    uint64_t max_ns()   const { return max_abs_; }
    double   mean_ns()  const {
        return count_ ? static_cast<double>(sum_abs_) / static_cast<double>(count_) : 0.0;
    }

    static constexpr int kBuckets = 7;
    // Upper bounds (ns): 10us, 50us, 100us, 500us, 1ms, 5ms, infinity.
    static constexpr uint64_t kEdges[kBuckets] = {
        10'000, 50'000, 100'000, 500'000, 1'000'000, 5'000'000, UINT64_MAX};
    uint64_t bucket(int i) const { return buckets_[i]; }

private:
    uint64_t target_;
    bool     have_prev_ = false;
    uint64_t prev_      = 0;
    uint64_t count_     = 0;
    uint64_t late_      = 0;
    uint64_t sum_abs_   = 0;
    uint64_t max_abs_   = 0;
    uint64_t min_abs_   = UINT64_MAX;
    uint64_t buckets_[kBuckets] = {};
};

} // namespace rt
} // namespace retina
