// gpu_canvas.cu — CUDA implementation of GpuCanvas (see viz/gpu_canvas.hpp).
//
// Every kernel here is the exact per-pixel math of the matching Canvas method,
// with its for-loops replaced by "which pixel am I?". The clip rule (silently
// drop out-of-bounds writes) matches Canvas::set, so the two backends produce
// byte-for-byte identical framebuffers — which the m4_gpu demo checks via the
// shared checksum().

#include "retina/viz/gpu_canvas.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

#include <cuda_runtime.h>

#include "retina/viz/font5x7.hpp"

namespace retina {
namespace {

inline void cuda_die(cudaError_t e, const char* what) {
    if (e != cudaSuccess) {
        std::fprintf(stderr, "GpuCanvas CUDA error (%s): %s\n", what, cudaGetErrorString(e));
        std::abort();
    }
}
#define CU(x) cuda_die((x), #x)

// ---- kernels: one GPU thread per output pixel, clip like Canvas::set --------

__global__ void k_fill(uint8_t* px, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;
    size_t i = (size_t(y) * w + x) * 3;
    px[i] = r; px[i + 1] = g; px[i + 2] = b;
}

// Fills a rectangle; each thread owns one pixel of the rw x rh region.
__global__ void k_rect(uint8_t* px, int w, int h, int rx, int ry, int rw, int rh,
                       uint8_t r, uint8_t g, uint8_t b) {
    int dx = blockIdx.x * blockDim.x + threadIdx.x;
    int dy = blockIdx.y * blockDim.y + threadIdx.y;
    if (dx >= rw || dy >= rh) return;
    int x = rx + dx, y = ry + dy;
    if (x < 0 || y < 0 || x >= w || y >= h) return;   // == Canvas::set clip
    size_t i = (size_t(y) * w + x) * 3;
    px[i] = r; px[i + 1] = g; px[i + 2] = b;
}

// Nearest-neighbour GRAY8 -> RGB blit; identical integer math to Canvas::blit_gray.
__global__ void k_blit_gray(uint8_t* px, int w, int h,
                            const uint8_t* src, int sw, int sh,
                            int ox, int oy, int dw, int dh) {
    int dx = blockIdx.x * blockDim.x + threadIdx.x;
    int dy = blockIdx.y * blockDim.y + threadIdx.y;
    if (dx >= dw || dy >= dh) return;
    unsigned sy = unsigned(dy) * unsigned(sh) / unsigned(dh);
    unsigned sx = unsigned(dx) * unsigned(sw) / unsigned(dw);
    uint8_t v = src[size_t(sy) * sw + sx];
    int x = ox + dx, y = oy + dy;
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    size_t i = (size_t(y) * w + x) * 3;
    px[i] = v; px[i + 1] = v; px[i + 2] = v;
}

// One scaled 5x7 glyph. `rows` packs the 7 font bytes (row r in bits [8r,8r+8)).
// Matches Canvas::draw_char, which fills a scale x scale block per set bit.
__global__ void k_glyph(uint8_t* px, int w, int h, int ox, int oy,
                        unsigned long long rows, int scale,
                        uint8_t r, uint8_t g, uint8_t b) {
    int sxp = blockIdx.x * blockDim.x + threadIdx.x;   // subpixel within glyph
    int syp = blockIdx.y * blockDim.y + threadIdx.y;
    if (sxp >= 5 * scale || syp >= 7 * scale) return;
    int col = sxp / scale, row = syp / scale;
    uint8_t rowbits = uint8_t((rows >> (8 * row)) & 0xFF);
    if (!(rowbits & (1 << (4 - col)))) return;
    int x = ox + sxp, y = oy + syp;
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    size_t i = (size_t(y) * w + x) * 3;
    px[i] = r; px[i + 1] = g; px[i + 2] = b;
}

inline dim3 grid2d(int nx, int ny, dim3 block) {
    return dim3((nx + block.x - 1) / block.x, (ny + block.y - 1) / block.y);
}

}  // namespace

GpuCanvas::GpuCanvas(int width, int height) : w_(width), h_(height) {
    CU(cudaMalloc(&d_px_, size_t(w_) * h_ * 3));
    CU(cudaMemset(d_px_, 0, size_t(w_) * h_ * 3));   // Canvas starts all-zero
}

GpuCanvas::~GpuCanvas() {
    if (d_px_)  cudaFree(d_px_);
    if (d_src_) cudaFree(d_src_);
}

void GpuCanvas::fill(uint8_t r, uint8_t g, uint8_t b) {
    dim3 block(16, 16);
    k_fill<<<grid2d(w_, h_, block), block>>>(d_px_, w_, h_, r, g, b);
    CU(cudaGetLastError());
}

void GpuCanvas::rect(int x, int y, int rw, int rh, uint8_t r, uint8_t g, uint8_t b) {
    if (rw <= 0 || rh <= 0) return;
    dim3 block(16, 16);
    k_rect<<<grid2d(rw, rh, block), block>>>(d_px_, w_, h_, x, y, rw, rh, r, g, b);
    CU(cudaGetLastError());
}

void GpuCanvas::border(int x, int y, int rw, int rh, uint8_t r, uint8_t g, uint8_t b) {
    // Same four edges Canvas::border draws (corners double-written, harmless).
    rect(x, y, rw, 1, r, g, b);
    rect(x, y + rh - 1, rw, 1, r, g, b);
    rect(x, y, 1, rh, r, g, b);
    rect(x + rw - 1, y, 1, rh, r, g, b);
}

void GpuCanvas::blit_gray(const Frame& f, int x, int y, int dst_w, int dst_h) {
    if (f.data == nullptr || f.width == 0 || f.height == 0) return;   // == Canvas
    if (dst_w <= 0 || dst_h <= 0) return;
    if (f.size > src_cap_) {                     // grow reusable upload scratch
        if (d_src_) cudaFree(d_src_);
        CU(cudaMalloc(&d_src_, f.size));
        src_cap_ = f.size;
    }
    CU(cudaMemcpy(d_src_, f.data, f.size, cudaMemcpyHostToDevice));
    dim3 block(16, 16);
    k_blit_gray<<<grid2d(dst_w, dst_h, block), block>>>(
        d_px_, w_, h_, d_src_, int(f.width), int(f.height), x, y, dst_w, dst_h);
    CU(cudaGetLastError());
}

void GpuCanvas::draw_char(int x, int y, char c, uint8_t r, uint8_t g, uint8_t b, int scale) {
    const uint8_t* gl = glyph5x7(c);
    unsigned long long rows = 0;
    for (int row = 0; row < 7; ++row)
        rows |= (unsigned long long)(gl[row]) << (8 * row);
    dim3 block(8, 8);
    k_glyph<<<grid2d(5 * scale, 7 * scale, block), block>>>(
        d_px_, w_, h_, x, y, rows, scale, r, g, b);
    CU(cudaGetLastError());
}

void GpuCanvas::draw_text(int x, int y, const std::string& s, uint8_t r, uint8_t g,
                          uint8_t b, int scale) {
    int cx = x;
    for (char c : s) {
        draw_char(cx, y, c, r, g, b, scale);
        cx += 6 * scale;   // 5px glyph + 1px gap, same as Canvas
    }
}

void GpuCanvas::sync_to_host() const {
    // caller-managed host mirror is created per call — write_ppm/checksum are cold.
}

bool GpuCanvas::write_ppm(const std::string& path) const {
    CU(cudaDeviceSynchronize());
    std::vector<uint8_t> host(size_t(w_) * h_ * 3);
    CU(cudaMemcpy(host.data(), d_px_, host.size(), cudaMemcpyDeviceToHost));
    std::FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;
    std::fprintf(fp, "P6\n%d %d\n255\n", w_, h_);
    std::fwrite(host.data(), 1, host.size(), fp);
    std::fclose(fp);
    return true;
}

uint64_t GpuCanvas::checksum() const {
    CU(cudaDeviceSynchronize());
    std::vector<uint8_t> host(size_t(w_) * h_ * 3);
    CU(cudaMemcpy(host.data(), d_px_, host.size(), cudaMemcpyDeviceToHost));
    uint64_t h = 1469598103934665603ull;   // FNV-1a, identical to Canvas
    for (uint8_t byte : host) { h ^= byte; h *= 1099511628211ull; }
    return h;
}

} // namespace retina
