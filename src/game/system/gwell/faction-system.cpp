#include <array>
#include <cstddef>

#include <gravitaris/game/component/faction-state.hpp>
#include <gravitaris/game/component/freighter.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/planet-attachment.hpp>
#include <gravitaris/game/component/planet.hpp>
#include <gravitaris/game/component/structure.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/resource/body.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/system/gwell/faction-system.hpp>
#include <gravitaris/game/system/gwell/home-site.hpp>

namespace Gravitaris {

namespace {
constexpr std::size_t NUM_TEAMS = 7; // TeamId::Blue..None

// True if a live Structure of `type`, belonging to `team`, sits at
// `planetNetId` (planetside or orbital) -- same check FreighterSystem/
// EconomySystem each keep their own copy of (this codebase's established
// pattern of small per-system duplicates rather than a shared helper).
bool HasFriendlyStructure(flecs::world& registry, std::uint32_t planetNetId, StructureType type, TeamId team)
{
    bool found = false;
    registry.each([&](const Structure& s, const Team& t, const PlanetSurfaceAttachment& attach) {
        if (attach.planetNetId == planetNetId && s.type == type && t.id == team) found = true;
    });
    registry.each([&](const Structure& s, const Team& t, const PlanetOrbitAttachment& attach) {
        if (attach.planetNetId == planetNetId && s.type == type && t.id == team) found = true;
    });
    return found;
}
}

FactionSystem::FactionSystem(flecs::world& registry, EntitySpawner& entitySpawner, GameEventQueue& eventQueue)
        : m_registry(registry)
        , m_entitySpawner(entitySpawner)
        , m_eventQueue(eventQueue)
{}

flecs::entity FactionSystem::GetOrCreate(TeamId team)
{
    flecs::entity found;
    m_registry.each([&](flecs::entity e, const FactionState& fs) {
        if (fs.team == team) found = e;
    });
    if (found.is_alive()) return found;

    flecs::entity created = m_registry.entity();
    created.set<FactionState>(FactionState{team, 0, false});
    return created;
}

std::optional<FactionSystem::SpawnPoint> FactionSystem::SpawnPosition(TeamId team)
{
    flecs::entity factionEntity = GetOrCreate(team);
    const FactionState& state = factionEntity.get<FactionState>();

    // A planet is only somewhere to come back to once the faction has built
    // there: the Base is the pad, the Colony is the reason anyone is on the
    // rock at all. Merely holding a claim isn't enough -- a bare planet has
    // nothing to respawn out of, so it is skipped and the search falls through
    // to somewhere that does. Stations are exempt: a High Port IS the
    // infrastructure.
    const auto canHost = [&](flecs::entity planet) { return IsHomePlanet(m_registry, planet, team); };

    // Site: last friendly landing site if it's still alive, friendly and built up...
    flecs::entity site;
    if (state.lastLandingSiteNetId != 0) {
        flecs::entity candidate = m_entitySpawner.EntityForNetId(state.lastLandingSiteNetId);
        if (candidate.is_alive()) {
            const Team* candidateTeam = candidate.try_get<Team>();
            if (candidateTeam && candidateTeam->id == team && canHost(candidate)) site = candidate;
        }
    }
    // ...else any remaining friendly High Port -- launching from the station
    // beats launching from the dirt, so it outranks the planet here...
    if (!site.is_alive()) {
        m_registry.each([&](flecs::entity e, const Structure& s, const Team& t, const PlanetOrbitAttachment&) {
            if (site.is_alive() || t.id != team || s.type != StructureType::HighPort) return;
            site = e;
        });
    }
    // ...else any remaining friendly planet that has been built up.
    if (!site.is_alive()) {
        m_registry.each([&](flecs::entity e, const Planet&, const Team& t) {
            if (site.is_alive() || t.id != team || !canHost(e)) return;
            site = e;
        });
    }
    if (!site.is_alive()) return std::nullopt; // nothing left -- that faction is out

    const Transform& siteTransf = site.get<Transform>();

    // Launching from a station: stand on its "spawn" hardpoint if the model
    // marks one (feet on the deck, nose out), else just off its outward side.
    // RESPAWN_OFFSET_RADIUS is sized to clear a whole planet *and* the
    // station orbiting it, which would put you nowhere near the port you
    // launched from.
    if (const PlanetOrbitAttachment* attach = site.try_get<PlanetOrbitAttachment>()) {
        const flecs::entity planet = m_entitySpawner.EntityForNetId(attach->planetNetId);
        Magnum::Vector2d outward{0., -1.};
        if (planet.is_alive()) {
            const Magnum::Vector2d radial = siteTransf.pos - planet.get<Transform>().pos;
            if (radial.length() > 1e-6) outward = radial.normalized();
        }

        // Nose (local -Y) pointing away from the planet, i.e. standing on the
        // deck the same way the station's own floor faces inward.
        const double rot = std::atan2(outward.x(), -outward.y());

        const ResourcePtr<Body>& body = site.get<RigidBodyDesc>().body;
        const Body::Hardpoint* spawnPoint = body ? (*body).FindHardpoint("spawn") : nullptr;
        if (spawnPoint) {
            // Model space -> world: the station's own rotation, which is
            // exactly `rot` (both put local -Y along `outward`).
            const double c = std::cos(rot);
            const double s = std::sin(rot);
            const Magnum::Vector2d local = spawnPoint->pos;
            const Magnum::Vector2d rotated{local.x() * c - local.y() * s, local.x() * s + local.y() * c};
            return SpawnPoint{siteTransf.pos + rotated + outward * SPAWN_HARDPOINT_CLEARANCE,
                              siteTransf.vel, rot};
        }

        return SpawnPoint{siteTransf.pos + outward * STATION_RESPAWN_OFFSET, siteTransf.vel, rot};
    }

    return SpawnPoint{siteTransf.pos + Magnum::Vector2d{0., -RESPAWN_OFFSET_RADIUS}, siteTransf.vel, 0.0};
}

std::optional<FactionSystem::SpawnPoint> FactionSystem::TryRespawn(TeamId team)
{
    const std::optional<SpawnPoint> pos = SpawnPosition(team);
    if (!pos) return std::nullopt;

    // Funding: a Base with an accompanying Lab, or a High Port (its own
    // producer since it absorbed the Space Dock), belonging to this team,
    // affording FIGHTER_COST -- same funder rule EconomySystem's freighter
    // dispatch uses (the producer needs to physically exist, not just its
    // funder).
    flecs::entity funder;
    m_registry.each([&](flecs::entity e, const Structure& s, const Team& t) {
        if (funder.is_alive() || t.id != team) return;
        if (s.type != StructureType::Base && s.type != StructureType::HighPort) return;
        if (s.finishedMaterials < FIGHTER_COST) return;

        if (s.type == StructureType::Base) {
            const PlanetSurfaceAttachment* a = e.try_get<PlanetSurfaceAttachment>();
            if (!a || !HasFriendlyStructure(m_registry, a->planetNetId, StructureType::Lab, team)) return;
        }

        funder = e;
    });
    if (!funder.is_alive()) return std::nullopt; // site exists, but no one can afford to fund it yet

    funder.get_mut<Structure>().finishedMaterials -= FIGHTER_COST;

    return pos;
}

void FactionSystem::Update()
{
    struct Counts {
        std::uint32_t colonies = 0;
        std::uint32_t freighters = 0;
    };
    std::array<Counts, NUM_TEAMS> counts{};
    std::array<bool, NUM_TEAMS> active{}; // teams seen owning a structure/freighter this tick

    // Read-only pass: GetOrCreate can create an entity, which is a
    // structural change flecs doesn't allow safely from inside an active
    // .each() iterator (observed as an intermittent crash in a long-running
    // game with many archetypes -- collect the team set here, create
    // FactionState entities in a separate pass below, once no iterator is
    // in progress).
    m_registry.each([&](const Structure& s, const Team& team) {
        if (team.id == TeamId::None) return;
        active[static_cast<std::size_t>(team.id)] = true;
        if (s.type == StructureType::Colony) ++counts[static_cast<std::size_t>(team.id)].colonies;
    });
    m_registry.each([&](const Freighter&, const Team& team) {
        if (team.id == TeamId::None) return;
        active[static_cast<std::size_t>(team.id)] = true;
        ++counts[static_cast<std::size_t>(team.id)].freighters;
    });
    for (std::size_t i = 0; i < NUM_TEAMS; ++i) {
        if (active[i]) GetOrCreate(static_cast<TeamId>(i));
    }

    // Defeat check over EXISTING FactionState entities, not a live Structure
    // query -- a FactionState persists even after its team loses every
    // structure it ever had (total wipeout), so this still fires in that
    // case, unlike deriving the team set fresh from Structure ownership
    // each tick.
    m_registry.each([&](FactionState& fs) {
        if (fs.defeated) return;
        const Counts& c = counts[static_cast<std::size_t>(fs.team)];
        if (c.colonies == 0 && c.freighters == 0) {
            fs.defeated = true;
            m_eventQueue.Emit(GameEventType::FactionDefeated, flecs::entity{}, {},
                              static_cast<std::uint32_t>(fs.team));
        }
    });

    // Win check: every currently-claimed planet belongs to the same team.
    if (!m_roundOver) {
        bool anyClaimed = false;
        bool mixedOwners = false;
        TeamId owner = TeamId::None;
        m_registry.each([&](const Planet&, const Team& team) {
            if (team.id == TeamId::None) return;
            anyClaimed = true;
            if (owner == TeamId::None) owner = team.id;
            else if (owner != team.id) mixedOwners = true;
        });
        if (anyClaimed && !mixedOwners) {
            m_roundOver = true;
            m_eventQueue.Emit(GameEventType::RoundOver, flecs::entity{}, {}, static_cast<std::uint32_t>(owner));
        }
    }
}

} // namespace Gravitaris
