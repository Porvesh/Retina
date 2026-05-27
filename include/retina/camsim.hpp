#pragma once
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "retina/frame.hpp"

namespace retina {

// CamSim: a synthetic camera — the producer the rest of Retina was missing.
// Emits Frames on demand via next(), DETERMINISTICALLY from a seed: the same
// (seed, faults, frame index) always yields the same stream, so tests and the
// M1 latency-vs-completeness probe are reproducible run to run.
//
// V4L2-style borrow: next() returns a Frame whose `data` points into CamSim's
// own buffer, valid only until the NEXT call to next(). Copy it if you need to
// keep it — which is exactly what FrameBuffer::publish() does. next() performs
// no allocation (the buffer is sized once in the ctor), matching the project's
// RT-safe discipline.
//
// Fault injection (both off by default):
//   - drop_rate: fraction of capture ticks dropped AT CAPTURE. A dropped tick
//     still consumes its frame number, so emitted frames show a seq GAP —
//     exactly how a downstream consumer detects capture loss. (This is what
//     M0's "inject a 10% drop rate" needs.)
//   - max_jitter_ns: capture_ts gets a bounded random offset, modelling
//     delivery jitter. Keep it below the frame interval to keep timestamps
//     monotonic.
//
// Timestamps are SIMULATED (tick * frame interval, + jitter), never read from a
// clock, so output depends only on the seed, not on wall-clock timing.
//
// Note: this next() is a frame SOURCE and is unrelated to FrameBuffer::next()
// (the ring's FIFO consumer verb) — different type, different job.
class CamSim {
public:
    struct Faults {
        double   drop_rate     = 0.0;   // P(a given tick is dropped at capture), [0,1)
        uint64_t max_jitter_ns = 0;     // capture_ts += random offset in [0, max_jitter_ns]
    };

    // Full constructor (with faults).
    CamSim(uint32_t width, uint32_t height, PixelFormat format,
           uint64_t seed, uint32_t fps, Faults faults)
        : width_(width),
          height_(height),
          format_(format),
          seed_(seed),
          frame_interval_ns_(fps ? 1'000'000'000ull / fps : 0),
          faults_(faults),
          rng_(seed),
          storage_(bytes_for(width, height, format)) {}

    // Convenience constructor (no faults). Delegates rather than using a
    // defaulted Faults argument, which C++ forbids here: the nested Faults'
    // in-class member initializers can't be used to default an argument inside
    // the enclosing class definition.
    CamSim(uint32_t width, uint32_t height, PixelFormat format,
           uint64_t seed = 0, uint32_t fps = 30)
        : CamSim(width, height, format, seed, fps, Faults{}) {}

    // Produce the next EMITTED frame. Frame N depends only on (seed, faults, N).
    Frame next() {
        // Advance through capture ticks, skipping any dropped at capture.
        for (;;) {
            ++tick_;
            if (faults_.drop_rate > 0.0 && next_unit() < faults_.drop_rate)
                continue;   // dropped at capture — its frame number is still spent
            break;
        }
        ++emitted_;

        // Scheduled exposure time for this tick, plus optional bounded jitter.
        uint64_t ts = tick_ * frame_interval_ns_;
        if (faults_.max_jitter_ns > 0)
            ts += static_cast<uint64_t>(next_unit() *
                                        static_cast<double>(faults_.max_jitter_ns + 1));

        // Deterministic synthetic content: a gradient whose base shifts by one
        // each frame, so the image "moves" and a consumer can both see motion
        // and verify the frame is internally consistent:
        //   data[0] == (seq + seed) & 0xFF ,  data[i] == data[0] + i
        const uint8_t base = static_cast<uint8_t>((tick_ + seed_) & 0xFF);
        for (std::size_t i = 0; i < storage_.size(); ++i)
            storage_[i] = static_cast<uint8_t>(base + static_cast<uint8_t>(i));

        Frame f;
        f.seq        = tick_;      // true frame number (gaps appear when drops occur)
        f.capture_ts = ts;
        f.data       = storage_.data();
        f.size       = static_cast<uint32_t>(storage_.size());
        f.width      = width_;
        f.height     = height_;
        f.format     = format_;
        return f;
    }

    uint64_t frames_emitted() const { return emitted_; }        // frames returned by next()
    uint64_t frames_dropped() const { return tick_ - emitted_; } // dropped at capture so far

private:
    // Uniform [0,1) drawn from the engine directly (portable-deterministic,
    // unlike std::uniform_real_distribution). 53 random bits → double mantissa.
    double next_unit() {
        return static_cast<double>(rng_() >> 11) * (1.0 / 9007199254740992.0);
    }

    static std::size_t bytes_for(uint32_t w, uint32_t h, PixelFormat fmt) {
        const std::size_t px = static_cast<std::size_t>(w) * h;
        switch (fmt) {
            case PixelFormat::GRAY8:      return px;          // 8 bpp
            case PixelFormat::RGB8:       return px * 3;      // 24 bpp
            case PixelFormat::NV12:       return px + px / 2; // 12 bpp (4:2:0)
            case PixelFormat::BAYER_RGGB: return px;          // 8 bpp mosaic
        }
        return px;
    }

    uint32_t        width_;
    uint32_t        height_;
    PixelFormat     format_;
    uint64_t        seed_;
    uint64_t        frame_interval_ns_;
    Faults          faults_;
    std::mt19937_64 rng_;

    uint64_t             tick_    = 0;   // capture ticks so far (incl. dropped)
    uint64_t             emitted_ = 0;   // frames actually returned
    std::vector<uint8_t> storage_;       // the buffer next() views into
};

} // namespace retina
