#include <gravitaris/game/resource/common/resource-loader.hpp>
#include <gravitaris/game/resource/body.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>

namespace Gravitaris {

EntitySpawner::EntitySpawner(entt::registry& registry, ResourceLoader& resourceLoader)
    : m_registry(registry)
    , m_resourceLoader(resourceLoader)
{}

entt::entity EntitySpawner::SpawnPlayer(id_t modelId, Vector2d position)
{
    ResourcePtr<const Body> body = m_resourceLoader.Load<Body>(modelId);

    auto entity = m_registry.create();
    auto& t = m_registry.emplace<Transform>(entity, position);
    m_registry.emplace<Physics>(entity, "main"_id, body);
    m_registry.emplace<Controls>(entity);
    AddRenderable(entity, modelId);

    return entity;
}

entt::entity EntitySpawner::SpawnPlanet(id_t modelId, Vector2d position)
{
    ResourcePtr<const Body> body = m_resourceLoader.Load<Body>(modelId);

    auto entity = m_registry.create();
    m_registry.emplace<Transform>(entity, position);
    m_registry.emplace<Physics>(entity, "main"_id, body);
    AddRenderable(entity, modelId);

    return entity;
}

entt::entity EntitySpawner::SpawnBullet(id_t modelId, Vector2d position, Vector2d velocity)
{
    ResourcePtr<const Body> body = m_resourceLoader.Load<Body>(modelId);

    auto entity = m_registry.create();
    m_registry.emplace<Transform>(entity, position, Radd{0}, Vector2d{ 3., 3. }, velocity);
    m_registry.emplace<Physics>(entity, "main"_id, body);
    AddRenderable(entity, modelId);

    return entity;
}

void EntitySpawner::AddRenderable(entt::entity entity, id_t modelId)
{}

} // namespace Gravitaris
