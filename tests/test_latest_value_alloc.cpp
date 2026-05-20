// Real-time safety test for LatestValue: the hot path must not allocate.
//
// The spec's headline claim is "RT-safe, no realloc" — publish() and the
// consumer path (latest() → read → release) must never touch the allocator once
// the buffer is constructed. A single malloc on a 1 kHz control loop is a
// potential deadline miss. This test makes that claim a hard pass/fail.
//
// Method: override global operator new/delete with a counter. Do ALL setup
// (buffer construction, scratch buffers) while the counter is disarmed, then arm
// it and run a tight publish/consume loop. Any allocation in that window fails.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "retina/latest_value.hpp"

using namespace retina;

// ─── allocation counter ──────────────────────────────────────────────────────
// Plain globals: this test is single-threaded in the region we care about.
namespace {
bool     g_armed = false;   // only count allocations while armed
uint64_t g_allocs = 0;      // bumped by every operator new call while armed
}

void* operator new(std::size_t n) {
    if (g_armed) ++g_allocs;
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) {
    if (g_armed) ++g_allocs;
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

int main() {
    constexpr std::size_t kBytes = 4096;
    constexpr uint64_t    kIters = 200'000;

    // --- setup (allocations allowed) ---
    LatestValue lv(2, kBytes);          // constructs the slot pool + refcounts
    std::vector<uint8_t> scratch(kBytes, 0);

    // Warm up once so any lazy first-use allocation happens before we arm.
    {
        Frame f;
        f.seq  = 0;
        f.data = scratch.data();
        f.size = static_cast<uint32_t>(scratch.size());
        lv.publish(f);
        FrameHandle h = lv.latest();
        (void)h.valid();
    }

    // --- armed: the hot path must not allocate ---
    g_armed = true;
    for (uint64_t seq = 1; seq <= kIters; ++seq) {
        // fill is a plain write into an existing buffer — no allocation
        scratch[0] = static_cast<uint8_t>(seq);

        Frame f;
        f.seq    = seq;
        f.data   = scratch.data();
        f.size   = static_cast<uint32_t>(scratch.size());
        f.width  = 64;
        f.height = 16;
        f.format = PixelFormat::GRAY8;

        lv.publish(f);                  // producer hot path
        FrameHandle h = lv.latest();    // consumer hot path
        (void)h.valid();
        // h released here → release_slot(), an atomic decrement, no alloc
    }
    g_armed = false;

    std::printf("allocations on hot path: %llu (over %llu iterations)\n",
                (unsigned long long)g_allocs, (unsigned long long)kIters);

    if (g_allocs != 0) {
        std::printf("FAIL: hot path allocated %llu time(s) — not RT-safe\n",
                    (unsigned long long)g_allocs);
        return 1;
    }
    std::printf("PASS: publish + latest + release are allocation-free\n");
    return 0;
}
