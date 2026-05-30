// M5 "hard-RT video thread" demo — the real-time discipline, applied to the
// video pipeline itself.
//
//   3 CamSims --> composite into a CPU Canvas --> one frame every 1/FPS s,
//   on a thread configured via rt::configure() (SCHED_FIFO + CPU pin + mlock),
//   with a JitterMeter recording how far each frame slips from its deadline.
//
// This is M4's compositor turned into a PERIODIC REAL-TIME TASK. A live video
// pipeline's hot loop must emit a frame every frame-period or the stream
// stutters — the same deadline discipline a machine-control loop needs. The
// three RT levers are real only on Linux; off Linux this runs best-effort and
// the histogram simply shows what a non-RT OS gives you (the number M5 exists
// to tighten on Linux with PREEMPT_RT / isolcpus).
//
// No hot-path allocation: the Canvas is allocated once and reused every frame,
// so the loop never touches the heap after warm-up — the RT-correct discipline.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "retina/rt/rt.hpp"
#include "retina/sim/camsim.hpp"
#include "retina/viz/canvas.hpp"

using namespace retina;
using Clock = std::chrono::steady_clock;

namespace {

uint64_t ns_since(Clock::time_point t0) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
}

// Composite the three camera views + a HUD into the (reused) canvas. This is the
// per-frame CPU work the real-time loop must finish inside its deadline.
void compose(Canvas& c, const Frame& f0, const Frame& f1, const Frame& f2, int frame) {
    c.fill(18, 18, 28);
    c.blit_gray(f0, 8, 8, 336, 300);
    c.border(8, 8, 336, 300, 90, 90, 120);
    c.draw_text(14, 14, "CAM 0", 240, 240, 120, 2);

    c.blit_gray(f1, 352, 8, 280, 146);
    c.border(352, 8, 280, 146, 90, 90, 120);
    c.draw_text(358, 12, "CAM 1", 240, 240, 120, 1);

    c.blit_gray(f2, 352, 162, 280, 146);
    c.border(352, 162, 280, 146, 90, 90, 120);
    c.draw_text(358, 166, "CAM 2", 240, 240, 120, 1);

    c.rect(0, 316, 640, 44, 8, 8, 14);
    char line[128];
    std::snprintf(line, sizeof(line), "RETINA M5  RT VIDEO THREAD  FRAME %04d", frame);
    c.draw_text(8, 322, line, 120, 220, 160, 2);
}

}  // namespace

int main() {
    constexpr uint32_t W = 96, H = 96;
    constexpr uint32_t FPS = 60;                       // 60 Hz => 16.667 ms budget
    constexpr uint64_t PERIOD_NS = 1'000'000'000ull / FPS;
    constexpr int      FRAMES = 600;                   // ~10 s of real-time frames
    constexpr auto     PERIOD = std::chrono::nanoseconds(PERIOD_NS);

    CamSim cam0(W, H, PixelFormat::GRAY8, /*seed=*/1, FPS, {0, 0, /*drift=*/0});
    CamSim cam1(W, H, PixelFormat::GRAY8, /*seed=*/2, FPS, {0, 0, /*drift=*/+200});
    CamSim cam2(W, H, PixelFormat::GRAY8, /*seed=*/3, FPS, {0, 0, /*drift=*/-200});

    Canvas canvas(640, 360);                           // allocated ONCE, reused
    rt::Report        rep;
    rt::JitterMeter   meter(PERIOD_NS);
    uint64_t          last_checksum = 0;

    const Clock::time_point T0 = Clock::now();

    std::thread render([&] {
        rep = rt::configure({/*fifo_priority=*/80, /*cpu=*/-1, /*lock_memory=*/true});

        Clock::time_point next = Clock::now();
        for (int i = 0; i < FRAMES; ++i) {
            next += PERIOD;
            std::this_thread::sleep_until(next);
            meter.record(ns_since(T0));                // when did this frame actually start?

            Frame f0 = cam0.next();
            Frame f1 = cam1.next();
            Frame f2 = cam2.next();
            compose(canvas, f0, f1, f2, i);            // the deadline-bound CPU work
        }
        last_checksum = canvas.checksum();             // prove the work really happened
    });
    render.join();

    // ---- report ----
    std::printf("=== M5 hard-RT video thread: compositor @ %u fps (%.3f ms budget) ===\n\n",
                FPS, PERIOD_NS / 1e6);

    std::printf("RT config on this platform (%s):\n",
                rep.platform_linux ? "Linux" : "non-Linux, best-effort");
    std::printf("  SCHED_FIFO : %s\n", rep.realtime_msg.c_str());
    std::printf("  affinity   : %s\n", rep.affinity_msg.c_str());
    std::printf("  memlock    : %s\n", rep.memlock_msg.c_str());
    std::printf("  (hard-RT guarantees need Linux: SCHED_FIFO + isolcpus + mlockall)\n\n");

    std::printf("Deadline jitter over %llu frames (target %llu us):\n",
                (unsigned long long)meter.samples(), (unsigned long long)(PERIOD_NS / 1000));
    std::printf("  min %.1f  mean %.1f  max %.1f us     late %llu/%llu frames\n",
                meter.min_ns() / 1000.0, meter.mean_ns() / 1000.0, meter.max_ns() / 1000.0,
                (unsigned long long)meter.late(), (unsigned long long)meter.samples());
    const char* labels[rt::JitterMeter::kBuckets] = {
        "  <10us ", " <50us ", "<100us ", "<500us ", "  <1ms ", "  <5ms ", " >=5ms "};
    uint64_t peak = 1;
    for (int b = 0; b < rt::JitterMeter::kBuckets; ++b)
        peak = meter.bucket(b) > peak ? meter.bucket(b) : peak;
    for (int b = 0; b < rt::JitterMeter::kBuckets; ++b) {
        const int bar = static_cast<int>(40.0 * meter.bucket(b) / peak);
        std::printf("  %s | %-40.*s %llu\n", labels[b], bar,
                    "########################################",
                    (unsigned long long)meter.bucket(b));
    }

    std::printf("\nComposited %d frames, zero hot-path allocations (Canvas reused).\n", FRAMES);
    std::printf("  final frame checksum: %016llx\n", (unsigned long long)last_checksum);
    std::printf("  off Linux the tail buckets are the OS preempting us; on Linux with\n");
    std::printf("  SCHED_FIFO + isolcpus + mlockall the whole histogram collapses left.\n");
    return 0;
}
