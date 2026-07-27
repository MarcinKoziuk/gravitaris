#pragma once

#include <memory>
#include <cstdint>
#include <optional>
#include <vector>

#include <flecs.h>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/fwd.hpp>
#include <gravitaris/game/perf-monitor.hpp>
#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/resource/common/resource-loader.hpp>
#include <gravitaris/game/system/core/physics-system.hpp>
#include <gravitaris/game/system/core/orbit-system.hpp>
#include <gravitaris/game/system/core/structure-attachment-system.hpp>
#include <gravitaris/game/system/combat/structure-defense-system.hpp>
#include <gravitaris/game/system/gwell/freighter-system.hpp>
#include <gravitaris/game/system/gwell/economy-system.hpp>
#include <gravitaris/game/system/gwell/research-system.hpp>
#include <gravitaris/game/system/ship/input-system.hpp>
#include <gravitaris/game/system/ship/ship-controls-system.hpp>
#include <gravitaris/game/system/combat/bullet-lifetime-system.hpp>
#include <gravitaris/game/system/combat/damage-system.hpp>
#include <gravitaris/game/system/gwell/faction-system.hpp>
#include <gravitaris/game/system/ship/landing-state-system.hpp>
#include <gravitaris/game/system/gwell/conquest-system.hpp>
#include <gravitaris/game/system/combat/death-system.hpp>
#include <gravitaris/game/system/ship/ai-pilot-system.hpp>
#include <gravitaris/game/system/ship/ai-strategy-system.hpp>
#include <gravitaris/game/gnc/ai-personality-presets.hpp>
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

    OrbitSystem m_orbitSystem;

    StructureAttachmentSystem m_structureAttachmentSystem;

    StructureDefenseSystem m_structureDefenseSystem;

    FreighterSystem m_freighterSystem;

    EconomySystem m_economySystem;

    ResearchSystem m_researchSystem;

    InputSystem m_inputSystem;

    ShipControlsSystem m_shipControlsSystem;

    BulletLifetimeSystem m_bulletLifetimeSystem;

    DamageSystem m_damageSystem;

    // Declared before LandingStateSystem/ConquestSystem: both take a
    // reference to it in their constructors (member init order).
    FactionSystem m_factionSystem;

    LandingStateSystem m_landingStateSystem;

    ConquestSystem m_conquestSystem;

    DeathSystem m_deathSystem;

    TrajectoryPredictor m_trajectoryPredictor;

    AIPilotSystem m_aiPilotSystem;

    AIStrategySystem m_aiStrategySystem;

    // Dev performance overlay's timing data; sections are recorded from
    // Game::Update() (physics/game logic) and, in CGame, from Render() too.
    PerfMonitor m_perfMonitor;

    std::uint64_t m_step;

    std::optional<flecs::entity> m_player;

    // Countdown after a ship's death before TickRespawn starts retrying
    // TryRespawn. -1 means no respawn pending (alive, or permanently gone).
    int m_playerRespawnTimer = -1;
    static constexpr int RESPAWN_DELAY_TICKS = 90; // 1.5 s at the fixed tick

    // An AI faction fielding a leader fighter (docs/gravity-well-mode-plan.md
    // Phase 5). Populated by Start(); a dedicated server builds its own
    // scenario and fields none until round setup exists.
    struct AIFaction {
        TeamId team = TeamId::Red;
        AIPersonalityPreset preset = AIPersonalityPreset::Balanced;
        std::optional<flecs::entity> leader;
        int respawnTimer = -1;
    };
    std::vector<AIFaction> m_aiFactions;

    // Deterministic per-(tick, spawn) seed for SpawnRandomAIShip's preset pick
    // (ADR 0001: no std::rand -- it mutates sim state, so it must be
    // reproducible under replay). Incremented per call so repeated presses
    // within one tick still diverge.
    std::uint32_t m_randomAIShipSpawnCount = 0;

    // Debug/tuning knob (see the Physics debug tab): scales the player's live
    // Chipmunk mass off its resource-authored base every tick
    // (PhysicsSystem::SetMassMultiplier), so a respawn's fresh body picks it
    // up too without extra bookkeeping. 1 = unmodified -- the sim-test and
    // any other headless Game never touch the setter, so this is a no-op
    // there; CGame sets its own tuned default at startup.
    float m_shipWeightMultiplier = 1.f;

    // Death -> delay -> funded respawn, one tick's worth. Returns the spawn
    // point on the tick a replacement should be created (its materials are
    // already spent by then); nullopt while alive, waiting out the delay, or
    // waiting for a funder. Shared by the player and every AI leader --
    // no faction-specific respawn rules.
    std::optional<FactionSystem::SpawnPoint> TickRespawn(std::optional<flecs::entity>& ship, int& timer,
                                                         TeamId team);

    void HandlePlayerRespawn();

    void HandleAILeaderRespawns();

    virtual std::unique_ptr<EntitySpawner> CreateEntitySpawner();

public:
    explicit Game(IFilesystem& filesystem);

    Game(IFilesystem& filesystem, std::unique_ptr<EntitySpawner> entitySpawner);

    // Builds the scenario -- celestials plus a developed complex per side --
    // with no combatants in it, so a client can render the world while the
    // player is still picking a side.
    void BuildWorld();

    // Spawns the player on `playerTeam` and an AI faction on the opposing
    // side. Call after BuildWorld().
    void SpawnCombatants(TeamId playerTeam);

    // BuildWorld() + SpawnCombatants(TeamId::Blue).
    void Start();

    // Places every rail-driven body -- orbiting planets, then the structures
    // riding them -- at its real position and velocity. A freshly spawned
    // Transform carries no velocity, so anything read before the first
    // Update() (a launch site, most of all) otherwise sees a station standing
    // still that is in fact sweeping past at its orbital speed. Call once
    // after building a scenario, before spawning anything into it.
    void SettleScenario();

    void Update();

    flecs::world& GetRegistry()
    { return m_registry; }

    PhysicsSystem& GetPhysicsSystem()
    { return m_physicsSystem; }

    ResourceLoader& GetResourceLoader()
    { return m_resourceLoader; }

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

    FactionSystem& GetFactionSystem()
    { return m_factionSystem; }

    DamageSystem::LandingParams& GetLandingDamageParams()
    { return m_damageSystem.GetLandingParams(); }

    // The sim's one-shot event stream (docs/networking-plan.md Phase 1).
    // Consumers keep their own cursor and read via ConsumeSince.
    [[nodiscard]] const GameEventQueue& GetEventQueue() const { return m_eventQueue; }

    [[nodiscard]] float GetShipWeightMultiplier() const { return m_shipWeightMultiplier; }
    void SetShipWeightMultiplier(float multiplier) { m_shipWeightMultiplier = multiplier; }

    // Spawns an AI fighter near the player with a random personality preset;
    // shared by the Spawn debug tab's button and the J shortcut. A sim
    // mutation (like any Spawn*), so it lives here rather than in cgame --
    // under netcode this becomes a server command handler.
    void SpawnRandomAIShip();

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
