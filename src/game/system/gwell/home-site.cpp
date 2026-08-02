#include <limits>

#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/planet.hpp>
#include <gravitaris/game/component/planet-attachment.hpp>
#include <gravitaris/game/component/structure.hpp>
#include <gravitaris/game/component/transform.hpp>
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

static flecs::entity FindHomePlanet(flecs::world& registry, TeamId team, const Magnum::Vector2d& from,
                                    bool needsLab);

flecs::entity FindHomePlanet(flecs::world& registry, TeamId team, const Magnum::Vector2d& from)
{
    return FindHomePlanet(registry, team, from, /*needsLab=*/false);
}

flecs::entity FindResearchPlanet(flecs::world& registry, TeamId team, const Magnum::Vector2d& from)
{
    return FindHomePlanet(registry, team, from, /*needsLab=*/true);
}

static flecs::entity FindHomePlanet(flecs::world& registry, TeamId team, const Magnum::Vector2d& from,
                                    bool needsLab)
{
    flecs::entity best;
    double bestDistSq = std::numeric_limits<double>::max();
    std::uint32_t bestNetId = 0;

    registry.each([&](flecs::entity ent, const Planet&, const Transform& transf, const NetId& netId) {
        if (!IsHomePlanet(registry, ent, team)) return;
        if (needsLab && !HasStructure(registry, netId.value, StructureType::Lab, team)) return;
        const double distSq = (transf.pos - from).dot();
        if (distSq < bestDistSq || (distSq == bestDistSq && netId.value < bestNetId)) {
            bestDistSq = distSq;
            bestNetId = netId.value;
            best = ent;
        }
    });
    return best;
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
