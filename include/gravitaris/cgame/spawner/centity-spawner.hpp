#pragma once

#include <gravitaris/game/spawner/entity-spawner.hpp>

namespace Gravitaris {

class CEntitySpawner : public EntitySpawner {
protected:
    void AddRenderable(flecs::entity entity, id_t modelId) override;

public:
    explicit CEntitySpawner(flecs::world& registry, ResourceLoader& resourceLoader);

    ~CEntitySpawner() override = default;
};

} // namespace Gravitaris
