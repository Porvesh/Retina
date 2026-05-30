// M0 "spine" demo — the first end-to-end runnable in Retina.
//
//   CamSim (10% capture drop) --publish--> LatestValue --latest()--> slow consumer
//
// It shows M0's "done when": frames flow, a 10% drop rate is injected, and the
// consumer SKIPS STALE frames while the producer NEVER blocks. The producer runs
// faster than the consumer, so the lock-free LatestValue keeps handing the
// consumer the newest frame and lets the stale ones be recycled — and we measure
// the producer's worst publish() latency to show it never waited on the consumer.
//
// This is a demo, not a test: it uses real threads and the wall clock.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "retina/sim/camsim.hpp"
#include "retina/buffers/latest_value.hpp"

using namespace retina;
using Clock = std::chrono::steady_clock;

namespace {
uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count());
}
}  // namespace

int main() {
    // ---- demo configuration ----
    constexpr uint32_t W = 64, H = 64;
    constexpr uint32_t FPS       = 120;
    constexpr uint64_t SEED      = 1;
    constexpr double   DROP_RATE = 0.10;   // 10% capture loss
    constexpr int      NUM_FRAMES = 240;   // ~2 s at 120 fps
    // Producer runs at the camera's real frame rate, so wall time and the
    // simulated capture_ts share a clock. The consumer is deliberately ~5x
    // slower, so it must skip stale frames to keep up with "now".
    constexpr auto PRODUCER_PERIOD = std::chrono::nanoseconds(1'000'000'000ull / FPS);
    constexpr auto CONSUMER_PERIOD = std::chrono::milliseconds(40);

    CamSim      cam(W, H, PixelFormat::GRAY8, SEED, FPS, {DROP_RATE, /*jitter=*/0});
    LatestValue buf(/*consumers=*/1, static_cast<std::size_t>(W) * H);

    std::atomic<bool>     done{false};
    std::atomic<uint64_t> max_publish_ns{0};
    std::atomic<uint64_t> published{0};
    uint64_t              consumed_total = 0;   // written by consumer, read after join

    std::printf("M0 spine: CamSim(%u%% drop) -> LatestValue -> slow consumer\n\n",
                static_cast<unsigned>(DROP_RATE * 100));
    std::printf("  %-4s %-10s %-9s %-14s %-16s\n",
                "#", "frame.seq", "skipped", "wall dt (ms)", "capture dt (ms)");
    std::printf("  ---- ---------- --------- -------------- ----------------\n");

    // ---- producer: capture -> publish, never blocking ----
    std::thread producer([&] {
        for (int i = 0; i < NUM_FRAMES; ++i) {
            Frame f = cam.next();                    // may skip dropped ticks (seq gaps)

            const uint64_t t0 = now_ns();
            buf.publish(f);                          // lock-free, never blocks
            const uint64_t dt = now_ns() - t0;

            uint64_t prev = max_publish_ns.load(std::memory_order_relaxed);
            while (dt > prev &&
                   !max_publish_ns.compare_exchange_weak(prev, dt,
                                                         std::memory_order_relaxed)) {}
            published.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(PRODUCER_PERIOD);
        }
        done.store(true, std::memory_order_release);
    });

    // ---- consumer: slow; always sees newest, skips stale ----
    std::thread consumer([&] {
        uint64_t last_seq = 0, last_capture = 0, last_wall = now_ns();
        int      consumed = 0;

        auto take = [&](bool print) {
            FrameHandle h = buf.latest();
            if (!h.valid() || h.frame().seq == last_seq) return;
            const uint64_t seq     = h.frame().seq;
            const uint64_t skipped = (last_seq == 0) ? 0 : (seq - last_seq - 1);
            const uint64_t wall    = now_ns();
            const double   wall_ms = (wall - last_wall) / 1e6;
            const double   cap_ms  = (last_capture == 0) ? 0.0
                                   : static_cast<double>(h.frame().capture_ts - last_capture) / 1e6;
            if (print)
                std::printf("  %-4d %-10llu %-9llu %-14.2f %-16.2f\n",
                            ++consumed, (unsigned long long)seq,
                            (unsigned long long)skipped, wall_ms, cap_ms);
            else
                ++consumed;
            last_seq     = seq;
            last_capture = h.frame().capture_ts;
            last_wall    = wall;
        };

        for (;;) {
            const bool d = done.load(std::memory_order_acquire);
            take(/*print=*/true);
            if (d) { take(/*print=*/true); break; }   // final drain of the last frame
            std::this_thread::sleep_for(CONSUMER_PERIOD);
        }
        consumed_total = static_cast<uint64_t>(consumed);
    });

    producer.join();
    consumer.join();

    // ---- summary (safe to read cam/atomics after join) ----
    const uint64_t emitted = cam.frames_emitted();
    const uint64_t dropped = cam.frames_dropped();
    const uint64_t ticks   = emitted + dropped;

    std::printf("\nSummary\n");
    std::printf("  camera ticks:        %llu\n", (unsigned long long)ticks);
    std::printf("  dropped at capture:  %llu (%.1f%%)\n",
                (unsigned long long)dropped, 100.0 * (double)dropped / (double)ticks);
    std::printf("  published to buffer: %llu\n", (unsigned long long)published.load());
    std::printf("  consumed (slow):     %llu\n", (unsigned long long)consumed_total);
    std::printf("  skipped as stale:    %llu  <- LatestValue recycled these\n",
                (unsigned long long)(published.load() - consumed_total));
    std::printf("  max publish latency: %.1f us  <- producer never blocked on the consumer\n",
                (double)max_publish_ns.load() / 1000.0);
    return 0;
}
