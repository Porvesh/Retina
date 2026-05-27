#pragma once
#include <cstdint>
#include <vector>

namespace retina {

// A frame after "encoding" — the unit that crosses the M3 network.
//
// We do NOT run a real video codec. We MODEL the one property that makes network
// loss interesting: frame types with dependencies.
//   - I (intra): a keyframe. Decodable on its own. Big.
//   - P (predicted): depends on the previously-coded picture (depends_on). Small.
//     Undecodable if its reference chain is broken.
// (B-frames — which depend on FUTURE frames — are deliberately out of scope, so
// there is no decode reordering and dependencies only ever point backwards.)
//
// That asymmetry is the whole point: losing a P glitches one frame, but losing
// an I orphans every P after it until the next I. It is why FEC protects
// I-frames harder and why the receiver asks the sender for a fresh keyframe.
//
// Unlike Frame (a borrowed view into a shared pool), an EncodedFrame OWNS its
// bytes: across a network you copy, there is no shared memory on the far side.
enum class FrameType : uint8_t { I, P };

struct EncodedFrame {
    uint64_t             seq        = 0;             // frame number (from CamSim)
    uint64_t             capture_ts = 0;             // ns, carried end to end
    FrameType            type       = FrameType::I;
    uint64_t             depends_on = 0;             // seq of reference; 0 = none (I)
    std::vector<uint8_t> bytes;                      // encoded payload (owned)
};

inline const char* to_string(FrameType t) {
    return t == FrameType::I ? "I" : "P";
}

} // namespace retina
