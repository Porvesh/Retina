#pragma once
#include <cstdint>

namespace retina {

enum class PixelFormat : uint8_t { GRAY8, RGB8, NV12, BAYER_RGGB };

// Plain, trivially-copyable data. No ownership lives here: `data` is a BORROWED
// view into a buffer slot's storage, valid only between latest() and release().
// The slot (in the buffer) owns the bytes and keeps them alive via a refcount.
struct Frame {
    uint64_t       seq        = 0;        // monotonic; a gap = a dropped frame
    uint64_t       capture_ts = 0;        // ns, monotonic clock at "exposure"
    const uint8_t* data       = nullptr;  // borrowed; valid only until release()
    uint32_t       size       = 0;        // bytes at `data`
    uint32_t       width      = 0;
    uint32_t       height     = 0;
    PixelFormat    format     = PixelFormat::GRAY8;
};

} // namespace retina
