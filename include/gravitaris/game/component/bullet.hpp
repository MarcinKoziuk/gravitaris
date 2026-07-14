#pragma once

#include <gravitaris/game/component/team.hpp>

namespace Gravitaris {

struct Bullet {
    double remainingLifetime;
    // Shooter's team, so DamageSystem's hit query can skip friendly fire.
    TeamId team = TeamId::Blue;
    float damage = 10.f;
};

} // namespace Gravitaris
