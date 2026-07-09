#pragma once

#include <memory>
#include <cstdint>
#include <optional>

#include <entt/entity/registry.hpp>

#include <gravitaris/game/fwd.hpp>
#include <gravitaris/game/resource/common/resource-loader.hpp>
#include <gravitaris/game/system/physics-system.hpp>
#include <gravitaris/game/system/ship-controls-system.hpp>
#include <gravitaris/game/system/bullet-lifetime-system.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>

namespace Gravitaris {

class Game {
protected:
    IFilesystem& m_filesystem;

    ResourceLoader m_resourceLoader;

    entt::registry m_registry;

    std::unique_ptr<EntitySpawner> m_entitySpawner;

    PhysicsSystem m_physicsSystem;

    ShipControlsSystem m_shipControlsSystem;

    BulletLifetimeSystem m_bulletLifetimeSystem;

    std::uint64_t m_step;

    std::optional<entt::entity> m_player;

    virtual std::unique_ptr<EntitySpawner> CreateEntitySpawner();

public:
    explicit Game(IFilesystem& filesystem);

    Game(IFilesystem& filesystem, std::unique_ptr<EntitySpawner> entitySpawner);

    void Start();

    void Update();

    entt::registry& GetRegistry()
    { return m_registry; }

    std::optional<entt::entity> GetPlayer()
    { return m_player; }

    static constexpr double PHYSICS_DELTA = 1. / 60.;
};

} // namespace Gravitaris
