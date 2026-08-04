#pragma once

#include <memory>
#include <string>
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
#include <gravitaris/game/system/combat/shield-system.hpp>
#include <gravitaris/game/upgrade/upgrade-catalog.hpp>
#include <gravitaris/game/system/combat/missile-system.hpp>
#include <gravitaris/game/system/gwell/faction-system.hpp>
#include <gravitaris/game/system/ship/landing-state-system.hpp>
#include <gravitaris/game/system/gwell/conquest-system.hpp>
#include <gravitaris/game/system/ship/repair-system.hpp>
#include <gravitaris/game/system/combat/death-system.hpp>
#include <gravitaris/game/system/ship/ai-pilot-system.hpp>
#include <gravitaris/game/system/ship/ai-strategy-system.hpp>
#include <gravitaris/game/ai/ai-preset-library.hpp>
#include <gravitaris/game/config/economy-config.hpp>
#include <gravitaris/game/notify/notifier.hpp>
#include <gravitaris/game/gnc/nav/trajectory-predictor.hpp>
#include <gravitaris/game/scenario/sector-scenario.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>

namespace Gravitaris {

class Game {
protected:
    IFilesystem& m_filesystem;

    ResourceLoader m_resourceLoader;

    // The weapon table and research pool (data/upgrades.toml), and the AI
    // temperaments (data/ai-presets.toml). Declared ahead of the systems
    // below, several of which hold a reference to the catalog.
    UpgradeCatalog m_upgradeCatalog;

    AIPresetLibrary m_aiPresets;

    // The economy's rates and the round timers (data/economy.toml).
    EconomyConfig m_economyConfig;

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

    ShieldSystem m_shieldSystem;

    MissileSystem m_missileSystem;

    // Declared before LandingStateSystem/ConquestSystem: both take a
    // reference to it in their constructors (member init order).
    FactionSystem m_factionSystem;

    LandingStateSystem m_landingStateSystem;

    RepairSystem m_repairSystem;

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

    // What this process's own player is called, stamped onto every hull it
    // flies (Callsign) so a respawn keeps the name. A server has one of these
    // per peer instead (NetServer::PeerState::name) and never uses this one.
    std::string m_playerName = "Pilot";

    // Which pilot the local player is, kept across respawns so the Supplies
    // they banked follow them onto the fresh hull (see PilotRef).
    std::uint32_t m_playerPilotId = 0;

    // Countdown after a ship's death before TickRespawn starts retrying
    // TryRespawn. -1 means no respawn pending (alive, or permanently gone).
    int m_playerRespawnTimer = -1;
    static constexpr int RESPAWN_DELAY_TICKS = 90; // 1.5 s at the fixed tick

    // An AI faction fielding a leader fighter (docs/gravity-well-mode-plan.md
    // Phase 5). Populated through AddAIFaction, by Start() in single-player
    // and by the server itself in multiplayer.
    struct AIFaction {
        TeamId team = TeamId::Red;
        id_t preset = 0; // an AIPresetLibrary key; 0 means the library's default
        std::optional<flecs::entity> leader;
        int respawnTimer = -1;
    };
    std::vector<AIFaction> m_aiFactions;

    std::vector<TeamId> m_roster;

    double m_sectorExtent = 0.;

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

    // Where notifications go, and whether anyone asked for them. Both null and
    // off by default: a headless sim-test and a single-player client have
    // nobody to notify.
    std::unique_ptr<INotifier> m_notifier;
    bool m_notificationsEnabled = false;

    // Death -> delay -> funded respawn, one tick's worth. Returns the spawn
    // point on the tick a replacement should be created (its materials are
    // already spent by then); nullopt while alive, waiting out the delay, or
    // waiting for a funder. Shared by the player and every AI leader --
    // no faction-specific respawn rules.
    std::optional<FactionSystem::SpawnPoint> TickRespawn(std::optional<flecs::entity>& ship, int& timer,
                                                         TeamId team);

    void HandlePlayerRespawn();

    void HandleAILeaderRespawns();

    // The Callsign an AI faction's leader flies under, so it can be addressed
    // by name the same way a human can.
    [[nodiscard]] static std::string LeaderCallsign(TeamId team);

    virtual std::unique_ptr<EntitySpawner> CreateEntitySpawner();

public:
    explicit Game(IFilesystem& filesystem);

    Game(IFilesystem& filesystem, std::unique_ptr<EntitySpawner> entitySpawner);

    // Builds a generated sector (docs/sector-generation-plan.md) -- celestials
    // plus a developed complex per faction -- with no combatants in it, so a
    // client can render the world while the player is still picking a side.
    void BuildWorld(const SectorParams& params);

    // Throws the current world away and builds a fresh one on `params`. Only
    // meaningful before combatants exist -- this is the round-setup screen
    // reseeding its backdrop, not a mid-round restart, so it makes no attempt
    // to preserve anything.
    void RebuildWorld(const SectorParams& params);

    // The fixed two-sun blue-vs-red arena. Kept alongside generation as the
    // known-good layout sim-test's proofs run on, so a failing test means the
    // rule under test broke rather than the seed having moved a planet.
    void BuildClassicWorld();

    // Spawns the player on `playerTeam` and an AI faction on every other side
    // this round fields. Call after one of the BuildWorld calls above.
    void SpawnCombatants(TeamId playerTeam);

    // Fields a leader for `team` -- the ship that plays the mode rather than
    // only fighting in it -- and keeps it respawning for the round. `preset`
    // is an AIPresetLibrary key; 0 takes the library's default. A dedicated
    // server calls this itself for the sides no human is playing, since it
    // builds its own scenario instead of calling Start().
    void AddAIFaction(TeamId team, id_t preset = 0);

    // BuildClassicWorld() + SpawnCombatants(TeamId::Blue).
    void Start();

    // The sides this round fields, in FACTION_ROSTER order -- set by whichever
    // BuildWorld call ran. Faction i started at the i'th generated home.
    [[nodiscard]] const std::vector<TeamId>& GetRoster() const { return m_roster; }

    // World radius covering every celestial, for callers that have to frame
    // the whole sector (the minimap). Zero until a world is built.
    [[nodiscard]] double GetSectorExtent() const { return m_sectorExtent; }

    // Places every rail-driven body -- orbiting planets, then the structures
    // riding them -- at its real position and velocity. A freshly spawned
    // Transform carries no velocity, so anything read before the first
    // Update() (a launch site, most of all) otherwise sees a station standing
    // still that is in fact sweeping past at its orbital speed. Call once
    // after building a scenario, before spawning anything into it.
    void SettleScenario();

    void Update();

    // Takes ownership of where notifications go (game/notify). Null, the
    // default, means nowhere.
    void SetNotifier(std::unique_ptr<INotifier> notifier);

    // Reaches whoever is watching this server from outside it. Silently does
    // nothing with no notifier configured or notifications switched off, so
    // call sites don't branch on either.
    void Notify(const std::string& text);

    // Off until something asks for it (the /notify cheat), so a server started
    // with a webhook configured doesn't start posting unbidden.
    void SetNotificationsEnabled(bool enabled) { m_notificationsEnabled = enabled; }

    [[nodiscard]] bool AreNotificationsEnabled() const { return m_notificationsEnabled; }

    // True when a notifier exists at all, which is what separates "switched
    // off" from "this build/server has nowhere to send them".
    [[nodiscard]] bool HasNotifier() const { return m_notifier != nullptr; }

    flecs::world& GetRegistry()
    { return m_registry; }

    PhysicsSystem& GetPhysicsSystem()
    { return m_physicsSystem; }

    ResourceLoader& GetResourceLoader()
    { return m_resourceLoader; }

    const UpgradeCatalog& GetUpgradeCatalog() const
    { return m_upgradeCatalog; }

    const AIPresetLibrary& GetAIPresets() const
    { return m_aiPresets; }

    const EconomyConfig& GetEconomyConfig() const
    { return m_economyConfig; }

    // The preset `id` names, or the library's default when it names nothing
    // (an unconfigured faction, or a key the data file no longer has).
    [[nodiscard]] const AIPreset& ResolveAIPreset(id_t id) const;

    std::optional<flecs::entity> GetPlayer()
    { return m_player; }

    // The name this player flies under. Trimmed and truncated to
    // MAX_PLAYER_NAME; a blank one is ignored, so the default stands. Set
    // before spawning or connecting -- ClientHello is built the moment the
    // transport connects, and a hull already in the world keeps whatever
    // Callsign it launched with.
    void SetPlayerName(const std::string& name);

    [[nodiscard]] const std::string& GetPlayerName() const { return m_playerName; }

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

    DamageSystem& GetDamageSystem()
    { return m_damageSystem; }

    // Every ship destroyed, for whoever writes the kill feed -- single-player
    // straight into its own chat log, a server out to its peers.
    sigslot::signal<const DeathReport&>& OnDeath()
    { return m_deathSystem.OnDeath(); }

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
