// Multithreaded stress test for SpscRing — run under ThreadSanitizer.
//
//   cmake -S . -B build-tsan -DRETINA_TSAN=ON && cmake --build build-tsan
//   ctest --test-dir build-tsan -R spsc_ring_mt --output-on-failure
//
// EXACTLY one producer and one consumer — that's the contract (the "S"s in
// SPSC). The invariants a correct FIFO-with-drop ring must uphold:
//
//   1. DATA RACES — TSan instruments the write_/read_ handshake. A missing
//      acquire/release, or the producer overwriting a slot the consumer is
//      reading, shows up as a reported race.
//
//   2. NO REORDER / NO DUPLICATES — every seq the consumer receives must be
//      STRICTLY GREATER than the previous one. Drops are allowed (that's the
//      policy under load) so the sequence may have gaps, but it must never go
//      backwards or repeat.
//
//   3. NO TORN READS — each frame's bytes are filled with (seq & 0xFF); the
//      consumer re-checks that after reading, catching any slot recycled mid-read.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "retina/buffers/spsc_ring.hpp"
#include "mt_env.hpp"

using namespace retina;

namespace {

// Defaults are heavy (wide race window); override via env for CI. See mt_env.hpp.
const std::size_t kCapacity   = static_cast<std::size_t>(retina_test::env_u64("RETINA_MT_CAPACITY", 8));
const uint64_t    kFrameCount = retina_test::env_u64("RETINA_MT_FRAMES", 200'000);
const std::size_t kFrameBytes = static_cast<std::size_t>(retina_test::env_u64("RETINA_MT_BYTES", 4096));

std::atomic<bool>     g_producer_done{false};
std::atomic<uint64_t> g_received{0};      // frames the consumer got
std::atomic<uint64_t> g_torn{0};          // bytes didn't match seq → BUG
std::atomic<uint64_t> g_out_of_order{0};  // seq didn't strictly increase → BUG

bool frame_is_consistent(const Frame& f) {
    const uint8_t expect = static_cast<uint8_t>(f.seq & 0xFF);
    for (uint32_t i = 0; i < f.size; ++i)
        if (f.data[i] != expect) return false;
    return true;
}

void producer(SpscRing& ring) {
    std::vector<uint8_t> scratch(kFrameBytes);
    for (uint64_t seq = 1; seq <= kFrameCount; ++seq) {
        for (auto& b : scratch) b = static_cast<uint8_t>(seq & 0xFF);
        Frame f;
        f.seq    = seq;
        f.data   = scratch.data();
        f.size   = static_cast<uint32_t>(scratch.size());
        f.width  = 64;
        f.height = 16;
        f.format = PixelFormat::GRAY8;
        ring.publish(f);   // never blocks; drops on full
    }
    g_producer_done.store(true, std::memory_order_release);
}

void consumer(SpscRing& ring) {
    uint64_t last_seq = 0;
    uint64_t received = 0, torn = 0, ooo = 0;

    auto consume_one = [&]() -> bool {
        FrameHandle h = ring.next();   // released at end of scope → tail advances
        if (!h.valid()) return false;
        const Frame& f = h.frame();
        if (!frame_is_consistent(f)) ++torn;
        if (f.seq <= last_seq)        ++ooo;   // must strictly increase
        last_seq = f.seq;
        ++received;
        return true;
    };

    for (;;) {
        const bool done = g_producer_done.load(std::memory_order_acquire);
        while (consume_one()) { /* drain what's available */ }
        if (done) {
            while (consume_one()) { /* final drain after producer finished */ }
            break;
        }
    }

    g_received.fetch_add(received,     std::memory_order_relaxed);
    g_torn.fetch_add(torn,             std::memory_order_relaxed);
    g_out_of_order.fetch_add(ooo,      std::memory_order_relaxed);
}

}  // namespace

int main() {
    SpscRing ring(kCapacity, kFrameBytes);

    std::thread c(consumer, std::ref(ring));
    std::thread p(producer, std::ref(ring));
    p.join();
    c.join();

    const uint64_t received = g_received.load();
    const uint64_t torn     = g_torn.load();
    const uint64_t ooo      = g_out_of_order.load();

    std::printf("received: %llu / %llu  (%.1f%% completeness)   torn: %llu   out-of-order: %llu\n",
                (unsigned long long)received, (unsigned long long)kFrameCount,
                100.0 * (double)received / (double)kFrameCount,
                (unsigned long long)torn, (unsigned long long)ooo);

    if (torn != 0 || ooo != 0) {
        std::printf("FAIL: integrity violated (torn=%llu, out_of_order=%llu)\n",
                    (unsigned long long)torn, (unsigned long long)ooo);
        return 1;
    }
    if (received == 0) {
        std::printf("FAIL: consumer never received a frame\n");
        return 1;
    }
    std::printf("PASS: strictly-ordered, no torn reads across %llu received frames\n",
                (unsigned long long)received);
    return 0;
}
