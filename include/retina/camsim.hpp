#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "retina/frame.hpp"

namespace retina {

// CamSim: a synthetic camera — the producer the rest of Retina was missing.
// Emits Frames on demand via next(), DETERMINISTICALLY from a seed: the same
// (seed, frame index) always yields the same frame, so tests and the M1
// latency-vs-completeness probe are reproducible run to run.
//
// V4L2-style borrow: next() returns a Frame whose `data` points into CamSim's
// own buffer, valid only until the NEXT call to next(). Copy it if you need to
// keep it — which is exactly what FrameBuffer::publish() does. next() performs
// no allocation (the buffer is sized once in the ctor), matching the project's
// RT-safe discipline.
//
// Note: this next() is a frame SOURCE and is unrelated to FrameBuffer::next()
// (the ring's FIFO consumer verb) — different type, different job.
//
// Timestamps are SIMULATED (a fixed frame interval), not read from a clock, so
// output depends only on the seed, never on wall-clock timing.
class CamSim {
public:
    CamSim(uint32_t width, uint32_t height, PixelFormat format,
           uint64_t seed = 0, uint32_t fps = 30)
        : width_(width),
          height_(height),
          format_(format),
          seed_(seed),
          frame_interval_ns_(fps ? 1'000'000'000ull / fps : 0),
          storage_(bytes_for(width, height, format)) {}

    // Produce the next frame. Frame N (1-based) depends only on (seed, N).
    Frame next() {
        ++seq_;                              // 1-based monotonic sequence
        capture_ts_ += frame_interval_ns_;   // simulated exposure time

        // Deterministic synthetic content: a gradient whose base shifts by one
        // each frame, so the image "moves" and a consumer can both see motion
        // and verify the frame is internally consistent:
        //   data[0] == (seq + seed) & 0xFF ,  data[i] == data[0] + i
        const uint8_t base = static_cast<uint8_t>((seq_ + seed_) & 0xFF);
        for (std::size_t i = 0; i < storage_.size(); ++i)
            storage_[i] = static_cast<uint8_t>(base + static_cast<uint8_t>(i));

        Frame f;
        f.seq        = seq_;
        f.capture_ts = capture_ts_;
        f.data       = storage_.data();
        f.size       = static_cast<uint32_t>(storage_.size());
        f.width      = width_;
        f.height     = height_;
        f.format     = format_;
        return f;
    }

    // Frames emitted so far (== the seq of the most recent frame).
    uint64_t frames_emitted() const { return seq_; }

private:
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

    uint32_t    width_;
    uint32_t    height_;
    PixelFormat format_;
    uint64_t    seed_;
    uint64_t    frame_interval_ns_;

    uint64_t             seq_        = 0;   // frames emitted so far
    uint64_t             capture_ts_ = 0;   // simulated ns clock
    std::vector<uint8_t> storage_;          // the buffer next() views into
};

} // namespace retina
