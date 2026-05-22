#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "retina/frame.hpp"
#include "retina/frame_buffer.hpp"

namespace retina {

// SpscRing: a bounded FIFO for ONE producer and ONE consumer. Where LatestValue
// optimises freshness (keep newest, drop stale), the ring optimises COMPLETENESS:
// the consumer sees every frame, in order, until the queue fills. This is the
// other end of the M1 latency-vs-completeness tradeoff.
//
// Under a slow consumer the queue backs up and per-frame latency grows; when it
// fills, publish() DROPS the incoming frame (drop-not-resend) rather than
// blocking — the hub's producer never blocks, ever.
//
// Lock-free via the classic single-producer/single-consumer scheme: the producer
// owns write_, the consumer owns read_, each reads the OTHER's cursor with
// acquire and publishes its OWN with release. Because there is exactly one
// consumer, that single read cursor is the only bookkeeping we need — no
// per-slot refcounts (contrast LatestValue, which is multi-consumer).
class SpscRing : public FrameBuffer {
public:
    // capacity: queue depth in frames (>= 1). max_frame_bytes: the largest frame
    // we'll publish; each slot's storage is sized to this ONCE here, so publish()
    // never reallocates (RT-safe), same discipline as LatestValue.
    SpscRing(std::size_t capacity, std::size_t max_frame_bytes)
        : slots_(capacity == 0 ? 1 : capacity) {
        for (auto& s : slots_) s.storage.resize(max_frame_bytes);
    }

    // --- producer side (one thread) ---
    void publish(const Frame& f) override {
        const uint64_t w = write_.load(std::memory_order_relaxed);  // we own write_
        const uint64_t r = read_.load(std::memory_order_acquire);   // observe frees
        if (w - r >= slots_.size()) return;   // FULL → drop, never block

        // Copy pixels into the slot's OWN storage, then rebind the view — the
        // slot owns the bytes, the Frame just views them (same as LatestValue).
        Slot& s = slots_[w % slots_.size()];
        const std::size_t n = f.size < s.storage.size() ? f.size : s.storage.size();
        if (f.data && n) std::memcpy(s.storage.data(), f.data, n);
        s.frame      = f;
        s.frame.data = s.storage.data();

        // Publish: release ordering makes the copied bytes visible to the
        // consumer's acquire-load of write_ below.
        write_.store(w + 1, std::memory_order_release);
    }

    // --- consumer side (one thread) ---
    // The oldest frame not yet consumed, or an empty handle if drained. FIFO:
    // the returned handle pins its slot; the read cursor only advances when that
    // handle is released, so the producer can't overwrite a frame in flight.
    FrameHandle next() override {
        // One borrow at a time: the ring hands out the tail, and the tail can't
        // move until you let go of it. Calling next() while still holding a frame
        // is a usage error, not a "no data" condition — so it throws.
        if (read_in_flight_)
            throw std::logic_error("SpscRing: release the previous frame before calling next()");

        const uint64_t r = read_.load(std::memory_order_relaxed);   // we own read_
        const uint64_t w = write_.load(std::memory_order_acquire);  // observe writes
        if (r == w) return {};   // empty: consumer has drained everything published

        read_in_flight_ = true;
        const std::size_t idx = r % slots_.size();
        return make_handle(this, static_cast<uint32_t>(idx), slots_[idx].frame);
    }

protected:
    void release_slot(uint32_t /*slot*/) override {
        // FIFO: the frame being released is always the tail, so we just advance
        // read_. Release ordering tells the producer this slot is free before it
        // could reuse it; it also happens-after the consumer finished reading.
        read_.fetch_add(1, std::memory_order_release);
        read_in_flight_ = false;
    }

private:
    struct Slot {
        std::vector<uint8_t> storage;   // the slot OWNS the pixel bytes
        Frame                frame;     // metadata; frame.data points into storage
    };

    std::vector<Slot>     slots_;
    std::atomic<uint64_t> write_{0};   // producer's cursor: index of next slot to write
    std::atomic<uint64_t> read_{0};    // consumer's cursor: index of next slot to read
    bool                  read_in_flight_ = false;  // consumer-only: at most one live borrow
};

} // namespace retina
