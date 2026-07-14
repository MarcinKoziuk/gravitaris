#pragma once

#include <Magnum/Math/Color.h>

#include <gravitaris/game/component/team.hpp>

namespace Gravitaris {

// Strokes authored in this exact color are replaced by the entity's team
// color at render time (StarCraft-style team-color mask, see ModelRenderer2);
// all other strokes keep their authored color.
inline constexpr Magnum::Color3 TEAM_COLOR_PLACEHOLDER{1.f, 0.f, 1.f};

// Per-team display color. Each value is the literal on-screen color a
// placeholder stroke becomes -- one distinct hue per team. sRGB display
// values (not fromHsv) to stay predictable through the glow/CRT passes.
inline Magnum::Color3 TeamColor(TeamId id)
{
    switch (id) {
        case TeamId::Blue:    return {0.12f, 0.30f, 1.00f};
        case TeamId::Red:     return {1.00f, 0.15f, 0.15f};
        case TeamId::Green:   return {0.20f, 1.00f, 0.25f};
        case TeamId::Yellow:  return {1.00f, 0.80f, 0.10f};
        case TeamId::Magenta: return {1.00f, 0.20f, 0.85f};
        case TeamId::Cyan:    return {0.15f, 0.90f, 1.00f};
        case TeamId::None:    return {1.00f, 1.00f, 1.00f};
    }
    return {1.00f, 1.00f, 1.00f};
}

} // namespace Gravitaris
