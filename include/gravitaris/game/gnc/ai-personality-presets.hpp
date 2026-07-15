#pragma once

#include <cstdint>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Named bundles of AIPersonality + matching GuidanceParams/FlightControllerParams
// tuning. Purely starting points -- tweak the values in the .cpp freely.
enum class AIPersonalityPreset : std::uint8_t {
    Balanced,   // matches the original, single hardcoded tuning
    Aggressive, // chases farther, fires looser and faster, cuts closer to wells
    Cautious,   // keeps distance, wants a clean shot, evades earlier and wider
    Sniper,     // long standoff, precise fire, patient
    Reckless,   // fastest and most trigger-happy; sometimes ignores danger
                // entirely and flies straight into a gravity well
};

// Overwrites pilot.personality, pilot.guidance, and pilot.flight with the
// preset's tuning. Does not touch pilot.behavior/target/cooldowns.
void ApplyAIPersonalityPreset(AIPilot& pilot, AIPersonalityPreset preset);

} // namespace Gravitaris
