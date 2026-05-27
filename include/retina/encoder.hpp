#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "retina/encoded_frame.hpp"
#include "retina/frame.hpp"

namespace retina {

// Encoder: turns raw CamSim Frames into EncodedFrames, modelling a GOP (group of
// pictures) structure — an I-frame every `gop` frames, P-frames in between.
//
// It is a MODEL, not a codec: it assigns a type and a modelled encoded size
// (I-frames are much larger than P-frames, matching real bitrate profiles) and
// fills the payload with deterministic synthetic bytes so a receiver can verify
// a reassembled frame byte-for-byte after transport. The point is the frame-type
// dependency graph, which is what the lossy link (M3-D..G) has to survive.
//
// Determinism: encode(frame N) depends only on (config, emission order, N), so a
// run reproduces exactly — same discipline as CamSim.
//
// request_keyframe() forces the next encode() to emit an I-frame and restart the
// GOP. That is the hook the M3 feedback channel (M3-G) pulls when the receiver
// reports an unrecoverable loss ("send me a keyframe").
class Encoder {
public:
    struct Config {
        uint32_t gop     = 30;      // I-frame cadence: I, then (gop-1) P's
        uint32_t i_bytes = 4000;    // modelled encoded size of an I-frame
        uint32_t p_bytes = 400;     // modelled encoded size of a P-frame
    };

    // Delegating default ctor: C++ forbids defaulting the argument with `{}`
    // here (the nested Config's in-class initializers aren't usable to default
    // an argument inside the enclosing class), so spell it out — same pattern as
    // CamSim's convenience constructor.
    Encoder() : Encoder(Config{}) {}
    explicit Encoder(Config cfg) : cfg_(cfg) {}

    EncodedFrame encode(const Frame& src) {
        // A keyframe request (or the very start / GOP wrap) restarts the GOP.
        if (force_keyframe_) pos_in_gop_ = 0;
        const bool is_i = (pos_in_gop_ == 0);

        EncodedFrame ef;
        ef.seq        = src.seq;
        ef.capture_ts = src.capture_ts;
        if (is_i) {
            ef.type       = FrameType::I;
            ef.depends_on = 0;                    // keyframe: no reference
            ++i_count_;
        } else {
            ef.type       = FrameType::P;
            ef.depends_on = last_seq_;            // the previously-coded picture
            ++p_count_;
        }
        ef.bytes = synth(is_i ? cfg_.i_bytes : cfg_.p_bytes, src.seq);

        last_seq_       = src.seq;
        pos_in_gop_     = (pos_in_gop_ + 1) % (cfg_.gop ? cfg_.gop : 1);
        force_keyframe_ = false;
        return ef;
    }

    // Make the next encode() an I-frame regardless of GOP position.
    void request_keyframe() { force_keyframe_ = true; }

    uint64_t i_count() const { return i_count_; }
    uint64_t p_count() const { return p_count_; }

private:
    // Deterministic synthetic payload: byte i = (seq + i) & 0xFF. Reproducible
    // and verifiable after reassembly; the actual pixel content is irrelevant to
    // M3, only deliverability is.
    static std::vector<uint8_t> synth(uint32_t size, uint64_t seq) {
        std::vector<uint8_t> b(size);
        for (std::size_t i = 0; i < b.size(); ++i)
            b[i] = static_cast<uint8_t>((seq + i) & 0xFF);
        return b;
    }

    Config   cfg_;
    uint32_t pos_in_gop_     = 0;       // 0 => next frame is an I
    uint64_t last_seq_       = 0;       // seq of the previously-coded picture
    bool     force_keyframe_ = false;
    uint64_t i_count_        = 0;
    uint64_t p_count_        = 0;
};

} // namespace retina
