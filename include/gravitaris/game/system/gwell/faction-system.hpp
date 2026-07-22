#pragma once

#include <optional>

#include <flecs.h>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Per-faction bookkeeping (docs/gravity-well-mode-plan.md Phase 4):
// FactionState entities are created lazily (first GetOrCreate call for a
// team), one each for every team that has ever owned a structure. Each tick,
// counts colonies+freighters per team to detect defeat (sticky -- a
// defeated faction stays defeated even if, say, a lone freighter somehow
// survived its last colony's loss and is later destroyed too), and checks
// whether every currently-claimed planet belongs to a single team to detect
// a round win (also sticky, global rather than per-team).
class FactionSystem {
public:
    // Finished materials a Base/High Port spends to rebuild a dead fighter,
    // same funder rule as EconomySystem::FREIGHTER_COST (needs an
    // accompanying Lab/Space Dock too -- see TryRespawn). Placeholder
    // magnitude pending playtesting.
    static constexpr float FIGHTER_COST = 30.f;

    FactionSystem(flecs::world& registry, EntitySpawner& entitySpawner, GameEventQueue& eventQueue);

    // Finds this team's FactionState entity, creating one (defeated=false,
    // lastLandingSiteNetId=0) the first time it's asked for.
    flecs::entity GetOrCreate(TeamId team);

    // Respawn-site + funding rule (docs/gravity-well-mode-plan.md Phase 4):
    // picks the ship's last friendly landing site if it's still alive and
    // friendly, else any remaining friendly planet or High Port; then
    // requires an affordable funder (a Base with a Lab, or a High Port with
    // a Space Dock, belonging to this team, with >= FIGHTER_COST finished
    // materials) and spends it. Returns the spawn position on success;
    // std::nullopt if there's no site at all (that faction is out -- for
    // the player, game over) OR a site exists but nothing can afford to
    // fund the fighter yet (caller should keep retrying next tick).
    std::optional<Magnum::Vector2d> TryRespawn(TeamId team);

    void Update();

private:
    flecs::world& m_registry;
    EntitySpawner& m_entitySpawner;
    GameEventQueue& m_eventQueue;
    bool m_roundOver = false;
};

} // namespace Gravitaris
