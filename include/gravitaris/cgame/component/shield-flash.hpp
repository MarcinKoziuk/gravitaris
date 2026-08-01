#pragma once

#include <cstdint>

#include <Magnum/Math/Vector2.h>

namespace Gravitaris {

// Client-side glow on a shield that just took a hit, set by whatever consumes
// the ShieldHit GameEvent and decayed every rendered frame (HitFlashSystem).
// Separate from HitFlash because the two mean opposite things on screen: a
// HitFlash says the hull was reached, a ShieldFlash says it wasn't.
//
// `dir` is the bearing of the impact from the ship's own origin, in the ship's
// LOCAL frame, so it stays correct as the ship turns without being resampled.
// The bubble's shader fades its glow away from it; plating uses it to pick
// which plate lights.
//
// Replication class: client-only presentation state.
struct ShieldFlash {
    // Which ablative plate took the hit, or BUBBLE when the bubble did. Also
    // selects how the glow fades: a plate holds its light for about a second,
    // the bubble settles faster.
    static constexpr std::int8_t BUBBLE = -1;

    float amount = 0.f;               // 1 right after a hit, decaying to 0
    Magnum::Vector2 dir{0.f, -1.f};   // unit; local -Y is the ship's nose
    std::int8_t plate = BUBBLE;
};

} // namespace Gravitaris
