#pragma once

#include <memory>
#include <cstdint>
#include <optional>

#include <flecs.h>

#include <gravitaris/game/fwd.hpp>
#include <gravitaris/game/resource/common/resource-loader.hpp>
#include <gravitaris/game/system/physics-system.hpp>
#include <gravitaris/game/system/input-system.hpp>
#include <gravitaris/game/system/ship-controls-system.hpp>
#include <gravitaris/game/system/bullet-lifetime-system.hpp>
#include <gravitaris/game/nav/trajectory-predictor.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>

namespace Gravitaris {

class Game {
protected:
    IFilesystem& m_filesystem;

    ResourceLoader m_resourceLoader;

    flecs::world m_registry;

    std::unique_ptr<EntitySpawner> m_entitySpawner;

    PhysicsSystem m_physicsSystem;

    InputSystem m_inputSystem;

    ShipControlsSystem m_shipControlsSystem;

    BulletLifetimeSystem m_bulletLifetimeSystem;

    TrajectoryPredictor m_trajectoryPredictor;

    std::uint64_t m_step;

    std::optional<flecs::entity> m_player;

    virtual std::unique_ptr<EntitySpawner> CreateEntitySpawner();

public:
    explicit Game(IFilesystem& filesystem);

    Game(IFilesystem& filesystem, std::unique_ptr<EntitySpawner> entitySpawner);

    void Start();

    void Update();

    flecs::world& GetRegistry()
    { return m_registry; }

    std::optional<flecs::entity> GetPlayer()
    { return m_player; }

    // The tick the next Update() will run. Producers stamp commands with this
    // before calling Update() so InputSystem consumes them on the right tick.
    std::uint64_t GetStep() const
    { return m_step; }

    TrajectoryPredictor& GetTrajectoryPredictor()
    { return m_trajectoryPredictor; }

    static constexpr double PHYSICS_DELTA = 1. / 60.;
};

} // namespace Gravitaris
