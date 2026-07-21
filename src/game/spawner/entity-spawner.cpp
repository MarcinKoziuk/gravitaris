#include <cmath>

#include <gravitaris/game/resource/common/resource-loader.hpp>
#include <gravitaris/game/resource/body.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/input-queue.hpp>
#include <gravitaris/game/component/ai-pilot.hpp>
#include <gravitaris/game/component/landing-state.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/planet.hpp>
#include <gravitaris/game/component/gravity-source.hpp>
#include <gravitaris/game/component/orbit.hpp>
#include <gravitaris/game/component/planet-attachment.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>

namespace Gravitaris {

EntitySpawner::EntitySpawner(flecs::world& registry, ResourceLoader& resourceLoader)
    : m_registry(registry)
    , m_resourceLoader(resourceLoader)
{
    // Deliberately empty otherwise -- see Init()'s comment (declared in the
    // header) for why the observer isn't registered here.
}

EntitySpawner::~EntitySpawner()
{
    // The observer closes over `this`; it must not outlive the spawner (same
    // teardown reasoning as PhysicsSystem). m_registry (owned by Game) is
    // declared before the spawner, so it's still alive here.
    if (m_netIdRemovedObserver) m_netIdRemovedObserver.destruct();
}

void EntitySpawner::Init()
{
    // Keep the reverse map current: OnRemove fires on both explicit removal
    // and entity destruction (matches the physics registry's observer). Guard
    // against a stale slot -- only erase if the map still points at this exact
    // entity, since a recycled flecs id could re-register under a new NetId.
    m_netIdRemovedObserver = m_registry.observer<NetId>()
            .event(flecs::OnRemove)
            .each([this](flecs::entity ent, NetId& netId) {
                const auto it = m_netIdToEntity.find(netId.value);
                if (it != m_netIdToEntity.end() && it->second == ent) {
                    m_netIdToEntity.erase(it);
                }
            });
}

void EntitySpawner::AssignNetId(flecs::entity entity)
{
    const std::uint32_t id = m_nextNetId++;
    entity.emplace<NetId>(id);
    m_netIdToEntity[id] = entity;
}

flecs::entity EntitySpawner::EntityForNetId(std::uint32_t netId) const
{
    const auto it = m_netIdToEntity.find(netId);
    return it != m_netIdToEntity.end() ? it->second : flecs::entity{};
}

flecs::entity EntitySpawner::SpawnPlayer(id_t modelId, Vector2d position, TeamId team)
{
    ResourcePtr<const Body> body = m_resourceLoader.Load<Body>(modelId);

    auto entity = m_registry.entity();
    entity.emplace<Transform>(position);
    entity.emplace<RigidBodyDesc>("main"_id, body);
    entity.emplace<Controls>();
    entity.emplace<InputQueue>();
    entity.emplace<Team>(team);
    entity.emplace<Damageable>();
    entity.emplace<LandingState>();
    AssignNetId(entity);
    AddRenderable(entity, modelId);

    return entity;
}

flecs::entity EntitySpawner::SpawnAIShip(id_t modelId, Vector2d position, AIPersonalityPreset preset)
{
    ResourcePtr<const Body> body = m_resourceLoader.Load<Body>(modelId);

    auto entity = m_registry.entity();
    entity.emplace<Transform>(position);
    entity.emplace<RigidBodyDesc>("main"_id, body);
    entity.emplace<Controls>();
    entity.emplace<InputQueue>();
    entity.emplace<AIPilot>();
    entity.emplace<Team>(TeamId::Red);
    entity.emplace<Damageable>();
    entity.emplace<LandingState>();
    ApplyAIPersonalityPreset(entity.get_mut<AIPilot>(), preset);
    AssignNetId(entity);
    AddRenderable(entity, modelId);

    return entity;
}

flecs::entity EntitySpawner::SpawnCelestial(id_t modelId, Vector2d position)
{
    ResourcePtr<const Body> body = m_resourceLoader.Load<Body>(modelId);

    // Same source PhysicsSystem::GetBody(ref) used to read for camera/minimap
    // radius -- baked into the Planet component now instead, so those readers
    // don't need a live PhysicsSystem (see Planet's own doc comment).
    const float radius = body->GetCircleShapes().empty()
            ? 0.f : static_cast<float>(body->GetCircleShapes().front().radius);

    auto entity = m_registry.entity();
    entity.emplace<Transform>(position);
    entity.emplace<RigidBodyDesc>("main"_id, body);
    entity.emplace<Planet>(radius);
    // Present from birth (TeamId::None = unowned) rather than emplaced on
    // first claim, so ownership flips are plain field writes and the
    // existing per-entity teamId replication picks planets up unchanged.
    entity.emplace<Team>(TeamId::None);
    if (body->IsGravitySource()) {
        entity.emplace<GravitySource>(
                GravitySource{body->GetMass(), static_cast<float>(body->GetGravityMultiplier())});
    }
    AssignNetId(entity);
    AddRenderable(entity, modelId);

    return entity;
}

flecs::entity EntitySpawner::SpawnStar(id_t modelId, Vector2d position)
{
    return SpawnCelestial(modelId, position);
}

flecs::entity EntitySpawner::SpawnOrbitingPlanet(id_t modelId, Vector2d center, double centerMass,
                                                 double radius, double direction, double phase)
{
    const Vector2d initialPos = center + Vector2d{std::cos(phase), std::sin(phase)} * radius;

    flecs::entity entity = SpawnCelestial(modelId, initialPos);
    entity.emplace<Orbit>(Orbit{center, centerMass, radius, phase, std::copysign(1.0, direction)});

    return entity;
}

flecs::entity EntitySpawner::SpawnBullet(id_t modelId, Vector2d position, Vector2d velocity, bool sensor)
{
    ResourcePtr<const Body> body = m_resourceLoader.Load<Body>(modelId);

    auto entity = m_registry.entity();
    entity.emplace<Transform>(position, Radd{0}, Vector2d{ 3., 3. }, velocity);
    entity.emplace<RigidBodyDesc>("main"_id, body, sensor);
    AssignNetId(entity);
    AddRenderable(entity, modelId);

    return entity;
}

flecs::entity EntitySpawner::SpawnStructureBase(StructureType type, id_t modelId, Vector2d initialPos, TeamId team)
{
    ResourcePtr<const Body> body = m_resourceLoader.Load<Body>(modelId);

    auto entity = m_registry.entity();
    entity.emplace<Transform>(initialPos);
    entity.emplace<RigidBodyDesc>("main"_id, body);
    entity.emplace<Team>(team);
    entity.emplace<Damageable>();
    entity.emplace<Structure>(Structure{type, 0.f, 0.f});
    if (type == StructureType::Base || type == StructureType::HighPort) {
        entity.emplace<StructureDefense>();
    }
    AssignNetId(entity);
    AddRenderable(entity, modelId);

    return entity;
}

flecs::entity EntitySpawner::SpawnStructure(StructureType type, id_t modelId, flecs::entity planet, TeamId team,
                                            Vector2d localOffset)
{
    const Vector2d initialPos = planet.get<Transform>().pos + localOffset;
    flecs::entity entity = SpawnStructureBase(type, modelId, initialPos, team);
    entity.emplace<PlanetSurfaceAttachment>(PlanetSurfaceAttachment{planet.get<NetId>().value, localOffset});
    return entity;
}

flecs::entity EntitySpawner::SpawnOrbitingStructure(StructureType type, id_t modelId, flecs::entity planet,
                                                    TeamId team, double radius, double direction, double phase)
{
    const Transform& planetTransf = planet.get<Transform>();
    const Vector2d initialPos =
            planetTransf.pos + Vector2d{std::cos(phase), std::sin(phase)} * radius;

    double centerMass = 0.0;
    if (const GravitySource* source = planet.try_get<GravitySource>()) {
        centerMass = source->mass * static_cast<double>(source->multiplier);
    }

    flecs::entity entity = SpawnStructureBase(type, modelId, initialPos, team);
    entity.emplace<PlanetOrbitAttachment>(
            PlanetOrbitAttachment{planet.get<NetId>().value, centerMass, radius, phase, std::copysign(1.0, direction)});
    return entity;
}

flecs::entity EntitySpawner::SpawnFreighter(id_t modelId, Vector2d position, TeamId team, flecs::entity targetPlanet,
                                            BuildOrder buildOrder)
{
    ResourcePtr<const Body> body = m_resourceLoader.Load<Body>(modelId);

    auto entity = m_registry.entity();
    entity.emplace<Transform>(position);
    entity.emplace<RigidBodyDesc>("main"_id, body);
    entity.emplace<Team>(team);
    entity.emplace<Damageable>();
    entity.emplace<Controls>();
    entity.emplace<Freighter>(Freighter{targetPlanet.get<NetId>().value, buildOrder, false});
    AssignNetId(entity);
    AddRenderable(entity, modelId);

    return entity;
}

void EntitySpawner::AddRenderable(flecs::entity entity, id_t modelId)
{}

} // namespace Gravitaris
