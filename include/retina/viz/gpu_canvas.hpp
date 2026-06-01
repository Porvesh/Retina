#pragma once
#include <cstdint>
#include <string>

#include "retina/core/frame.hpp"

namespace retina {

// GpuCanvas: a GPU-backed twin of Canvas (see viz/canvas.hpp).
//
// Same drawing surface, same public API, byte-identical output — but the RGB
// framebuffer lives in H100 device memory and every operation runs as a CUDA
// kernel (one GPU thread per output pixel) instead of a CPU loop. This is the
// M4 compositor's "decode -> GPU textures -> blend" stage made real, restoring
// the part of the original plan that was cut when there was no GPU.
//
// The interface is deliberately header-clean (no CUDA types leak out) so plain
// C++ can include it; the kernels and device bookkeeping live in gpu_canvas.cu.
// write_ppm()/checksum() copy the finished frame back to the host, so the GPU
// path drops into the exact same .ppm-and-checksum flow the CPU Canvas uses.
class GpuCanvas {
public:
    GpuCanvas(int width, int height);
    ~GpuCanvas();

    GpuCanvas(const GpuCanvas&)            = delete;   // owns a device allocation
    GpuCanvas& operator=(const GpuCanvas&) = delete;

    int width()  const { return w_; }
    int height() const { return h_; }

    void fill(uint8_t r, uint8_t g, uint8_t b);
    void rect(int x, int y, int rw, int rh, uint8_t r, uint8_t g, uint8_t b);
    void border(int x, int y, int rw, int rh, uint8_t r, uint8_t g, uint8_t b);
    void blit_gray(const Frame& f, int x, int y, int dst_w, int dst_h);
    void draw_char(int x, int y, char c, uint8_t r, uint8_t g, uint8_t b, int scale = 1);
    void draw_text(int x, int y, const std::string& s, uint8_t r, uint8_t g, uint8_t b,
                   int scale = 1);

    bool     write_ppm(const std::string& path) const;
    uint64_t checksum() const;

private:
    void sync_to_host() const;   // device framebuffer -> host mirror

    int      w_, h_;
    uint8_t* d_px_    = nullptr;  // RGB8 framebuffer, device memory
    uint8_t* d_src_   = nullptr;  // reusable upload scratch for blit sources
    size_t   src_cap_ = 0;        // bytes currently allocated at d_src_
};

} // namespace retina
