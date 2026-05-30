// M4 "compositor" demo — the visual deliverable, GPU-free.
//
//   3 drifting CamSims --> StreamAligner --> CPU composite (primary + thumbs,
//   aligned by timestamp) + a HUD of real metrics --> a sequence of .ppm frames.
//
// No GPU: everything is blended into a CPU RGB Canvas and written to disk as
// numbered .ppm frames, which stitch into a video/gif offline (see the command
// printed at the end). Deterministic, so the frame sequence reproduces exactly.
//
// The three cameras run at slightly different clock drifts, so their timestamps
// diverge; the M2 StreamAligner matches them within an epsilon guard and reports
// the sync skew, which the HUD shows climbing over the run.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "retina/align/stream_aligner.hpp"
#include "retina/sim/camsim.hpp"
#include "retina/viz/canvas.hpp"

using namespace retina;

namespace {

std::string fmt2(double v) {   // one-decimal fixed, uppercase-safe
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

}  // namespace

int main() {
    constexpr uint32_t W = 96, H = 96;      // camera resolution
    constexpr uint32_t FPS = 30;
    constexpr int      MAX_FRAMES = 90;     // ~3 s of output
    const std::string  OUT_DIR = "m4_frames";

    // Three cameras: same scene rate, different seeds (different content) and
    // different clock drift (so alignment has real work to do).
    CamSim cam0(W, H, PixelFormat::GRAY8, /*seed=*/1, FPS, {0, 0, /*drift=*/0});
    CamSim cam1(W, H, PixelFormat::GRAY8, /*seed=*/2, FPS, {0, 0, /*drift=*/+200});
    CamSim cam2(W, H, PixelFormat::GRAY8, /*seed=*/3, FPS, {0, 0, /*drift=*/-200});
    CamSim* cams[3] = {&cam0, &cam1, &cam2};

    StreamAligner aligner(3, /*epsilon=*/1'000'000 /*1 ms*/, /*max_wait=*/50'000'000);

    // Per-stream pixel cache keyed by seq — the aligner keeps frame metadata but
    // its data pointers dangle (CamSim reuses its buffer), so we hold the bytes.
    std::map<uint64_t, std::vector<uint8_t>> cache[3];

    std::filesystem::create_directories(OUT_DIR);

    int written = 0;
    for (int tick = 0; tick < MAX_FRAMES + 20 && written < MAX_FRAMES; ++tick) {
        for (int j = 0; j < 3; ++j) {
            Frame f = cams[j]->next();
            cache[j][f.seq].assign(f.data, f.data + f.size);   // own the pixels
            aligner.push(static_cast<std::size_t>(j), f);
        }

        while (auto tup = aligner.pop()) {
            const StreamAligner::AlignedTuple& t = *tup;

            Canvas c(640, 360);
            c.fill(18, 18, 28);

            // Wrap cached pixels back into a Frame view for blitting.
            auto view = [&](int j) {
                const std::vector<uint8_t>& px = cache[j][t.frames[j].seq];
                Frame v;
                v.data = px.data(); v.size = static_cast<uint32_t>(px.size());
                v.width = W; v.height = H; v.format = PixelFormat::GRAY8;
                return v;
            };

            // Primary (cam 0), large on the left.
            c.blit_gray(view(0), 8, 8, 336, 300);
            c.border(8, 8, 336, 300, 90, 90, 120);
            c.draw_text(14, 14, "CAM 0", 240, 240, 120, 2);

            // Thumbnails (cam 1, cam 2) stacked on the right.
            c.blit_gray(view(1), 352, 8, 280, 146);
            c.border(352, 8, 280, 146, 90, 90, 120);
            c.draw_text(358, 12, "CAM 1", 240, 240, 120, 1);

            c.blit_gray(view(2), 352, 162, 280, 146);
            c.border(352, 162, 280, 146, 90, 90, 120);
            c.draw_text(358, 166, "CAM 2", 240, 240, 120, 1);

            // HUD bar with live metrics.
            c.rect(0, 316, 640, 44, 8, 8, 14);
            char line[128];
            std::snprintf(line, sizeof(line), "RETINA M4  FRAME %03d  CAMS 3", written);
            c.draw_text(8, 322, line, 120, 220, 160, 2);
            const std::string hud2 =
                "SYNC " + fmt2(t.skew_ns / 1e6) + " MS   ALIGNED " +
                std::to_string(aligner.emitted()) + "/" +
                std::to_string(aligner.emitted() + aligner.rejected());
            c.draw_text(8, 340, hud2, 120, 220, 160, 2);

            char path[64];
            std::snprintf(path, sizeof(path), "%s/frame_%04d.ppm", OUT_DIR.c_str(), written);
            c.write_ppm(path);
            ++written;

            // Prune cache entries this tuple consumed (and older).
            for (int j = 0; j < 3; ++j) {
                auto& m = cache[j];
                m.erase(m.begin(), m.upper_bound(t.frames[j].seq));
            }
            if (written >= MAX_FRAMES) break;
        }
    }

    std::printf("M4 compositor: wrote %d composited frames to %s/\n", written, OUT_DIR.c_str());
    std::printf("  aligned tuples: %llu   rejected: %llu\n",
                (unsigned long long)aligner.emitted(), (unsigned long long)aligner.rejected());
    std::printf("\nStitch into a video:\n");
    std::printf("  ffmpeg -framerate %u -i %s/frame_%%04d.ppm -pix_fmt yuv420p retina_m4.mp4\n",
                FPS, OUT_DIR.c_str());
    std::printf("or a gif:\n");
    std::printf("  ffmpeg -framerate %u -i %s/frame_%%04d.ppm retina_m4.gif\n",
                FPS, OUT_DIR.c_str());
    return 0;
}
