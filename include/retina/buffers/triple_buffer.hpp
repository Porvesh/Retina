#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "retina/core/frame.hpp"
#include "retina/core/frame_buffer.hpp"

namespace retina {

// TripleBuf: the canonical lock-free triple buffer. One producer, one consumer,
// freshness discipline (implements latest()). The point of the THIRD buffer is
// full decoupling: the producer always has a private buffer to write, so
// publish() NEVER blocks and never fails to place the incoming frame; the
// consumer always sees the newest complete frame. Stale intermediate frames are
// dropped (that's latest-value semantics), but the producer never stalls.
//
// Three buffers, three roles, always a permutation of {0,1,2}:
//   - present_    : the buffer the consumer is reading      (consumer-only)
//   - inprogress_ : the buffer the producer is writing       (producer-only)
//   - ready_      : the newest completed buffer, handed off  (shared, atomic)
// Because the three indices are always distinct, the producer never touches the
// consumer's present_ buffer — so no refcounts are needed, just like SpscRing.
//
// ready_ packs the buffer index (low 2 bits) with a FRESH flag (bit 2) that says
// "producer published since the consumer last took a buffer". Hand-off is a
// single atomic exchange on each side.
class TripleBuf : public FrameBuffer {
public:
    explicit TripleBuf(std::size_t max_frame_bytes) {
        for (auto& s : slots_) s.storage.resize(max_frame_bytes);
    }

    // --- producer side (one thread) ---
    void publish(const Frame& f) override {
        // Write into our private in-progress buffer — always available.
        Slot& s = slots_[inprogress_];
        const std::size_t n = f.size < s.storage.size() ? f.size : s.storage.size();
        if (f.data && n) std::memcpy(s.storage.data(), f.data, n);
        s.frame      = f;
        s.frame.data = s.storage.data();

        // Publish: swap our just-written buffer into ready_ (marked fresh) and
        // take whatever was there as our next in-progress buffer. release makes
        // the bytes visible to the consumer's acquire on ready_.
        const unsigned old = ready_.exchange(inprogress_ | kFresh, std::memory_order_release);
        inprogress_ = old & kIndex;
    }

    // --- consumer side (one thread) ---
    FrameHandle latest() override {
        if (in_flight_)
            throw std::logic_error("TripleBuf: release the previous frame before calling latest()");

        const unsigned r = ready_.load(std::memory_order_acquire);
        if (r & kFresh) {
            // Something new: swap our present buffer into ready_ (clearing fresh)
            // and take the fresh buffer as our new present. acq_rel so we observe
            // the producer's writes to the buffer we're taking.
            const unsigned old = ready_.exchange(present_, std::memory_order_acq_rel);
            present_       = old & kIndex;
            present_valid_ = true;
        }

        if (!present_valid_) return {};   // nothing ever published
        in_flight_ = true;
        return make_handle(this, present_, slots_[present_].frame);
    }

protected:
    void release_slot(uint32_t /*slot*/) override {
        // present_ stays the consumer's until the next latest() swaps it out, and
        // the producer never touches it, so there is nothing to hand back — we
        // just re-open the door for the next latest().
        in_flight_ = false;
    }

private:
    static constexpr unsigned kIndex = 0x3;   // low 2 bits: buffer index (0..2)
    static constexpr unsigned kFresh = 0x4;   // bit 2: producer published since last take

    struct Slot {
        std::vector<uint8_t> storage;   // the slot OWNS the pixel bytes
        Frame                frame;     // metadata; frame.data points into storage
    };

    std::array<Slot, 3>   slots_;
    std::atomic<unsigned> ready_{1};            // start: index 1, not fresh
    unsigned              inprogress_ = 2;      // producer-only
    unsigned              present_    = 0;      // consumer-only
    bool                  present_valid_ = false;  // consumer-only
    bool                  in_flight_     = false;  // consumer-only: one borrow at a time
};

} // namespace retina
