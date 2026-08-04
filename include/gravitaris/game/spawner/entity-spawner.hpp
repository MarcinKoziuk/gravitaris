#pragma once

#include <cstdint>

#include <flecs.h>

#include <Magnum/Magnum.h>

#include <ankerl/unordered_dense.h>

#include <gravitaris/game/fwd.hpp>
#include <gravitaris/game/id.hpp>
#include <gravitaris/game/component/freighter.hpp>
#include <gravitaris/game/component/structure.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/ai/ai-preset-library.hpp>

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

    // A fresh pilot identity, which every spawned ship gets. Whoever respawns
    // a pilot overwrites the PilotRef with the id that ship was flying under,
    // so the account it built up carries over -- see NetServer's PeerState and
    // Game::HandlePlayerRespawn. A ship nobody claims keeps the fresh one,
    // which is what an AI wants: a new hull is a new pilot.
    std::uint32_t AllocatePilotId() { return m_nextPilotId++; }

    // Shared setup for stars and planets: a kinematic body (per the Body's
    // physics.kinematic) tagged Planet, with a GravitySource attached when the
    // Body declares one.
    flecs::entity SpawnCelestial(id_t modelId, Vector2d position);

    // Shared setup for both structure flavors below: kinematic body, Team,
    // Damageable, and the Structure component itself. Callers attach
    // whichever PlanetSurfaceAttachment/PlanetOrbitAttachment applies and
    // set the real initial position (StructureAttachmentSystem overwrites it
    // on the very next tick anyway, but a fresh entity should never render
    // one tick at the origin).
    flecs::entity SpawnStructureBase(StructureType type, id_t modelId, Vector2d initialPos, TeamId team);

private:
    std::uint32_t m_nextNetId = 1; // 0 stays reserved as "invalid" (see NetId)
    std::uint32_t m_nextPilotId = 1; // 0 stays reserved as "nobody" (see PilotRef)
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

    // `velocity`/`rot` are how the ship starts out, not just where: a
    // fighter launched from a station or a planet inherits that body's
    // motion, and one left at rest in world space is simply run over by the
    // home it launched from (see FactionSystem::SpawnPoint).
    flecs::entity SpawnPlayer(id_t modelId, Vector2d position, TeamId team = TeamId::Blue,
                              Vector2d velocity = {}, double rot = 0.0);

    flecs::entity SpawnAIShip(id_t modelId, Vector2d position, const AIPreset& preset,
                              Vector2d velocity = {}, double rot = 0.0, TeamId team = TeamId::Red);

    // An AI ship that plays the mode rather than only fighting in it: the
    // same fighter plus an AIStrategy weighted by `preset` (see
    // AIStrategySystem). One per AI faction, respawned by whoever owns the
    // faction's slot.
    flecs::entity SpawnAILeader(id_t modelId, Vector2d position, TeamId team, const AIPreset& preset,
                                Vector2d velocity = {}, double rot = 0.0);

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
    // `rot`/`scale` matter for a projectile whose drawing has a facing (a
    // missile); a bullet is a dot, hence the round-dot defaults.
    flecs::entity SpawnBullet(id_t modelId, Vector2d position, Vector2d velocity, bool sensor = false,
                              double rot = 0.0, Vector2d scale = Vector2d{3., 3.});

    // A structure nested inside `planet`'s outline (planetside: Base,
    // Colony, Lab, Comm Center) at the slot StructureLayout gives its type --
    // StructureAttachmentSystem keeps it riding the planet's own motion every
    // tick.
    flecs::entity SpawnStructure(StructureType type, id_t modelId, flecs::entity planet, TeamId team);

    // A structure on a circular orbit around `planet` (the High Port) --
    // same StructureAttachmentSystem
    // upkeep as SpawnStructure, orbit math instead of a fixed offset.
    // `direction`/`phase` match SpawnOrbitingPlanet's own parameters.
    flecs::entity SpawnOrbitingStructure(StructureType type, id_t modelId, flecs::entity planet, TeamId team,
                                         double radius, double direction, double phase);

    // A freighter dispatched to build `buildOrder` at `targetPlanet` --
    // kinematic (FreighterSystem drives its transit, then a real
    // PlanetOrbitAttachment once arrived), Team+Damageable like a ship
    // (interceptable). Carries Controls but no InputQueue -- nothing pilots
    // it; FreighterSystem sets thrustForward during transit purely so the
    // existing _thrust visual/audio/replication pipeline lights up (forces
    // from ShipControlsSystem are ignored on a kinematic body).
    flecs::entity SpawnFreighter(id_t modelId, Vector2d position, TeamId team, flecs::entity targetPlanet,
                                 BuildOrder buildOrder);

    // Resolves a NetId to its live entity, or a default (invalid) entity if no
    // entity currently holds that NetId.
    [[nodiscard]] flecs::entity EntityForNetId(std::uint32_t netId) const;
};

} // namespace Gravitaris
