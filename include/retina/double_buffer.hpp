#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "retina/frame.hpp"
#include "retina/frame_buffer.hpp"

namespace retina {

// DoubleBuf: two buffers, freshness discipline (implements latest()). This is
// the buffer that shows WHY triple buffering exists.
//
// With only two buffers, the producer and consumer cannot fully decouple. One
// buffer holds the newest frame; the other is the spare the producer writes
// into. When the consumer is mid-read holding one buffer AND that buffer is the
// newest, the producer's only remaining buffer is the one being read — it has
// nowhere tear-free to write, so it DROPS the incoming frame. (The classic
// double buffer instead makes the producer BLOCK until the consumer is done;
// the hub rule is "producer never blocks", so we drop.)
//
// Contrast TripleBuf, whose third buffer means publish() always has somewhere to
// write and never drops the incoming frame. That difference is exactly what the
// M1 latency-vs-completeness plot makes visible.
//
// Mechanics are a two-slot version of LatestValue's claim-then-recheck: the
// producer writes the slot that is neither newest nor being read; the consumer
// pins the newest and confirms it didn't move. Refcounts keep a read tear-free.
class DoubleBuf : public FrameBuffer {
public:
    explicit DoubleBuf(std::size_t max_frame_bytes) {
        for (auto& s : slots_) s.storage.resize(max_frame_bytes);
    }

    // --- producer side (one thread) ---
    void publish(const Frame& f) override {
        const int newest = newest_.load(std::memory_order_relaxed);

        // Pick a slot that isn't the newest and isn't being read.
        int dst = -1;
        for (int i = 0; i < 2; ++i) {
            if (i != newest && refcounts_[i].load(std::memory_order_acquire) == 0) {
                dst = i;
                break;
            }
        }
        if (dst < 0) return;   // consumer holds the only spare → DROP (2-buffer limit)

        Slot& s = slots_[dst];
        const std::size_t n = f.size < s.storage.size() ? f.size : s.storage.size();
        if (f.data && n) std::memcpy(s.storage.data(), f.data, n);
        s.frame      = f;
        s.frame.data = s.storage.data();

        newest_.store(dst, std::memory_order_release);
    }

    // --- consumer side (one thread) ---
    FrameHandle latest() override {
        while (true) {
            const int idx = newest_.load(std::memory_order_acquire);
            if (idx < 0) return {};   // nothing published yet

            refcounts_[idx].fetch_add(1, std::memory_order_acq_rel);
            if (newest_.load(std::memory_order_acquire) == idx)
                return make_handle(this, static_cast<uint32_t>(idx), slots_[idx].frame);

            refcounts_[idx].fetch_sub(1, std::memory_order_acq_rel);   // moved; retry
        }
    }

protected:
    void release_slot(uint32_t slot) override {
        refcounts_[slot].fetch_sub(1, std::memory_order_acq_rel);
    }

private:
    struct Slot {
        std::vector<uint8_t> storage;   // the slot OWNS the pixel bytes
        Frame                frame;     // metadata; frame.data points into storage
    };

    std::array<Slot, 2>                slots_;
    std::array<std::atomic<uint32_t>, 2> refcounts_{};   // one count per slot
    std::atomic<int>                   newest_{-1};      // index of newest; -1 = none
};

} // namespace retina
