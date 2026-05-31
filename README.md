# Retina

[![CI](https://github.com/Porvesh/Retina/actions/workflows/ci.yml/badge.svg)](https://github.com/Porvesh/Retina/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)

A synthetic camera → lock-free buffer → consumers pipeline that simulates a
robot's **perception-to-action path entirely in software** — no hardware. A fake
camera produces frames; a lock-free buffer holds the newest one; a family of
consumers each demonstrates a different hard real-time / systems problem:
drop-policy tradeoffs, multi-stream time alignment, a lossy network with FEC, and
that pipeline run as a hard-real-time thread under deadline discipline.

Everything is a **deterministic simulation** (seeded, no wall clock in the
library), so every result reproduces run to run and is unit-testable — including
the concurrency, which is checked under ThreadSanitizer. The focus is
**end-to-end latency and real-time determinism**.

---

## Architecture

```
  ┌──────────────┐
  │   camsim     │   synthetic camera, deterministic, injectable faults
  │              │   (drop / jitter / drift)
  └──────┬───────┘
         │ Frame (by handle, not copy)
         ▼
  ┌──────────────┐
  │  buffer hub  │   latest-value / SPSC ring / double / triple
  │              │   atomic publish, refcounted slots — never blocks
  └──────┬───────┘
         │
   ┌─────┴───────────┬──────────────────┬───────────────────┐
   ▼                 ▼                  ▼                     ▼
┌────────┐    ┌──────────────┐   ┌──────────────┐    ┌──────────────┐
│ align  │    │     net      │   │  compositor  │    │   RT video   │
│ N cams │    │ lossy UDP +  │   │ frames → HUD │    │ compositor @ │
│ + drift│    │ jitter + FEC │   │ → .ppm (CPU) │    │ fixed cadence│
│  (M2)  │    │  (M3)        │   │   (M4)       │    │   (M5)       │
└────────┘    └──────────────┘   └──────────────┘    └──────────────┘
```

## Repository layout

```
include/retina/
  core/      Frame, FrameBuffer (the shared contract + RAII FrameHandle)
  buffers/   LatestValue, SpscRing, DoubleBuf, TripleBuf
  sim/       CamSim — deterministic synthetic camera with fault injection
  align/     StreamAligner — M2 multi-stream time alignment
  net/       Encoder, Packetizer, LossyChannel, JitterBuffer, FEC, feedback (M3)
  viz/       Canvas (CPU framebuffer) + 5x7 font — M4 headless compositor
  rt/        rt::configure (SCHED_FIFO/affinity/mlockall) + JitterMeter (M5)
apps/        m0_spine, m3_link, m4_compositor, m5_rt_video — runnable demos
tests/       one dependency-free test per component (+ TSan stress tests)
```

The library is **header-only C++20**; `retina_core` is a CMake INTERFACE target
that just hands over the include path.

## Build & test

Requires CMake ≥ 3.16 and a C++20 compiler.

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Prove the lock-free buffers race-free under ThreadSanitizer:

```bash
cmake -S . -B build-tsan -DRETINA_TSAN=ON
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure
```

The multithreaded stress tests are sized via env vars (`RETINA_MT_FRAMES`,
`RETINA_MT_BYTES`, …) so CI can run them quickly.

## Demos

```bash
./build/m0_spine      # M0: CamSim(10% drop) -> LatestValue -> slow consumer
./build/m3_link       # M3: full lossy-network stack + metrics
./build/m4_compositor # M4: composite 3 aligned streams + HUD -> .ppm frames
./build/m5_rt_video   # M5: compositor as a periodic hard-RT task + jitter meter
```

- **`m0_spine`** — shows the drop rate, stale-frame skipping, and that the
  producer's worst `publish()` latency stays in the microseconds (never blocks).
- **`m3_link`** — prints the three M3 artifacts: a glass-to-glass latency
  distribution, an FEC recovery-vs-overhead curve, and a jitter-buffer
  latency-vs-stutter tradeoff.
- **`m4_compositor`** — composites three drifting streams (aligned by timestamp)
  plus a live HUD into `m4_frames/*.ppm`; stitch to a video with one `ffmpeg`
  command (printed on exit).
- **`m5_rt_video`** — runs the compositor as a fixed-cadence (60 fps) real-time
  thread and reports the RT capabilities available on the platform plus a
  per-frame deadline-jitter histogram (the thing you tighten on Linux).

## Design notes

- **Lock-free, never-blocking producer.** The buffer hub publishes with a single
  atomic swap; consumers borrow slots via a refcount (RAII `FrameHandle`). Two
  drop policies behind one interface: `latest()` (drop stale) and `next()` (FIFO).
- **Deterministic simulation.** Seeded `mt19937_64`, simulated timestamps, no
  wall clock in the library → reproducible faults and metrics; concurrency
  verified under TSan and proven allocation-free on the hot path.
- **Frame-type–aware networking (M3).** Packets model I/P dependencies, so loss
  is asymmetric: a lost P glitches one frame, a lost I orphans a whole GOP —
  which is why FEC protects I-frames harder and the receiver requests a keyframe.
- **RT discipline (M5).** The compositor runs as a periodic hard-real-time task:
  `SCHED_FIFO` + CPU pinning + `mlockall` on the render thread, the `Canvas`
  allocated once and reused so the hot path never touches the heap, and a
  `JitterMeter` histogram of how far each frame slips from its deadline.

## License

[MIT](LICENSE)
