#pragma once

#include <cmath>

#include <Magnum/Math/Color.h>
#include <Magnum/Math/Functions.h>

#include <gravitaris/game/component/structure.hpp>
#include <gravitaris/game/component/team.hpp>

#include <gravitaris/cgame/team-color.hpp>

namespace Gravitaris {

// Seconds one leg of the ready cycle takes (red -> yellow -> blue -> red).
inline constexpr float LAB_READY_CYCLE_LEG_SECONDS = 0.3f;

// What a Lab's team-color strokes render as: pulsing white/blue while its
// faction researches, then cycling red/yellow/blue twice as fast once the
// finished upgrade is waiting to be collected -- the speed-up is the tell
// that it's ready, so the two cadences are deliberately a factor of 2 apart.
// `timeSeconds` is any continuous client clock; both animations are cosmetic
// and need no tick agreement between clients.
inline Magnum::Color3 LabGlowColor(const Structure& structure, float timeSeconds)
{
    // Ping-pong across `legs` (there and back for the two-color case, round
    // and round for three).
    const auto cycle = [timeSeconds](const Magnum::Color3* legs, int count, float legSeconds) {
        const float phase = std::fmod(timeSeconds, legSeconds * static_cast<float>(count)) / legSeconds;
        const int leg = static_cast<int>(phase);
        return Magnum::Math::lerp(legs[leg], legs[(leg + 1) % count], phase - static_cast<float>(leg));
    };

    if (structure.upgradesReady > 0) {
        const Magnum::Color3 legs[]{TeamColor(TeamId::Red), TeamColor(TeamId::Yellow),
                                    TeamColor(TeamId::Blue)};
        return cycle(legs, 3, LAB_READY_CYCLE_LEG_SECONDS);
    }

    const Magnum::Color3 legs[]{Magnum::Color3{1.f}, TeamColor(TeamId::Blue)};
    return cycle(legs, 2, LAB_READY_CYCLE_LEG_SECONDS * 2.f);
}

} // namespace Gravitaris
