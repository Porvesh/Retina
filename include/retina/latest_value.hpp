#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "retina/frame.hpp"
#include "retina/frame_buffer.hpp"

namespace retina {

// LatestValue: the camera hub. One producer, MANY consumers. Always hands out the
// newest frame and lets stale ones be recycled. Lock-free; the producer never blocks.
//
// Slot pool sized (consumers + 2) so that even when every consumer holds a different
// frame AND one slot is the freshly-published "newest", there is still a free slot
// for the producer to write into. See the design note on why +2.
class LatestValue : public FrameBuffer {
public:
    // max_frame_bytes: the largest frame we'll ever publish. We allocate each slot's
    // storage to this size ONCE here, so publish() never reallocates (RT-safe).
    LatestValue(unsigned num_consumers, std::size_t max_frame_bytes)
        : slots_(num_consumers + 2),
          refcounts_(num_consumers + 2) {
        for (auto& s : slots_) s.storage.resize(max_frame_bytes);
    }

    // --- producer side (one thread) ---
    void publish(const Frame& f) override {
        // 1. Find a free slot: refcount 0 AND not the current newest (we never
        //    overwrite the slot a reader might be about to grab as "newest").
        const int newest = newest_.load(std::memory_order_relaxed);
        int dst = -1;
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (static_cast<int>(i) != newest &&
                refcounts_[i].load(std::memory_order_acquire) == 0) {
                dst = static_cast<int>(i);
                break;
            }
        }
        // The +2 sizing guarantees this never fails. (Belt-and-suspenders: if it
        // somehow did, dropping the frame is the correct latest-value behavior.)
        if (dst < 0) return;

        // 2. Copy the pixels into the slot's OWN storage, then point the Frame view
        //    at that owned copy. The slot owns the bytes; the Frame just views them.
        Slot& s = slots_[dst];
        const std::size_t n = f.size < s.storage.size() ? f.size : s.storage.size();
        if (f.data && n) std::memcpy(s.storage.data(), f.data, n);
        s.frame      = f;
        s.frame.data = s.storage.data();   // rebind the borrowed view to our copy

        // 3. Publish: make this slot the newest. release ordering ensures the bytes
        //    above are visible to any consumer that later acquire-loads this index.
        newest_.store(dst, std::memory_order_release);
    }

    // --- consumer side (many threads) ---
    FrameHandle latest() override {
        while (true) {
            const int idx = newest_.load(std::memory_order_acquire);
            if (idx < 0) return {};   // nothing published yet → empty handle

            // Tentatively claim the slot, THEN confirm it's still newest. The
            // producer skips the newest slot when picking where to write, so if
            // newest hasn't moved since our claim, this slot is safe to read.
            refcounts_[idx].fetch_add(1, std::memory_order_acq_rel);
            if (newest_.load(std::memory_order_acquire) == idx)
                return make_handle(this, static_cast<uint32_t>(idx), slots_[idx].frame);

            // Newest moved while we were claiming — back off and retry.
            refcounts_[idx].fetch_sub(1, std::memory_order_acq_rel);
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

    std::vector<Slot>                  slots_;       // the pool
    std::vector<std::atomic<uint32_t>> refcounts_;   // one count per slot
    std::atomic<int>                   newest_{-1};  // index of newest slot; -1 = none
};

} // namespace retina
