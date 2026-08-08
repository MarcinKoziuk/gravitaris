#include <limits>
#include <vector>

#include <ankerl/unordered_dense.h>

#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/planet.hpp>
#include <gravitaris/game/component/planet-attachment.hpp>
#include <gravitaris/game/component/structure.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/resource/body.hpp>
#include <gravitaris/game/system/gwell/home-site.hpp>

namespace Gravitaris {

static bool HasStructure(flecs::world& registry, std::uint32_t planetNetId, StructureType type, TeamId team);

bool IsHomePlanet(flecs::world& registry, flecs::entity planet, TeamId team)
{
    if (team == TeamId::None || !planet.is_alive() || !planet.has<Planet>()) return false;

    const Team* planetTeam = planet.try_get<Team>();
    const NetId* netId = planet.try_get<NetId>();
    if (!planetTeam || planetTeam->id != team || !netId) return false;

    return HasStructure(registry, netId->value, StructureType::Base, team)
            && HasStructure(registry, netId->value, StructureType::Colony, team);
}

std::uint32_t SitePlanetNetId(flecs::entity site)
{
    if (!site.is_alive()) return 0;
    if (const PlanetOrbitAttachment* orbit = site.try_get<PlanetOrbitAttachment>()) {
        return orbit->planetNetId;
    }
    const NetId* netId = site.try_get<NetId>();
    return netId ? netId->value : 0;
}

std::vector<HomeSite> GatherHomeSites(flecs::world& registry)
{
    // What each side has built where, in two flat passes: one walk over the
    // structures rather than one walk per planet, and nothing nested inside
    // the walk over planets below.
    struct Built {
        bool base = false;
        bool colony = false;
        bool lab = false;
    };
    ankerl::unordered_dense::map<std::uint64_t, Built> built;
    const auto key = [](std::uint32_t planetNetId, TeamId team) {
        return (static_cast<std::uint64_t>(planetNetId) << 8) | static_cast<std::uint8_t>(team);
    };
    const auto note = [&](std::uint32_t planetNetId, StructureType type, TeamId owner) {
        if (owner == TeamId::None) return;
        Built& b = built[key(planetNetId, owner)];
        if (type == StructureType::Base) b.base = true;
        if (type == StructureType::Colony) b.colony = true;
        if (type == StructureType::Lab) b.lab = true;
    };
    registry.each([&](const Structure& s, const Team& t, const PlanetSurfaceAttachment& attach) {
        note(attach.planetNetId, s.type, t.id);
    });
    registry.each([&](const Structure& s, const Team& t, const PlanetOrbitAttachment& attach) {
        note(attach.planetNetId, s.type, t.id);
    });

    std::vector<HomeSite> sites;
    ankerl::unordered_dense::map<std::uint32_t, std::size_t> planetSite;

    registry.each([&](flecs::entity ent, const Planet&, const Transform& transf, const Team& team,
                      const NetId& netId) {
        if (team.id == TeamId::None) return;
        const auto it = built.find(key(netId.value, team.id));
        if (it == built.end() || !it->second.base || !it->second.colony) return;

        planetSite.emplace(netId.value, sites.size());
        sites.push_back(HomeSite{ent, ent, team.id, netId.value, netId.value, transf.pos, transf.vel,
                                 LandingRadius(ent), /*station=*/false, it->second.lab});
    });

    // A port only counts as somewhere to come home to while the rock beneath
    // it is: a station over a planet its side no longer holds is a place to
    // be shot at, not a yard.
    registry.each([&](flecs::entity ent, const Structure& s, const Team& team, const Transform& transf,
                      const NetId& netId, const PlanetOrbitAttachment& orbit) {
        if (team.id == TeamId::None || s.type != StructureType::HighPort) return;
        const auto it = planetSite.find(orbit.planetNetId);
        if (it == planetSite.end()) return;

        // Copied out before the push_back below, which reallocates `sites`
        // and would leave a reference into it dangling.
        const flecs::entity planetEntity = sites[it->second].planet;
        const TeamId planetTeam = sites[it->second].team;
        const bool planetHasLab = sites[it->second].hasLab;
        if (planetTeam != team.id) return;

        sites.push_back(HomeSite{ent, planetEntity, team.id, netId.value, orbit.planetNetId,
                                 transf.pos, transf.vel, LandingRadius(ent), /*station=*/true,
                                 planetHasLab});
    });

    return sites;
}

HomeSite SelectHomeSite(const std::vector<HomeSite>& sites, TeamId team,
                        const Magnum::Vector2d& from, bool needsLab)
{
    if (team == TeamId::None) return {};

    HomeSite best;
    double bestDistSq = std::numeric_limits<double>::max();

    for (const HomeSite& site : sites) {
        if (site.team != team) continue;
        if (needsLab && !site.hasLab) continue;

        const double distSq = (site.pos - from).dot();

        // A port wins the planet it orbits outright rather than on distance:
        // it is always the nearer of the two from outside, and inside the ring
        // the few units the other way do not pay for the descent.
        if (best.IsValid()) {
            if (best.station && !site.station) continue;
            if (best.station == site.station
                && !(distSq < bestDistSq || (distSq == bestDistSq && site.netId < best.netId))) {
                continue;
            }
        }

        bestDistSq = distSq;
        best = site;
    }

    return best;
}

// A planet states its own radius. A station states where its deck is: the
// "spawn" hardpoint is already where a fighter's feet go when it launches
// (FactionSystem::SpawnPosition), and a descent ends at the same place it
// would take off from. Its outward extent is what matters -- the station's
// own rotation puts local -Y along the outward radial, and a descent is
// radial -- so the whole-model bounding radius is the wrong figure: a High
// Port is long and wide, and flaring against its furthest arm leaves a ship
// hovering tens of units above the deck it was aiming at.
double LandingRadius(flecs::entity site)
{
    const Transform& transf = site.get<Transform>();
    if (const Planet* planet = site.try_get<Planet>()) {
        return planet->radius * transf.scale.x();
    }

    const RigidBodyDesc* desc = site.try_get<RigidBodyDesc>();
    if (!desc || !desc->body) return 0.0;
    const Body& body = *desc->body;

    if (const Body::Hardpoint* deck = body.FindHardpoint("spawn"); deck && deck->pos.y() < 0.0) {
        return -deck->pos.y() * transf.scale.x();
    }
    return body.GetBoundingRadius() * transf.scale.x();
}

double ObstacleRadius(flecs::entity site)
{
    const Transform& transf = site.get<Transform>();
    if (const Planet* planet = site.try_get<Planet>()) {
        return planet->radius * transf.scale.x();
    }
    const RigidBodyDesc* desc = site.try_get<RigidBodyDesc>();
    if (!desc || !desc->body) return 0.0;
    return (*desc->body).GetBoundingRadius() * transf.scale.x();
}

static bool HasStructure(flecs::world& registry, std::uint32_t planetNetId, StructureType type, TeamId team)
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

} // namespace Gravitaris
