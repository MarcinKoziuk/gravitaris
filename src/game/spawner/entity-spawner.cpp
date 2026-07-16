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
#include <gravitaris/game/spawner/entity-spawner.hpp>

namespace Gravitaris {

EntitySpawner::EntitySpawner(flecs::world& registry, ResourceLoader& resourceLoader)
    : m_registry(registry)
    , m_resourceLoader(resourceLoader)
{}

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
    AddRenderable(entity, modelId);

    return entity;
}

flecs::entity EntitySpawner::SpawnPlanet(id_t modelId, Vector2d position)
{
    ResourcePtr<const Body> body = m_resourceLoader.Load<Body>(modelId);

    auto entity = m_registry.entity();
    entity.emplace<Transform>(position);
    entity.emplace<RigidBodyDesc>("main"_id, body);
    entity.add<Planet>();
    AddRenderable(entity, modelId);

    return entity;
}

flecs::entity EntitySpawner::SpawnBullet(id_t modelId, Vector2d position, Vector2d velocity, bool sensor)
{
    ResourcePtr<const Body> body = m_resourceLoader.Load<Body>(modelId);

    auto entity = m_registry.entity();
    entity.emplace<Transform>(position, Radd{0}, Vector2d{ 3., 3. }, velocity);
    entity.emplace<RigidBodyDesc>("main"_id, body, sensor);
    AddRenderable(entity, modelId);

    return entity;
}

void EntitySpawner::AddRenderable(flecs::entity entity, id_t modelId)
{}

} // namespace Gravitaris
