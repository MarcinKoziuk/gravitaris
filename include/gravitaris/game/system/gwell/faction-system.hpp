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
    // accompanying Lab for a Base -- see TryRespawn). Placeholder
    // magnitude pending playtesting.
    static constexpr float FIGHTER_COST = 30.f;

    // How a fresh fighter starts out. Launching from a station or a planet
    // means inheriting that body's motion: left at rest in world space, a
    // ship is simply run over by the home it launched from, which moves at
    // ~90 u/s along its own orbit. `rot` faces it away from the site, so one
    // launched off a deck stands on it rather than lying across it.
    struct SpawnPoint {
        Magnum::Vector2d pos;
        Magnum::Vector2d vel;
        double rot = 0.0;
    };

    // Offset a respawned/spawned fighter lands at from its site's center --
    // clear of a planet's own body (the largest, "simple", has a ~120-unit
    // true radius per landing-state-system.cpp's own convention) and of the
    // High Port orbiting it at 2x that.
    static constexpr double RESPAWN_OFFSET_RADIUS = 420.0;

    // Same, but from a High Port's center: just off the deck of the station
    // you launch from, not clear of the whole planet it orbits. Only used
    // when the model has no "spawn" hardpoint to stand on.
    static constexpr double STATION_RESPAWN_OFFSET = 30.0;

    // Clearance from a "spawn" hardpoint to the middle of the ship standing
    // on it -- the marker is where the feet go, and a fighter is ~18 units
    // long. Overlapping the deck is survivable now that a launch inherits the
    // station's velocity (no relative motion, so no impact), but starting
    // clear of it is still the honest reading of the marker.
    static constexpr double SPAWN_HARDPOINT_CLEARANCE = 12.0;

    FactionSystem(flecs::world& registry, EntitySpawner& entitySpawner, GameEventQueue& eventQueue);

    // Finds this team's FactionState entity, creating one (defeated=false,
    // lastLandingSiteNetId=0) the first time it's asked for.
    flecs::entity GetOrCreate(TeamId team);

    // Where this team's next fighter appears, and how it starts out: the
    // last friendly site landed on if it's still alive and friendly, else any
    // remaining friendly High Port, else any remaining friendly planet.
    // std::nullopt if there's no site at all (that faction is out -- for the
    // player, game over). Does no funding check and spends nothing; used both
    // for the free initial spawn (Game::Start) and as TryRespawn's first
    // step, so the two share one site-selection rule.
    std::optional<SpawnPoint> SpawnPosition(TeamId team);

    // Respawn-site + funding rule (docs/gravity-well-mode-plan.md Phase 4):
    // the SpawnPosition site, but additionally requiring an affordable funder
    // (a Base with a Lab, or a High Port, belonging to this
    // team, with >= FIGHTER_COST finished materials) which it spends. Returns
    // the spawn position on success; std::nullopt if there's no site
    // (SpawnPosition's own out case) OR a site exists but nothing can afford
    // to fund the fighter yet (caller should keep retrying next tick).
    std::optional<SpawnPoint> TryRespawn(TeamId team);

    void Update();

private:
    flecs::world& m_registry;
    EntitySpawner& m_entitySpawner;
    GameEventQueue& m_eventQueue;
    bool m_roundOver = false;
};

} // namespace Gravitaris
