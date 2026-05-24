// Multithreaded stress test for DoubleBuf and TripleBuf — run under TSan.
//
//   cmake -S . -B build-tsan -DRETINA_TSAN=ON && cmake --build build-tsan
//   ctest --test-dir build-tsan -R double_triple_mt --output-on-failure
//
// Both are 1-producer/1-consumer freshness buffers behind latest(), so one
// templated harness covers both. Invariants for a freshness buffer:
//
//   1. DATA RACES — TSan instruments the hand-off. A slot recycled under a
//      reader, or a missing acquire/release, shows up as a race.
//   2. MONOTONIC — latest() never goes backwards: each seq the consumer sees is
//      >= the previous one (the newest only advances). Drops are expected.
//   3. NO TORN READS — bytes are filled with (seq & 0xFF) and re-checked.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "retina/double_buffer.hpp"
#include "retina/triple_buffer.hpp"

using namespace retina;

namespace {

constexpr uint64_t    kFrameCount = 200'000;
constexpr std::size_t kFrameBytes = 4096;

bool frame_is_consistent(const Frame& f) {
    const uint8_t expect = static_cast<uint8_t>(f.seq & 0xFF);
    for (uint32_t i = 0; i < f.size; ++i)
        if (f.data[i] != expect) return false;
    return true;
}

// Returns the number of failures (0 = pass) and prints a one-line summary.
template <class Buf>
int run_stress(const char* name) {
    Buf buf(kFrameBytes);

    std::atomic<bool>     done{false};
    std::atomic<uint64_t> received{0}, torn{0}, backwards{0};

    std::thread producer([&] {
        std::vector<uint8_t> scratch(kFrameBytes);
        for (uint64_t seq = 1; seq <= kFrameCount; ++seq) {
            for (auto& b : scratch) b = static_cast<uint8_t>(seq & 0xFF);
            Frame f;
            f.seq  = seq;
            f.data = scratch.data();
            f.size = static_cast<uint32_t>(scratch.size());
            buf.publish(f);   // never blocks
        }
        done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        uint64_t last = 0, got = 0, bad = 0, back = 0;
        for (;;) {
            const bool d = done.load(std::memory_order_acquire);
            {
                FrameHandle h = buf.latest();   // released at scope end
                if (h.valid()) {
                    const Frame& f = h.frame();
                    if (!frame_is_consistent(f)) ++bad;
                    if (f.seq < last)            ++back;   // must not regress
                    last = f.seq;
                    ++got;
                }
            }
            if (d) {
                FrameHandle h = buf.latest();
                if (h.valid()) {
                    if (!frame_is_consistent(h.frame())) ++bad;
                    ++got;
                }
                break;
            }
        }
        received.store(got); torn.store(bad); backwards.store(back);
    });

    producer.join();
    consumer.join();

    const uint64_t got  = received.load();
    const uint64_t bad  = torn.load();
    const uint64_t back = backwards.load();
    std::printf("[%s] received: %llu   torn: %llu   backwards: %llu\n",
                name, (unsigned long long)got,
                (unsigned long long)bad, (unsigned long long)back);

    int fails = 0;
    if (bad != 0)  { std::printf("  FAIL %s: %llu torn reads\n",  name, (unsigned long long)bad);  ++fails; }
    if (back != 0) { std::printf("  FAIL %s: %llu regressions\n", name, (unsigned long long)back); ++fails; }
    if (got == 0)  { std::printf("  FAIL %s: never saw a frame\n", name); ++fails; }
    return fails;
}

}  // namespace

int main() {
    int fails = 0;
    fails += run_stress<DoubleBuf>("DoubleBuf");
    fails += run_stress<TripleBuf>("TripleBuf");

    if (fails == 0) {
        std::printf("PASS: both freshness buffers race-free, monotonic, no torn reads\n");
        return 0;
    }
    std::printf("%d failure(s)\n", fails);
    return 1;
}
