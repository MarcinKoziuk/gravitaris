#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/callsign.hpp>
#include <gravitaris/game/net/protocol.hpp>
#include <gravitaris/game/component/faction-state.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/ai/ai-preset-library.hpp>
#include <gravitaris/game/util/splitmix.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/scenario/classic-scenario.hpp>
#include <gravitaris/game/scenario/starting-complex.hpp>
#include <gravitaris/game/game.hpp>

namespace Gravitaris {

Game::Game(IFilesystem& filesystem, std::unique_ptr<EntitySpawner> entitySpawner)
        : m_filesystem(filesystem)
        , m_resourceLoader(filesystem)
        , m_entitySpawner(std::move(entitySpawner))
        , m_physicsSystem(m_registry)
        , m_orbitSystem(m_registry, m_physicsSystem)
        , m_structureAttachmentSystem(m_registry, *m_entitySpawner, m_physicsSystem)
        , m_structureDefenseSystem(m_registry, *m_entitySpawner, m_eventQueue, m_upgradeCatalog)
        , m_freighterSystem(m_registry, *m_entitySpawner, m_physicsSystem, m_eventQueue, m_economyConfig)
        , m_economySystem(m_registry, *m_entitySpawner, m_eventQueue, m_economyConfig)
        , m_researchSystem(m_registry, *m_entitySpawner, m_eventQueue, m_upgradeCatalog, m_economyConfig)
        , m_inputSystem(m_registry)
        , m_shipControlsSystem(m_registry, *m_entitySpawner, m_physicsSystem, m_eventQueue, m_upgradeCatalog)
        , m_bulletLifetimeSystem(m_registry)
        , m_damageSystem(m_registry, m_physicsSystem, m_eventQueue, m_upgradeCatalog)
        , m_shieldSystem(m_registry, m_upgradeCatalog)
        , m_missileSystem(m_registry, *m_entitySpawner, m_physicsSystem, m_upgradeCatalog)
        , m_factionSystem(m_registry, *m_entitySpawner, m_eventQueue)
        , m_landingStateSystem(m_registry, m_physicsSystem, m_factionSystem)
        , m_repairSystem(m_registry, *m_entitySpawner, m_economyConfig)
        , m_conquestSystem(m_registry, *m_entitySpawner, m_eventQueue, m_factionSystem, m_economyConfig)
        , m_deathSystem(m_registry, *m_entitySpawner, m_eventQueue)
        , m_trajectoryPredictor(m_registry, m_physicsSystem)
        , m_aiPilotSystem(m_registry, m_physicsSystem, m_trajectoryPredictor, m_upgradeCatalog)
        , m_aiStrategySystem(m_registry, m_physicsSystem)
        , m_step(0L)
{
    // Claude: if registry must be initiated first, why don't we construct it before, and pass it to Game::? (could be even moved?)
    // Must happen here, not in EntitySpawner's own constructor: the spawner is
    // built via CreateEntitySpawner(), called as an ARGUMENT to this (possibly
    // delegating/base-class) constructor -- i.e. before m_registry above has
    // actually been constructed, even though it's earlier in this initializer
    // list textually (argument evaluation for a delegating/base-class call
    // happens before ANY of this constructor's own member-initializers run).
    // By the time this constructor BODY executes, every member above is fully
    // constructed, so it's safe for Init() to touch m_registry now.
    m_entitySpawner->Init();

    m_upgradeCatalog.Load(m_filesystem);
    m_aiPresets.Load(m_filesystem);
    m_economyConfig.Load(m_filesystem);
}

void Game::BuildWorld(const SectorParams& params)
{
    SectorParams sanitized = params;
    sanitized.Sanitize();

    const SectorLayout layout = BuildSectorScenario(*m_entitySpawner, sanitized);
    m_sectorExtent = layout.extent;

    // Faction i of the roster starts at the i'th home the generator picked,
    // so a seed always hands the same side the same planet.
    m_roster.assign(FACTION_ROSTER, FACTION_ROSTER + sanitized.factionCount);
    for (std::size_t i = 0; i < m_roster.size(); ++i) {
        BuildStartingComplex(*m_entitySpawner, layout.homes[i], m_roster[i]);
    }

    SettleScenario();
}

void Game::RebuildWorld(const SectorParams& params)
{
    // Collected before destroying any of them: the NetId and PhysicsRef
    // OnRemove observers run during destruct() and touch the registry, which
    // is not something to do halfway through iterating it.
    std::vector<flecs::entity> doomed;
    m_registry.each([&](flecs::entity entity, const NetId&) { doomed.push_back(entity); });
    // FactionState entities carry no NetId (FactionSystem creates them bare),
    // so they need their own sweep or a faction's respawn site and defeat
    // flag would outlive the world it referred to.
    m_registry.each([&](flecs::entity entity, const FactionState&) { doomed.push_back(entity); });

    for (flecs::entity entity : doomed) {
        if (entity.is_alive()) entity.destruct();
    }

    m_player.reset();
    m_playerRespawnTimer = -1;
    m_aiFactions.clear();

    BuildWorld(params);
}

void Game::BuildClassicWorld()
{
    const ClassicScenarioHomes homes = BuildClassicScenario(*m_entitySpawner);
    // One developed complex per side, a sun apart (docs/gravity-well-mode-plan.md
    // Phase 2).
    BuildStartingComplex(*m_entitySpawner, homes.blue, TeamId::Blue);
    BuildStartingComplex(*m_entitySpawner, homes.red, TeamId::Red);

    m_roster = {TeamId::Blue, TeamId::Red};
    m_sectorExtent = CLASSIC_SECTOR_EXTENT;

    SettleScenario();
}

void Game::SetPlayerName(const std::string& name)
{
    const std::size_t begin = name.find_first_not_of(" \t");
    if (begin == std::string::npos) return;

    m_playerName = name.substr(begin, name.find_last_not_of(" \t") - begin + 1)
                           .substr(0, MAX_PLAYER_NAME);
}

void Game::SpawnCombatants(TeamId playerTeam)
{
    // Same site selection a respawn uses (just no funding -- the first
    // fighter is free), so the initial spawn lands exactly where a respawn
    // would rather than at an arbitrary world origin.
    const FactionSystem::SpawnPoint spawn =
            m_factionSystem.SpawnPosition(playerTeam).value_or(FactionSystem::SpawnPoint{});
    m_player = m_entitySpawner->SpawnPlayer("models/ships/fighter-1"_id, spawn.pos, playerTeam,
                                            spawn.vel, spawn.rot);
    m_player->emplace<Callsign>(m_playerName);

    // Every complex the player isn't flying for gets a leader that plays the
    // mode, not just a dogfighter. Per-faction presets are U4's round-setup
    // screen.
    for (TeamId team : m_roster) {
        if (team != playerTeam) AddAIFaction(team, ID("balanced"));
    }
}

void Game::AddAIFaction(TeamId team, id_t preset)
{
    m_aiFactions.push_back(AIFaction{team, preset});
    AIFaction& faction = m_aiFactions.back();

    if (const std::optional<FactionSystem::SpawnPoint> site = m_factionSystem.SpawnPosition(team)) {
        faction.leader = m_entitySpawner->SpawnAILeader("models/ships/fighter-1"_id, site->pos, team,
                                                        ResolveAIPreset(faction.preset), site->vel,
                                                        site->rot);
        faction.leader->emplace<Callsign>(LeaderCallsign(team));
    }
}

// One per faction, so it needs no counter to stay unique -- and it is what
// somebody flying alone types to go and find the enemy (/tp red-leader).
std::string Game::LeaderCallsign(TeamId team)
{
    switch (team) {
    case TeamId::Blue: return "blue-leader";
    case TeamId::Red: return "red-leader";
    case TeamId::Violet: return "violet-leader";
    case TeamId::Yellow: return "yellow-leader";
    case TeamId::Magenta: return "magenta-leader";
    case TeamId::Cyan: return "cyan-leader";
    case TeamId::None: break;
    }
    return "leader";
}

void Game::Start()
{
    BuildClassicWorld();
    SpawnCombatants(TeamId::Blue);
}

Game::Game(IFilesystem& filesystem)
        // Qualified (non-virtual) on purpose: this argument is evaluated
        // while delegating to Game's own constructor, before Game's vptr is
        // installed, and virtual dispatch there jumps through a garbage
        // vtable on at least one toolchain (Apple Clang 21 arm64). A plain
        // Game never overrides this anyway; CGame's base-init is a different
        // case and is fine.
        : Game(filesystem, Game::CreateEntitySpawner())
{}

void Game::SettleScenario()
{
    m_orbitSystem.Update();
    m_structureAttachmentSystem.Update();
}

void Game::Update()
{
    // Emitters read the current tick off the queue rather than threading the
    // step through every EmitEvent call.
    m_eventQueue.SetCurrentTick(m_step);

    {
        ScopedPerfTimer timer(m_perfMonitor, "Physics");

        // Place orbiting bodies on their rails before the step reads positions
        // for gravity and resolves collisions against them.
        m_orbitSystem.Update();
        // Freighters may arrive and get a real PlanetOrbitAttachment this
        // tick -- must run before StructureAttachmentSystem so that
        // attachment is already driven the same tick it's added.
        m_freighterSystem.Update();
        // Planet-attached structures ride the planets' just-updated positions.
        m_structureAttachmentSystem.Update();

        // Debug/tuning only: reapplies every tick (cheap, one cpBodySetMass
        // call) so it stays in effect across a respawn's fresh body without
        // extra bookkeeping -- see m_shipWeightMultiplier's field comment.
        if (m_player) {
            if (const PhysicsRef* ref = m_player->try_get<PhysicsRef>()) {
                m_physicsSystem.SetMassMultiplier(*ref, m_shipWeightMultiplier);
            }
        }

        // Guidance steers before the step integrates it, so a missile's
        // turn lands on the same tick it was decided.
        m_missileSystem.Update();

        m_physicsSystem.Simulate(Game::PHYSICS_DELTA);
        m_physicsSystem.Update();
    }

    {
        ScopedPerfTimer timer(m_perfMonitor, "Game Logic");
        // Before DamageSystem, so this tick's regen is available to this
        // tick's incoming fire.
        m_shieldSystem.Update();
        // DamageSystem applies this step's bullet hits and landing impacts, so
        // DeathSystem (next) sees final hp and can explode ships the same tick.
        m_damageSystem.Update(m_step);
        m_structureDefenseSystem.Update();
        m_landingStateSystem.Update();
        // After LandingStateSystem, so a ship that touched down this tick is
        // already standing when its hull is topped up.
        m_repairSystem.Update();
        m_conquestSystem.Update();
        m_economySystem.Update();
        m_deathSystem.Update(m_step);
        // After DeathSystem: defeat/win checks should see this tick's freshest
        // colony/freighter/planet-ownership state, not last tick's.
        m_factionSystem.Update();
        // After FactionSystem: reads the FactionState entities it creates, and
        // this tick's LandingStateSystem flags for upgrade pickup.
        m_researchSystem.Update(m_step);
        // Detect a player death from DeathSystem before any system reads m_player.
        HandlePlayerRespawn();
        HandleAILeaderRespawns();
        // Before AIPilotSystem, so an order issued this tick is flown this tick.
        m_aiStrategySystem.Update();
        m_aiPilotSystem.Update(m_step);
        m_inputSystem.Update(m_step);
        m_shipControlsSystem.Update(m_step);
        m_bulletLifetimeSystem.Update(Game::PHYSICS_DELTA);
    }

    m_step++;
}

std::optional<FactionSystem::SpawnPoint> Game::TickRespawn(std::optional<flecs::entity>& ship, int& timer,
                                                           TeamId team)
{
    if (ship && !ship->is_alive()) {
        ship.reset();
        timer = RESPAWN_DELAY_TICKS;
    }

    if (timer < 0) return std::nullopt;
    if (timer > 0) {
        --timer;
        return std::nullopt;
    }

    // The timer stays at 0 and retries every tick: a site with no funder yet
    // is a transient wait, no site at all is that faction being out of the
    // round (for the player, game over -- not surfaced yet).
    std::optional<FactionSystem::SpawnPoint> spawn = m_factionSystem.TryRespawn(team);
    if (spawn) timer = -1;
    return spawn;
}

void Game::HandlePlayerRespawn()
{
    // Single-player is always Blue (SpawnPlayer's own default team).
    if (const std::optional<FactionSystem::SpawnPoint> spawn =
                TickRespawn(m_player, m_playerRespawnTimer, TeamId::Blue)) {
        m_player = m_entitySpawner->SpawnPlayer("models/ships/fighter-1"_id, spawn->pos, TeamId::Blue,
                                                spawn->vel, spawn->rot);
        m_player->emplace<Callsign>(m_playerName);
    }
}

void Game::HandleAILeaderRespawns()
{
    for (AIFaction& faction : m_aiFactions) {
        if (const std::optional<FactionSystem::SpawnPoint> spawn =
                    TickRespawn(faction.leader, faction.respawnTimer, faction.team)) {
            faction.leader = m_entitySpawner->SpawnAILeader("models/ships/fighter-1"_id, spawn->pos,
                                                            faction.team, ResolveAIPreset(faction.preset), spawn->vel,
                                                            spawn->rot);
            faction.leader->emplace<Callsign>(LeaderCallsign(faction.team));
        }
    }
}

const AIPreset& Game::ResolveAIPreset(id_t id) const
{
    if (const AIPreset* preset = m_aiPresets.Find(id)) return *preset;
    return m_aiPresets.Default();
}

void Game::SpawnRandomAIShip()
{
    // AI ships are Red (see EntitySpawner::SpawnAIShip), so they launch from
    // Red's own site under the same rule a respawn uses -- off its High Port
    // if it still holds one. Only when that faction has nothing left does a
    // spawn fall back to appearing next to the player.
    FactionSystem::SpawnPoint spawn;
    spawn.pos = Vector2d{300.0, 200.0};
    if (const std::optional<FactionSystem::SpawnPoint> site = m_factionSystem.SpawnPosition(TeamId::Red)) {
        spawn = *site;
    }
    else if (const Transform* transform = m_player ? m_player->try_get<Transform>() : nullptr) {
        spawn.pos = transform->pos + Vector2d{250.0, 150.0};
    }

    std::uint64_t rng = SplitMix64Seed(m_step, m_randomAIShipSpawnCount++);
    const AIPreset& preset = m_aiPresets.PickRandom(static_cast<std::uint32_t>(SplitMix64Next(rng)));
    m_entitySpawner->SpawnAIShip("models/ships/fighter-1"_id, spawn.pos, preset, spawn.vel, spawn.rot);
}

std::unique_ptr<EntitySpawner> Game::CreateEntitySpawner()
{
    return std::make_unique<EntitySpawner>(m_registry, m_resourceLoader);
}

std::uint64_t Game::ComputeStateChecksum()
{
    struct Entry {
        std::uint32_t netId;
        std::int64_t qposX, qposY, qrot, qvelX, qvelY;
    };

    // Quantization scales: 1/1000 world unit, 1/100000 rad, 1/1000 unit/s.
    constexpr double POS_SCALE = 1000.0;
    constexpr double ROT_SCALE = 100000.0;
    constexpr double VEL_SCALE = 1000.0;

    std::vector<Entry> entries;
    m_registry.each([&](flecs::entity, const Transform& t, const NetId& netId) {
        entries.push_back(Entry{
                netId.value,
                static_cast<std::int64_t>(std::llround(t.pos.x() * POS_SCALE)),
                static_cast<std::int64_t>(std::llround(t.pos.y() * POS_SCALE)),
                static_cast<std::int64_t>(std::llround(static_cast<double>(t.rot) * ROT_SCALE)),
                static_cast<std::int64_t>(std::llround(t.vel.x() * VEL_SCALE)),
                static_cast<std::int64_t>(std::llround(t.vel.y() * VEL_SCALE)),
        });
    });

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.netId < b.netId; });

    std::uint64_t hash = 1469598103934665603ull; // FNV-1a 64-bit offset basis
    constexpr std::uint64_t FNV_PRIME = 1099511628211ull;
    const auto mix = [&](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            hash ^= (v >> (i * 8)) & 0xFFull;
            hash *= FNV_PRIME;
        }
    };
    for (const Entry& e : entries) {
        mix(e.netId);
        mix(static_cast<std::uint64_t>(e.qposX));
        mix(static_cast<std::uint64_t>(e.qposY));
        mix(static_cast<std::uint64_t>(e.qrot));
        mix(static_cast<std::uint64_t>(e.qvelX));
        mix(static_cast<std::uint64_t>(e.qvelY));
    }
    return hash;
}

} // namespace Gravitaris