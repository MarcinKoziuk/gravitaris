#pragma once

#include <cstdint>

#include <flecs.h>

#include <Magnum/Magnum.h>

#include <ankerl/unordered_dense.h>

#include <gravitaris/game/fwd.hpp>
#include <gravitaris/game/id.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/gnc/ai-personality-presets.hpp>

namespace Gravitaris {

using Magnum::Vector2d;

// Owns entity creation and the authoritative NetId <-> entity registry (ADR
// 0001 constraint 3). Every spawned entity gets a monotonic NetId here; a
// NetId OnRemove observer keeps the reverse map current as entities die. The
// map is the resolution point for replicated cross-entity references and, in
// Phase 2+, for applying snapshots.
class EntitySpawner {
protected:
    flecs::world& m_registry;

    ResourceLoader& m_resourceLoader;

    virtual void AddRenderable(flecs::entity entity, id_t modelId);

    // Emplaces a fresh monotonic NetId on `entity` and registers it. Called by
    // every Spawn* method before the entity is returned.
    void AssignNetId(flecs::entity entity);

    // Shared setup for stars and planets: a kinematic body (per the Body's
    // physics.kinematic) tagged Planet, with a GravitySource attached when the
    // Body declares one.
    flecs::entity SpawnCelestial(id_t modelId, Vector2d position);

private:
    std::uint32_t m_nextNetId = 1; // 0 stays reserved as "invalid" (see NetId)
    ankerl::unordered_dense::map<std::uint32_t, flecs::entity> m_netIdToEntity;
    flecs::observer m_netIdRemovedObserver;

public:
    explicit EntitySpawner(flecs::world& registry, ResourceLoader& resourceLoader);

    virtual ~EntitySpawner();

    // Registers the NetId OnRemove observer. Deliberately NOT done in the
    // constructor: Game builds its EntitySpawner via a virtual
    // CreateEntitySpawner() call evaluated as an ARGUMENT to Game's own
    // (possibly delegating/base-class) constructor -- i.e. before Game's own
    // m_registry member has been constructed. Touching m_registry (observer<>()
    // does) at that point is undefined behavior; merely storing the reference
    // is not. Game's constructor BODY calls this once m_registry is safely
    // alive. Idempotent-by-construction: called exactly once, from exactly
    // one place.
    void Init();

    flecs::entity SpawnPlayer(id_t modelId, Vector2d position, TeamId team = TeamId::Blue);

    flecs::entity SpawnAIShip(id_t modelId, Vector2d position,
                              AIPersonalityPreset preset = AIPersonalityPreset::Balanced);

    flecs::entity SpawnStar(id_t modelId, Vector2d position);

    // A planet on a circular orbit around `center`, whose angular speed
    // OrbitSystem derives each tick from `centerMass` and the live gravity
    // settings (matching the speed a freely falling ship would need at this
    // radius). Its initial transform is placed at the tick-0 orbit position.
    // `direction` is sign-only: positive/negative picks the orbit direction.
    flecs::entity SpawnOrbitingPlanet(id_t modelId, Vector2d center, double centerMass,
                                      double radius, double direction, double phase);

    // sensor: true for bullets whose hits are resolved by DamageSystem's
    // segment query rather than Chipmunk collision response (see RigidBodyDesc).
    flecs::entity SpawnBullet(id_t modelId, Vector2d position, Vector2d velocity, bool sensor = false);

    // Resolves a NetId to its live entity, or a default (invalid) entity if no
    // entity currently holds that NetId.
    [[nodiscard]] flecs::entity EntityForNetId(std::uint32_t netId) const;
};

} // namespace Gravitaris
