// m4_gpu — the M4 compositor run on the GPU, verified byte-for-byte vs the CPU.
//
// This runs the EXACT M4 scene (3 drifting CamSims -> StreamAligner -> primary +
// thumbnails + HUD) twice: once through the CPU `Canvas`, once through the
// device-backed `GpuCanvas`. Both are deterministic and seeded identically, so a
// correct GPU backend must reproduce every frame bit-for-bit. We compare the
// per-frame FNV checksum() of the two backends and write the GPU frames to disk.
//
// A single template `draw_scene` draws into whichever canvas it's given, so the
// two runs share one code path — the only difference is where the pixels live.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "retina/align/stream_aligner.hpp"
#include "retina/sim/camsim.hpp"
#include "retina/viz/canvas.hpp"
#include "retina/viz/gpu_canvas.hpp"

using namespace retina;

namespace {

constexpr uint32_t W = 96, H = 96;
constexpr uint32_t FPS = 30;
constexpr int      MAX_FRAMES = 90;

std::string fmt2(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

// The M4 scene, drawn into any canvas exposing the shared drawing API.
template <class C>
void draw_scene(C& c, const Frame& v0, const Frame& v1, const Frame& v2,
                int written, long skew_ns, unsigned long long emitted,
                unsigned long long rejected) {
    c.fill(18, 18, 28);

    c.blit_gray(v0, 8, 8, 336, 300);
    c.border(8, 8, 336, 300, 90, 90, 120);
    c.draw_text(14, 14, "CAM 0", 240, 240, 120, 2);

    c.blit_gray(v1, 352, 8, 280, 146);
    c.border(352, 8, 280, 146, 90, 90, 120);
    c.draw_text(358, 12, "CAM 1", 240, 240, 120, 1);

    c.blit_gray(v2, 352, 162, 280, 146);
    c.border(352, 162, 280, 146, 90, 90, 120);
    c.draw_text(358, 166, "CAM 2", 240, 240, 120, 1);

    c.rect(0, 316, 640, 44, 8, 8, 14);
    char line[128];
    std::snprintf(line, sizeof(line), "RETINA M4  FRAME %03d  CAMS 3", written);
    c.draw_text(8, 322, line, 120, 220, 160, 2);
    const std::string hud2 =
        "SYNC " + fmt2(skew_ns / 1e6) + " MS   ALIGNED " +
        std::to_string(emitted) + "/" + std::to_string(emitted + rejected);
    c.draw_text(8, 340, hud2, 120, 220, 160, 2);
}

// Run the whole M4 pipeline through canvas type C; return each frame's checksum.
// If out_dir is non-empty, also write the composited .ppm sequence there.
template <class C>
std::vector<uint64_t> run(const std::string& out_dir) {
    CamSim cam0(W, H, PixelFormat::GRAY8, 1, FPS, {0, 0, 0});
    CamSim cam1(W, H, PixelFormat::GRAY8, 2, FPS, {0, 0, +200});
    CamSim cam2(W, H, PixelFormat::GRAY8, 3, FPS, {0, 0, -200});
    CamSim* cams[3] = {&cam0, &cam1, &cam2};

    StreamAligner aligner(3, 1'000'000, 50'000'000);
    std::map<uint64_t, std::vector<uint8_t>> cache[3];

    if (!out_dir.empty()) std::filesystem::create_directories(out_dir);

    std::vector<uint64_t> sums;
    int written = 0;
    for (int tick = 0; tick < MAX_FRAMES + 20 && written < MAX_FRAMES; ++tick) {
        for (int j = 0; j < 3; ++j) {
            Frame f = cams[j]->next();
            cache[j][f.seq].assign(f.data, f.data + f.size);
            aligner.push(static_cast<std::size_t>(j), f);
        }
        while (auto tup = aligner.pop()) {
            const StreamAligner::AlignedTuple& t = *tup;
            auto view = [&](int j) {
                const std::vector<uint8_t>& px = cache[j][t.frames[j].seq];
                Frame v;
                v.data = px.data(); v.size = static_cast<uint32_t>(px.size());
                v.width = W; v.height = H; v.format = PixelFormat::GRAY8;
                return v;
            };
            C c(640, 360);
            draw_scene(c, view(0), view(1), view(2), written,
                       static_cast<long>(t.skew_ns), aligner.emitted(), aligner.rejected());
            sums.push_back(c.checksum());
            if (!out_dir.empty()) {
                char path[64];
                std::snprintf(path, sizeof(path), "%s/frame_%04d.ppm", out_dir.c_str(), written);
                c.write_ppm(path);
            }
            ++written;
            for (int j = 0; j < 3; ++j) {
                auto& m = cache[j];
                m.erase(m.begin(), m.upper_bound(t.frames[j].seq));
            }
            if (written >= MAX_FRAMES) break;
        }
    }
    return sums;
}

}  // namespace

int main() {
    std::printf("M4-GPU: running the M4 compositor on CPU and GPU, comparing frames...\n\n");

    std::vector<uint64_t> cpu = run<Canvas>("");
    std::vector<uint64_t> gpu = run<GpuCanvas>("m4_gpu_frames");

    std::printf("  CPU frames: %zu   GPU frames: %zu\n", cpu.size(), gpu.size());
    if (cpu.size() != gpu.size()) {
        std::printf("MISMATCH: different frame counts\n");
        return 1;
    }

    int mismatches = 0;
    for (size_t i = 0; i < cpu.size(); ++i) {
        if (cpu[i] != gpu[i]) {
            if (mismatches < 5)
                std::printf("  frame %zu differs: cpu=%016llx gpu=%016llx\n",
                            i, (unsigned long long)cpu[i], (unsigned long long)gpu[i]);
            ++mismatches;
        }
    }

    if (mismatches == 0) {
        std::printf("\nRESULT: all %zu frames byte-for-byte identical (CPU checksum == GPU checksum).\n",
                    cpu.size());
        std::printf("        GPU-composited .ppm sequence written to m4_gpu_frames/\n");
        std::printf("        The M4 compositor now runs on the H100 -> CPU reference.\n");
        return 0;
    }
    std::printf("\nRESULT: %d/%zu frames MISMATCH.\n", mismatches, cpu.size());
    return 1;
}
