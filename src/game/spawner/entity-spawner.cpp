#include <cmath>

#include <gravitaris/game/resource/common/resource-loader.hpp>
#include <gravitaris/game/resource/body.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/input-queue.hpp>
#include <gravitaris/game/component/ai-pilot.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/planet.hpp>
#include <gravitaris/game/component/gravity-source.hpp>
#include <gravitaris/game/component/orbit.hpp>
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

flecs::entity EntitySpawner::SpawnPlayer(id_t modelId, Vector2d position)
{
    ResourcePtr<const Body> body = m_resourceLoader.Load<Body>(modelId);

    auto entity = m_registry.entity();
    entity.emplace<Transform>(position);
    entity.emplace<RigidBodyDesc>("main"_id, body);
    entity.emplace<Controls>();
    entity.emplace<InputQueue>();
    entity.emplace<Team>(TeamId::Blue);
    entity.emplace<Damageable>();
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
    ApplyAIPersonalityPreset(entity.get_mut<AIPilot>(), preset);
    AssignNetId(entity);
    AddRenderable(entity, modelId);

    return entity;
}

flecs::entity EntitySpawner::SpawnCelestial(id_t modelId, Vector2d position)
{
    ResourcePtr<const Body> body = m_resourceLoader.Load<Body>(modelId);

    auto entity = m_registry.entity();
    entity.emplace<Transform>(position);
    entity.emplace<RigidBodyDesc>("main"_id, body);
    entity.add<Planet>();
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

flecs::entity EntitySpawner::SpawnPlanet(id_t modelId, Vector2d position)
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

void EntitySpawner::AddRenderable(flecs::entity entity, id_t modelId)
{}

} // namespace Gravitaris
