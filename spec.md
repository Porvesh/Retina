# Project Spec — `retina`
### A synthetic camera → lock-free buffer → consumers pipeline (no hardware)

---

## 1. What this is

Retina simulates a robot's **perception-to-action path entirely in software**. A
synthetic camera produces frames; a lock-free buffer hub holds the newest one; a
family of consumers each tackles a different hard real-time / systems problem —
drop-policy tradeoffs, multi-stream time alignment, a lossy network with error
correction, a live compositor, and that compositor run as a hard-real-time
thread under deadline discipline.

It is the successor to *Axon*: where Axon proved concurrency **correctness**,
Retina is about **end-to-end latency and real-time determinism** — and about
*measuring* every layer, not just building it.

The whole system is a **deterministic, in-process simulation**: seeded RNG, no
wall clock inside the library, no real sockets, no GPU. That is a deliberate
choice — it makes every result (including the concurrency and the injected
faults) reproducible run-to-run and unit-testable, which is the point of a
learning/portfolio project. Where the real world would add a driver, a socket,
or a GPU, Retina models the *concept* behind a clean interface that a real
backend could later slot into.

---

## 2. Design principles

These are the through-lines every component obeys:

- **Deterministic by construction.** Faults, jitter, and drift come from a
  seeded `std::mt19937_64`; timestamps are simulated, never read from a clock.
  Same seed → same stream → same metrics.
- **Header-only C++20.** The core is an INTERFACE library (`retina_core`); each
  component is one self-contained header under `include/retina/<domain>/`.
- **The producer never blocks.** The buffer hub publishes with a single atomic
  swap; a slow or dead consumer can never stall the producer. Proven under
  ThreadSanitizer and asserted allocation-free on the hot path.
- **Test-first, framework-free.** Every component has a dependency-free test
  (a tiny `CHECK` harness) run under CTest; lock-free code additionally runs a
  multithreaded stress test under TSan.
- **Fail safe under starvation.** Every waiting stage has a bounded horizon —
  the aligner declares a dead stream, the jitter buffer times a packet out — so
  nothing hangs forever on missing input.

---

## 3. Architecture

```
  ┌──────────────┐
  │   camsim     │  synthetic camera, deterministic, seeded
  │  (sim/)      │  adversary: drop / timing jitter / clock drift
  └──────┬───────┘
         │ Frame (borrowed by handle, not copied)
         ▼
  ┌──────────────┐
  │  buffer hub  │  latest-value · SPSC ring · double · triple
  │  (core/,     │  atomic publish (never blocks), refcounted RAII slots
  │   buffers/)  │  two verbs: latest() drop-stale · next() FIFO
  └──────┬───────┘
         │
   ┌─────┴────────────┬───────────────────┬────────────────────┐
   ▼                  ▼                   ▼                     ▼
┌────────┐    ┌───────────────┐   ┌───────────────┐   ┌────────────────┐
│ align  │    │      net      │   │  compositor   │   │   RT video     │
│ (align/)│   │    (net/)     │   │    (viz/)     │   │    (rt/)       │
│ N cams  │   │ lossy channel │   │ CPU composite │   │ compositor as  │
│ + drift │   │ + jitter buf  │   │ N streams +   │   │ a periodic     │
│ ε-align │   │ + XOR FEC +   │   │ HUD → .ppm    │   │ hard-RT task;  │
│         │   │ keyframe req  │   │ frames        │   │ deadline meter │
│  (M2)   │   │    (M3)       │   │    (M4)       │   │     (M5)       │
└────────┘    └───────────────┘   └───────────────┘   └────────────────┘
```

### Data flow — one frame's journey (net path expanded)

The topology above shows *what connects to what*. This traces a single frame all
the way through, expanding the **net (M3)** box into its glass-to-glass chain.

```
  camsim ──▶ buffer hub ──▶ fan-out to four consumers:
             (Frame by handle)
        align (M2)  ·  compositor (M4)  ·  RT video (M5)  ·  net (M3, below)


  net (M3) — one frame's glass-to-glass journey:

    SENDER
    Encoder ─────────▶ tags each frame I or P on a GOP cadence; models the
      │                encoded size (i_bytes/p_bytes) — a model, not a codec.
      ▼  EncodedFrame{seq, type, depends_on, bytes}
    Packetizer ──────▶ splits into MTU-sized Packets; each carries a stream-wide
      │                seq, frag_idx/count, a last-fragment marker, frame metadata.
      ▼  Packet[]
    FecEncoder ──────▶ appends one XOR-parity packet per group of k data packets;
      │                I-frames use a smaller k (protected harder).
      ▼  data + parity packets
    ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈  the wire  ┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈┈
    LossyChannel ────▶ drops (drop_prob), delays (base_latency + uniform jitter);
      │                jitter alone reorders packets. Seeded → reproducible.
      ▼  packets: jittery, reordered, some missing
    RECEIVER
    FecDecoder ──────▶ recovers one lost packet per group by XOR-ing the
      │                survivors (two losses in a group are unrecoverable).
      ▼  surviving + recovered packets
    JitterBuffer ────▶ holds each packet a playout delay, releases in stream
      │                order; the delay adapts to measured jitter (RFC 3550).
      ▼  in-order packets
    Reassembler ─────▶ regroups fragments (frame_seq + frag_idx) into whole
      │                EncodedFrames; any missing fragment leaves a hole.
      ▼  EncodedFrame
    Decoder ─────────▶ decides DECODABLE against the I/P chain: a P needs its
      │                reference decoded; a lost I orphans the whole GOP.
      ▼  decode result (glass-to-glass latency measured here)
    KeyframeRequester ▶ on an undecodable stream, asks the sender for a fresh
      │                 keyframe (PLI-style, debounced).
      └────── keyframe request ──────▶ Encoder.request_keyframe()
```

---

## 4. Core data structures

### Frame — the shared contract (`core/frame.hpp`)
```
Frame {
    seq        : u64        // monotonic; a gap = a dropped frame
    capture_ts : u64 ns     // simulated "exposure" time
    data       : const u8*  // BORROWED view into a slot; valid only until release
    size       : u32
    width, height : u32
    format     : enum { GRAY8, RGB8, NV12, BAYER_RGGB }
}
```
`data` is a borrowed pointer, never a copy — the slot owns the bytes and keeps
them alive via a refcount. Across the network hop (M3) frames are serialized and
*owned* instead (there is no shared memory on the far side).

### Buffer hub — the interface every consumer codes against (`core/frame_buffer.hpp`)
```
FrameBuffer {
    publish(frame)            // producer: never blocks; overwrites stale on full
    latest() -> FrameHandle   // consumer: newest frame, drop stale   (freshness)
    next()   -> FrameHandle   // consumer: next in FIFO order, no skip (completeness)
}
```
Each implementation overrides the verb matching its drop policy; the other throws.
A move-only **`FrameHandle`** pins a slot for the borrow and releases it (refcount
decrement) in its destructor — RAII, no manual `release()`.

Implementations (`buffers/`): `LatestValue` (N-consumer, refcounted) · `SpscRing`
(1P/1C FIFO, drop-on-full) · `DoubleBuf` + `TripleBuf` (freshness; triple never
drops the incoming frame, double can — which is *why* the third buffer exists).

### camsim faults (`sim/camsim.hpp`)
```
CamSim::Faults {
    drop_rate     : f64   // P(a capture tick is dropped) → seq gaps
    max_jitter_ns : u64   // bounded random offset on capture_ts
    drift_ppm     : f64   // clock runs fast/slow vs truth (signed) → M2's fault
}
```
Deterministic via a seed; a dropped tick still spends its frame number, so
emitted frames show a **seq gap** — exactly how a consumer detects capture loss.

---

## 5. Components (as built)

| # | Component | Location | Responsibility | Teaches |
|---|-----------|----------|----------------|---------|
| — | **camsim** | `sim/` | emit `Frame`s with injectable, seeded faults | V4L2-style capture, fault injection, determinism |
| — | **buffer hub** | `core/`, `buffers/` | hold newest frame; atomic publish; refcounted recycle | latest-value vs queue, drop policy, lock-free publish |
| M2 | **aligner** | `align/` | match N drifting streams by `capture_ts` within an ε-guard | multi-stream time alignment, stall handling |
| M3 | **net** | `net/` | I/P encoder, RTP-style framing, lossy channel, jitter buffer, XOR FEC, keyframe-request feedback | RTP/UDP concepts, jitter buffer, FEC, drop-not-resend |
| M4 | **compositor** | `viz/` | composite N streams + a metrics HUD into a CPU framebuffer → `.ppm` | timestamp alignment, CPU compositing, on-screen metrics |
| M5 | **RT video** | `rt/` | run the compositor as a periodic hard-RT task (fixed frame cadence); measure per-frame deadline jitter | hard-RT scheduling (`SCHED_FIFO`/`isolcpus`/`mlockall`), deadline jitter, allocation-free hot path |

---

## 6. Milestones (build order + "done when")

Each milestone is independently demonstrable. **All are complete** except the
noted deferrals.

### M0 — Spine ✅
`camsim` + `LatestValue` + a consumer that prints inter-frame latency.
**Done:** frames flow; a 10% drop is visible; the consumer skips stale frames;
the producer's worst `publish()` stays in the microseconds (never blocks).
Demo: `m0_spine`.

### M1 — Buffer family + drop-policy probe ✅ *(plot deferred)*
`SpscRing` + `DoubleBuf`/`TripleBuf` behind the same interface, compared.
Freshness vs completeness is captured by the two verbs and pinned by tests; the
formal *latency-vs-completeness plot* is deferred.

### M2 — Multi-stream time alignment ✅ *(plots deferred)*
N cams with independent `drift_ppm`. `StreamAligner` picks stream 0 as reference,
matches each other stream's nearest neighbour within an ε-guard (one-to-one),
emits aligned tuples (with skew) or rejects desynced sets, and detects dropped
triggers via seq gaps. Robust to a dead/lagging stream via a **max-wait horizon**
(rejects at bounded latency instead of stalling) + an old-frame prune. The
sync-error histogram / rejection-vs-drift plots are deferred.

### M3 — Lossy link + jitter buffer + FEC ✅
An **in-process** lossy channel (drop / latency / jitter / bandwidth cap, seeded)
— the deterministic stand-in for a real UDP socket, behind a `send`/`deliver`
shim a socket backend could replace. RTP-style fragmentation + reassembly;
frame-type–aware encoding (I/P, backward deps only) so **loss is asymmetric** — a
lost P glitches one frame, a lost I orphans a whole GOP; an adaptive jitter
buffer (RFC 3550-style estimate); **XOR FEC** that protects I-frames harder
(Reed-Solomon deferred); and a **keyframe-request feedback loop** (PLI-style — the
real-time-correct recovery, not per-packet retransmit).
**Done:** `m3_link` prints a glass-to-glass latency distribution, an FEC
recovery-vs-overhead curve, and a jitter-buffer latency-vs-stutter tradeoff.

### M4 — Compositor + HUD ✅ *(headless, no GPU)*
No GPU available, so the compositor blends streams into a **CPU RGB framebuffer**
and writes each composed frame to disk as a `.ppm` (which stitches into a
video/gif) — the concept of GPU compositing minus the driver. Aligns the streams
by timestamp (reusing M2), lays out a primary view + thumbnails, and overlays a
HUD fed by real pipeline numbers.
**Done:** `m4_compositor` writes a `.ppm` frame sequence of composited, aligned
streams + a live HUD. *(A real GPU/window backend is the natural extension.)*

### M5 — Hard-RT video thread ✅ *(RT numbers need Linux)*
A live video pipeline's hot loop must emit a frame *every frame-period* or the
stream stutters — the same deadline discipline a machine-control loop needs. M5
takes M4's compositor and runs it as a **periodic hard-real-time task** at a
fixed cadence (60 fps): the render thread applies the three RT levers —
`SCHED_FIFO`, CPU pinning (`isolcpus`), `mlockall` — and composites one frame per
tick with **zero hot-path allocation** (the `Canvas` is allocated once and
reused). A `JitterMeter` records how far each frame slips from its deadline as a
histogram. The RT levers are guarded under `#ifdef __linux__` and no-op
elsewhere. This reuses M3-A's I/P concepts only loosely; its real point is the
*scheduling* discipline, and it shares the RT primitives (`rt/`) any control-grade
consumer would need.
**Done (logic):** `m5_rt_video` reports the RT capabilities available on the
platform and the per-frame deadline-jitter histogram. **Deferred:** the
*tightened* jitter numbers, which require a Linux VM (`PREEMPT_RT` / `isolcpus`) —
a Mac can only run it best-effort, so its histogram shows the OS preempting the
render thread (exactly the tail M5 exists to squeeze out on Linux).

---

## 7. Demos & artifacts

Each milestone ships a runnable under `apps/`:

- `m0_spine` — drop visibility, stale-skipping, non-blocking producer.
- `m3_link` — the three M3 metric artifacts (deterministic).
- `m4_compositor` — the composited multi-stream + HUD `.ppm` sequence (the
  closest thing to the "one screen recording" success criterion).
- `m5_rt_video` — RT-capability report + per-frame deadline-jitter histogram for
  the compositor run as a periodic hard-RT task.

The original aspiration was a *single unified live window* showing all streams +
a HUD of every stage's metrics. Retina realizes that as the headless
`m4_compositor` output plus the per-milestone metric dumps; a unified live GPU
window is future work gated on hardware.

---

## 8. Platform & tech notes

- **Language:** header-only **C++20** throughout (`std::atomic`, `pthread` for RT
  on Linux). The original spec floated mixing Rust for some parts; the
  implementation is single-language for cohesion.
- **Determinism over realism:** in-process channel instead of real UDP sockets;
  CPU `.ppm` compositing instead of a GPU window. Both hide behind interfaces a
  real backend could replace, and both keep the project reproducible/testable.
- **Linux for RT (M5):** `SCHED_FIFO` / `isolcpus` / `mlockall` are Linux-only and
  guarded under `#ifdef __linux__`; a Lima/Multipass VM suffices for everything
  except the very tightest jitter numbers (a VM adds its own jitter — note it,
  don't fight it). Everything compiles and runs best-effort on macOS.

## 9. Possible extensions

Seqlock buffer · Reed-Solomon FEC (multi-loss) · RTCP-style bitrate backoff ·
real localhost UDP backend behind the channel shim · a GPU/window compositor
backend · the deferred M1/M2 plots · the tightened M5 jitter run on a Linux VM.
