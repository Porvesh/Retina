#pragma once
#include <cstdint>
#include <stdexcept>

#include "retina/frame.hpp"

namespace retina {

class FrameBuffer;  // forward decl: the handle holds a FrameBuffer* to release back to

// ─── FrameHandle ────────────────────────────────────────────────────────────
// The buffer hands a consumer a FrameHandle, not a bare Frame. The handle is the
// "borrow receipt": it carries the frame to read AND remembers which slot it came
// from, so it can hand the slot back when the consumer is done.
//
// It's RAII — the destructor releases automatically, so a consumer can't forget.
// It's move-only — copying would mean two receipts for one borrow, and we'd
// release the same slot twice. Moving transfers the receipt; the moved-from
// handle no longer owns anything.
class FrameHandle {
public:
    FrameHandle() = default;  // empty handle: owns nothing (what latest() returns on "no frame")

    // No copying — one borrow, one receipt.
    FrameHandle(const FrameHandle&)            = delete;
    FrameHandle& operator=(const FrameHandle&) = delete;

    // Moving transfers ownership of the borrow (defined below, once FrameBuffer is complete).
    FrameHandle(FrameHandle&& other) noexcept;
    FrameHandle& operator=(FrameHandle&& other) noexcept;

    ~FrameHandle();  // releases the slot if we still own one

    bool         valid() const { return buffer_ != nullptr; }
    const Frame& frame() const { return frame_; }

private:
    // Only the buffer can mint a "full" handle.
    friend class FrameBuffer;
    FrameHandle(FrameBuffer* buffer, uint32_t slot, const Frame& frame)
        : buffer_(buffer), slot_(slot), frame_(frame) {}

    FrameBuffer* buffer_ = nullptr;  // who to release to; nullptr == owns nothing
    uint32_t     slot_   = 0;        // which slot's refcount to drop
    Frame        frame_;             // the borrowed view itself
};

// ─── FrameBuffer ────────────────────────────────────────────────────────────
// The SHAPE every buffer implementation fills in (LatestValue, SpscRing, ...).
// Pure abstract: you never make a bare FrameBuffer, only a concrete subclass.
// Consumers hold a FrameBuffer* and don't care which implementation they got.
class FrameBuffer {
public:
    virtual ~FrameBuffer() = default;

    // --- producer side ---
    virtual void publish(const Frame& frame) = 0;  // never blocks; drops stale on full

    // --- consumer side ---
    // Two disciplines, two verbs. A buffer implements the ONE that matches its
    // drop policy; the other keeps the base's throwing default. This is why the
    // trait is the right seam for the M1 latency-vs-completeness comparison:
    // the SAME consumer code path, a different verb, a measurably different
    // tradeoff. Calling the unsupported verb is a programming error (wrong
    // buffer for the job), so it throws rather than returning an empty handle —
    // empty already means "supported, but no frame yet".

    // FRESHNESS: the newest frame, stale ones dropped. Empty handle if none yet.
    // Implemented by LatestValue / DoubleBuf / TripleBuf.
    virtual FrameHandle latest() {
        throw std::logic_error("FrameBuffer: latest() not supported by this implementation");
    }

    // COMPLETENESS: the oldest frame not yet consumed, FIFO, no skipping. Empty
    // handle if the consumer has drained everything published so far.
    // Implemented by SpscRing.
    virtual FrameHandle next() {
        throw std::logic_error("FrameBuffer: next() not supported by this implementation");
    }

protected:
    // Called by FrameHandle's destructor, NOT by consumers directly. Drops the
    // refcount on `slot` so the buffer can recycle it. Protected because only the
    // handle (a friend) should ever invoke it.
    friend class FrameHandle;
    virtual void release_slot(uint32_t slot) = 0;

    // Lets subclasses construct a handle (the handle's full ctor is private).
    static FrameHandle make_handle(FrameBuffer* buffer, uint32_t slot, const Frame& frame) {
        return FrameHandle(buffer, slot, frame);
    }
};

// ─── FrameHandle bodies (need a complete FrameBuffer to call release_slot) ────

inline FrameHandle::~FrameHandle() {
    if (buffer_) buffer_->release_slot(slot_);   // still own a slot? hand it back.
}

inline FrameHandle::FrameHandle(FrameHandle&& other) noexcept
    : buffer_(other.buffer_), slot_(other.slot_), frame_(other.frame_) {
    other.buffer_ = nullptr;   // steal the receipt; leave the source owning nothing
}

inline FrameHandle& FrameHandle::operator=(FrameHandle&& other) noexcept {
    if (this != &other) {
        if (buffer_) buffer_->release_slot(slot_);  // drop what WE currently hold first
        buffer_ = other.buffer_;
        slot_   = other.slot_;
        frame_  = other.frame_;
        other.buffer_ = nullptr;                    // source now owns nothing
    }
    return *this;
}

} // namespace retina
