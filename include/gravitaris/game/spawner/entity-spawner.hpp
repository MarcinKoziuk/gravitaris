#pragma once

#include <entt/entity/entity.hpp>
#include <entt/entity/registry.hpp>

#include <Magnum/Magnum.h>

#include <gravitaris/game/fwd.hpp>
#include <gravitaris/game/id.hpp>

namespace Gravitaris {

using Magnum::Vector2d;

class EntitySpawner {
protected:
    entt::registry& m_registry;

    ResourceLoader& m_resourceLoader;

    virtual void AddRenderable(entt::entity entity, id_t modelId);

public:
    explicit EntitySpawner(entt::registry& registry, ResourceLoader& resourceLoader);

    virtual ~EntitySpawner() = default;

    entt::entity SpawnPlayer(id_t modelId, Vector2d position);

    entt::entity SpawnPlanet(id_t modelId, Vector2d position);

    entt::entity SpawnBullet(id_t modelId, Vector2d position, Vector2d velocity);
};

} // namespace Gravitaris
