// Multithreaded stress test for LatestValue — run under ThreadSanitizer.
//
//   cmake -S . -B build -DRETINA_TSAN=ON && cmake --build build
//   ctest --test-dir build -R latest_value_mt --output-on-failure
//
// One hot producer, N consumers, exercising the two things the single-threaded
// suite can't:
//
//   1. DATA RACES — TSan instruments every access. A missing acquire/release or
//      a slot recycled out from under a reader shows up as a reported race.
//
//   2. TORN READS — the producer fills every byte of a frame with (seq & 0xFF)
//      and tags it with `seq`. A consumer reads seq, then checks all bytes equal
//      (seq & 0xFF). If the producer overwrote the slot mid-read, the bytes would
//      no longer match the seq the consumer saw → integrity failure. This is the
//      behavioral backstop in case TSan misses a window.
//
// The buffer is sized with num_consumers == thread count, so when every consumer
// pins a different frame AND one slot is the freshly-published newest, the
// producer still needs a free slot — the exact worst case the "+2" is for.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "retina/latest_value.hpp"
#include "mt_env.hpp"

using namespace retina;

namespace {

// Defaults are heavy (wide race window); override via env for CI. See mt_env.hpp.
const unsigned    kConsumers  = static_cast<unsigned>(retina_test::env_u64("RETINA_MT_CONSUMERS", 4));
const uint64_t    kFrameCount = retina_test::env_u64("RETINA_MT_FRAMES", 200'000);
const std::size_t kFrameBytes = static_cast<std::size_t>(retina_test::env_u64("RETINA_MT_BYTES", 32 * 1024));

std::atomic<bool>     g_producer_done{false};
std::atomic<uint64_t> g_valid_reads{0};   // handles that were valid
std::atomic<uint64_t> g_torn_reads{0};    // bytes didn't match seq  → BUG
std::atomic<uint64_t> g_regressions{0};   // seq went backwards for one consumer → BUG

// Read a frame and confirm every byte matches the seq tag. Returns false on a
// torn read.
bool frame_is_consistent(const Frame& f) {
    const uint8_t expect = static_cast<uint8_t>(f.seq & 0xFF);
    for (uint32_t i = 0; i < f.size; ++i) {
        if (f.data[i] != expect) return false;
    }
    return true;
}

void producer(LatestValue& lv) {
    std::vector<uint8_t> scratch(kFrameBytes);
    for (uint64_t seq = 1; seq <= kFrameCount; ++seq) {
        const uint8_t byte = static_cast<uint8_t>(seq & 0xFF);
        // fill the whole frame with the seq's byte so consumers can verify it
        for (auto& b : scratch) b = byte;

        Frame f;
        f.seq    = seq;
        f.data   = scratch.data();
        f.size   = static_cast<uint32_t>(scratch.size());
        f.width  = 128;
        f.height = 64;
        f.format = PixelFormat::GRAY8;
        lv.publish(f);   // never blocks; may drop under load — that's fine
    }
    g_producer_done.store(true, std::memory_order_release);
}

void consumer(LatestValue& lv) {
    uint64_t last_seq = 0;
    uint64_t local_valid = 0, local_torn = 0, local_regress = 0;

    // Keep reading until the producer is done, then one final drain so we also
    // validate the very last published frame.
    for (;;) {
        const bool done = g_producer_done.load(std::memory_order_acquire);

        FrameHandle h = lv.latest();
        if (h.valid()) {
            const Frame& f = h.frame();
            const uint64_t seq = f.seq;

            if (!frame_is_consistent(f)) ++local_torn;
            if (seq < last_seq)          ++local_regress;  // newest only advances
            last_seq = seq;
            ++local_valid;
        }
        // handle released here → slot refcount drops

        if (done) {
            // drain once more after seeing done, then stop
            FrameHandle tail = lv.latest();
            if (tail.valid()) {
                if (!frame_is_consistent(tail.frame())) ++local_torn;
                ++local_valid;
            }
            break;
        }
    }

    g_valid_reads.fetch_add(local_valid,   std::memory_order_relaxed);
    g_torn_reads.fetch_add(local_torn,     std::memory_order_relaxed);
    g_regressions.fetch_add(local_regress, std::memory_order_relaxed);
}

}  // namespace

int main() {
    LatestValue lv(kConsumers, kFrameBytes);

    std::vector<std::thread> threads;
    threads.reserve(kConsumers + 1);
    for (unsigned i = 0; i < kConsumers; ++i)
        threads.emplace_back(consumer, std::ref(lv));
    threads.emplace_back(producer, std::ref(lv));

    for (auto& t : threads) t.join();

    const uint64_t valid  = g_valid_reads.load();
    const uint64_t torn   = g_torn_reads.load();
    const uint64_t regress = g_regressions.load();

    std::printf("valid reads: %llu   torn: %llu   seq-regressions: %llu\n",
                (unsigned long long)valid,
                (unsigned long long)torn,
                (unsigned long long)regress);

    if (torn != 0 || regress != 0) {
        std::printf("FAIL: integrity violated (torn=%llu, regressions=%llu)\n",
                    (unsigned long long)torn, (unsigned long long)regress);
        return 1;
    }
    if (valid == 0) {
        std::printf("FAIL: consumers never observed a valid frame\n");
        return 1;
    }
    std::printf("PASS: no torn reads, no seq regressions across %llu reads\n",
                (unsigned long long)valid);
    return 0;
}
