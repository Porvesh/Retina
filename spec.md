# Project Spec — `retina`
### A synthetic camera → buffer → consumers pipeline (no hardware)

---

## 1. What this is

One repo that simulates a robot's perception-to-action path entirely in software.
A fake camera produces frames; a lock-free buffer holds the newest one; three
consumers each demonstrate a different hard real-time/systems problem.

It is the successor to Axon: Axon proved concurrency *correctness*; this proves
*end-to-end latency and real-time determinism*. No camera, no GPU board, no
robot required.

**Success = one screen recording** of a live window showing composited frames
plus a HUD of real numbers (per-stage latency, drops, link loss %, control-loop
deadline jitter) flowing through your own code.

---

## 2. Architecture

```
  ┌──────────────┐
  │   camsim     │   synthetic camera, 30 Hz
  │              │   emits Frame{ ts, seq, pixels }
  │  adversary:  │   can inject jitter / drops / dupes / drift
  └──────┬───────┘
         │ Frame (by handle, not copy)
         ▼
  ┌──────────────┐
  │  buffer hub  │   latest-value ring (keep newest, drop stale)
  │              │   atomic publish, refcounted slots
  └──────┬───────┘   ◄── THE core. all consumers plug in here
         │
   ┌─────┴───────────┬──────────────────┬───────────────────┐
   ▼                 ▼                  ▼
┌────────┐    ┌──────────────┐   ┌──────────────┐
│  net   │    │  compositor  │   │ control loop │
│ lossy  │    │ frames → GPU │   │   1 kHz PID  │
│ UDP +  │───►│ texture +    │   │ on a fake    │
│ jitter │ dec│ HUD on screen│   │ plant        │
│ buf+FEC│    │              │   │ (pendulum)   │
└────────┘    └──────────────┘   └──────────────┘
   M3              M4                 M5
```

---

## 3. Scope

**In scope**
- Synthetic frame source with controllable, reproducible faults
- Lock-free buffer family (latest-value, SPSC ring, double/triple, seqlock)
- Loss/jitter network simulation over localhost UDP + jitter buffer + FEC
- GPU compositing of N streams + a live metrics HUD
- A 1 kHz hard-RT control loop on a simulated plant with full RT tuning
- Per-stage timestamping and metrics → plots

**Out of scope (and why)**
- Real `dma-buf`/EGLImage hardware zero-copy → approximated with a shared-memory
  ring that passes handles/offsets, not bytes (the concept, minus the driver)
- Real HW encoder (NVENC) latency behavior → use software encode or skip encode
- True hardware genlock simultaneity → simulated via shared trigger timestamps

---

## 4. Core data structures

### Frame
```
Frame {
    seq        : u64        // monotonic, gap = dropped frame
    capture_ts : u64 ns     // monotonic clock at "exposure"
    width      : u32
    height     : u32
    format     : enum { GRAY8, RGB8, NV12, BAYER_RGGB }
    data       : handle     // offset into shared pool, NOT a copy
}
```

### Buffer hub interface (the contract every consumer codes against)
```
trait FrameBuffer {
    // producer side
    fn publish(frame: Frame)        // never blocks; overwrites stale on full

    // consumer side
    fn latest() -> Option<Frame>    // newest frame, or None if nothing new
    fn release(frame: Frame)        // refcount decrement → slot reusable
}
```
Implementations to build behind this one interface:
`LatestValue` (atomic pointer swap) · `SpscRing(n)` · `DoubleBuf` · `TripleBuf`
· `Seqlock`. Each is selectable per consumer link.

### camsim config
```
CamSimConfig {
    fps            : f32      // nominal rate
    jitter_ms      : f32      // +/- on frame timing
    drift_ppm      : f32      // clock drift vs "truth"
    drop_prob      : f32      // 0..1
    dup_prob       : f32      // 0..1
    resolution     : (u32,u32)
    seed           : u64      // deterministic reproduction
}
```

---

## 5. Components

| # | Component | Responsibility | Key interface | Teaches |
|---|-----------|----------------|---------------|---------|
| — | **camsim** | emit `Frame`s with injectable faults; deterministic via seed | `next() -> Frame` | V4L2-style capture, fault injection |
| — | **buffer hub** | hold newest frame, atomic publish, refcounted recycle | `FrameBuffer` trait | latest-value vs queue, drop policy, atomic publish |
| M3 | **net** | localhost UDP send/recv with loss+jitter shim; RTP-ish framing; adaptive jitter buffer; XOR→Reed-Solomon FEC | `send(Frame)` / `recv() -> Frame` | RTP/UDP, jitter buffer, FEC, drop-not-resend |
| M4 | **compositor** | decode → upload as GPU texture → blend N streams + overlays by timestamp | `present(frames[])` | GPU texture compositing, ts alignment, vsync |
| M5 | **control loop** | 1 kHz PID on a simulated 2nd-order plant, fed trajectories from a jittery 10 Hz "planner" | `tick()` every 1 ms | hard-RT scheduling, trajectory interpolation, deadline jitter |

---

## 6. Milestones (build order + "done when")

Each milestone is independently demonstrable and maps to one design.

### M0 — Spine
`camsim` + `LatestValue` buffer + a consumer that prints inter-frame latency.
**Done when:** frames flow, you can inject a 10% drop rate and *see* the consumer
skip stale frames without the producer ever blocking.
*(This alone is the backpressure/drop-policy probe — a real project.)*

### M1 — Buffer family + drop-policy probe
Implement SPSC ring + double/triple buffer behind the same trait. Compare them.
**Done when:** you have a plot of *latency vs completeness* for latest-value vs
queue under a slow consumer.

### M2 — Multi-stream time alignment
N camsims, each with independent drift. Match frames by seq/ts with an ε-guard;
emit aligned tuples; reject desynced pairs; detect dropped triggers via seq gaps.
**Done when:** sync-error histogram + "pairs rejected vs injected drift" curve.

### M3 — Lossy link + jitter buffer + FEC
UDP sender/receiver with an impairment shim (drop %, reorder, jitter, bw cap),
RTP-style framing, adaptive jitter buffer, FEC. Add RTCP-style feedback that
drops bitrate before latency.
**Done when:** glass-to-glass latency distribution + FEC recovery-vs-overhead
curve + jitter-buffer latency-vs-stutter tradeoff.
*Note:* if software encode is added here, model frame type (I/P/B) on the
*encoded packet* (not the raw `Frame`): dropping a P/B glitches one frame, but
dropping an I-frame breaks every dependent frame until the next keyframe — so
FEC/jitter-buffer should protect I-frames harder. (Raw capture frames have no
I/P/B; they're all independent.)

### M4 — Compositor + HUD
Decode → GPU textures → blend primary + thumbnails + overlay HUD, aligned by
timestamp, stale layers dropped.
**Done when:** a live window showing multiple streams + a HUD fed by *real*
pipeline numbers. **This is the screen-recordable demo.**

### M5 — Hard-RT control loop (the crown jewel)
Jittery 10 Hz planner publishes trajectories into a triple buffer; 1 kHz control
thread interpolates "now" and runs PID on a simulated pendulum. Run under
`SCHED_FIFO`, pinned to an `isolcpus` core, `mlockall`'d, zero hot-path alloc.
**Done when:** deadline-jitter histogram (1 ms ± what?) that you *tighten* by
killing page faults / IRQs one at a time, **plus** a demo where the planner
stalls and a watchdog executes a controlled stop.

---

## 7. The unifying demo

The final artifact is one window:
- composited camera views (M4)
- overlaid HUD showing live, real metrics from every stage:
  per-stage latency, drop count, sync error (M2), link loss % (M3),
  control-loop deadline jitter (M5)

One screen recording proves you built and *measured* every layer.

---

## 8. Tech choices

- **C++** for the lock-free buffer hub and the RT control loop (Axon continuity,
  role-matched, predictable). `std::atomic`, `pthread` affinity, `sched_setscheduler`.
- **Rust** fine for camsim, the link sim, and the compositor (`crossbeam`,
  `wgpu`). Hand-rolling lock-free/RT in Rust is instructive but fights the
  borrow checker — fine to mix.
- **Linux** required for the RT bits (`PREEMPT_RT`, `isolcpus`, `SCHED_FIFO`).
  A Lima/Multipass VM is enough for everything except the very tightest jitter
  numbers (a VM adds its own jitter — note it, don't fight it).