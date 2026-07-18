#pragma once

#include <memory>
#include <cstdint>
#include <optional>

#include <flecs.h>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/fwd.hpp>
#include <gravitaris/game/perf-monitor.hpp>
#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/resource/common/resource-loader.hpp>
#include <gravitaris/game/system/physics-system.hpp>
#include <gravitaris/game/system/input-system.hpp>
#include <gravitaris/game/system/ship-controls-system.hpp>
#include <gravitaris/game/system/bullet-lifetime-system.hpp>
#include <gravitaris/game/system/damage-system.hpp>
#include <gravitaris/game/system/death-system.hpp>
#include <gravitaris/game/system/ai-pilot-system.hpp>
#include <gravitaris/game/gnc/nav/trajectory-predictor.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>

namespace Gravitaris {

class Game {
protected:
    IFilesystem& m_filesystem;

    ResourceLoader m_resourceLoader;

    flecs::world m_registry;

    std::unique_ptr<EntitySpawner> m_entitySpawner;

    // Declared before the systems below: several take a reference to it in
    // their constructors (member init order).
    GameEventQueue m_eventQueue;

    PhysicsSystem m_physicsSystem;

    InputSystem m_inputSystem;

    ShipControlsSystem m_shipControlsSystem;

    BulletLifetimeSystem m_bulletLifetimeSystem;

    DamageSystem m_damageSystem;

    DeathSystem m_deathSystem;

    TrajectoryPredictor m_trajectoryPredictor;

    AIPilotSystem m_aiPilotSystem;

    // Dev performance overlay's timing data; sections are recorded from
    // Game::Update() (physics/game logic) and, in CGame, from Render() too.
    PerfMonitor m_perfMonitor;

    std::uint64_t m_step;

    std::optional<flecs::entity> m_player;

    // Where to (re)spawn the player, and the countdown after a death. -1 means
    // no respawn pending (alive, or permanently gone).
    Magnum::Vector2d m_playerSpawnPos{1., 1.};
    int m_playerRespawnTimer = -1;
    static constexpr int RESPAWN_DELAY_TICKS = 90; // 1.5 s at the fixed tick

    void HandlePlayerRespawn();

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

    PerfMonitor& GetPerfMonitor()
    { return m_perfMonitor; }

    EntitySpawner& GetEntitySpawner()
    { return *m_entitySpawner; }

    // The sim's one-shot event stream (docs/networking-plan.md Phase 1).
    // Consumers keep their own cursor and read via ConsumeSince.
    [[nodiscard]] const GameEventQueue& GetEventQueue() const { return m_eventQueue; }

    // FNV-1a over every NetId-bearing entity's (NetId, quantized pos/rot/vel),
    // sorted by NetId first (flecs iteration order is not guaranteed stable
    // across worlds, so hashing in table-encounter order would make two
    // otherwise-identical states hash differently). Quantized rather than
    // hashing raw doubles: same-process reruns would match on raw bits too,
    // but this checksum is meant to keep working once state is compared
    // across peers (ADR 0001 already flags floats as non-cross-platform
    // -deterministic; quantizing narrows how much that can bite). Used by
    // the headless sim-test target to catch nondeterminism regressions.
    [[nodiscard]] std::uint64_t ComputeStateChecksum();

    static constexpr double PHYSICS_DELTA = 1. / 60.;
};

} // namespace Gravitaris
