#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/gnc/ai-personality-presets.hpp>
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
        , m_structureDefenseSystem(m_registry, *m_entitySpawner, m_eventQueue)
        , m_freighterSystem(m_registry, *m_entitySpawner, m_physicsSystem, m_eventQueue)
        , m_economySystem(m_registry, *m_entitySpawner, m_eventQueue)
        , m_inputSystem(m_registry)
        , m_shipControlsSystem(m_registry, *m_entitySpawner, m_physicsSystem, m_eventQueue)
        , m_bulletLifetimeSystem(m_registry)
        , m_damageSystem(m_registry, m_physicsSystem, m_eventQueue)
        , m_factionSystem(m_registry, *m_entitySpawner, m_eventQueue)
        , m_landingStateSystem(m_registry, m_physicsSystem, m_factionSystem)
        , m_conquestSystem(m_registry, *m_entitySpawner, m_eventQueue, m_factionSystem)
        , m_deathSystem(m_registry, *m_entitySpawner, m_eventQueue)
        , m_trajectoryPredictor(m_registry, m_physicsSystem)
        , m_aiPilotSystem(m_registry, m_physicsSystem, m_trajectoryPredictor)
        , m_step(0L)
{
    // Must happen here, not in EntitySpawner's own constructor: the spawner is
    // built via CreateEntitySpawner(), called as an ARGUMENT to this (possibly
    // delegating/base-class) constructor -- i.e. before m_registry above has
    // actually been constructed, even though it's earlier in this initializer
    // list textually (argument evaluation for a delegating/base-class call
    // happens before ANY of this constructor's own member-initializers run).
    // By the time this constructor BODY executes, every member above is fully
    // constructed, so it's safe for Init() to touch m_registry now.
    m_entitySpawner->Init();
}

void Game::Start()
{
    m_player = m_entitySpawner->SpawnPlayer("models/ships/fighter-1"_id, m_playerSpawnPos);
    const flecs::entity homePlanet = BuildClassicScenario(*m_entitySpawner);
    // Single, shared starting complex for now (docs/gravity-well-mode-plan.md
    // Phase 2) -- per-faction starting planets are Phase 6's job (sector
    // generation).
    BuildStartingComplex(*m_entitySpawner, homePlanet, TeamId::Blue);
}

Game::Game(IFilesystem& filesystem)
        // Explicitly-qualified (non-virtual) call: this is a delegating
        // constructor of Game itself, not a derived class's base-init (that
        // case -- see CGame::CGame -- is fine; the derived class's own vptr
        // is already set by the time it evaluates the base-class argument).
        // Here, evaluating CreateEntitySpawner() is part of constructing
        // Game via delegation to Game's own two-arg constructor, and at
        // least one observed toolchain (Apple Clang 21 arm64) does not yet
        // have Game's vptr installed at that point, so a virtual call reads
        // a garbage vtable pointer and jumps into invalid memory (reliably
        // reproducible SEGV -- confirmed via AddressSanitizer, and via
        // bisection against every constructor in this class). Since a plain
        // Game (as opposed to CGame) never has this overridden anyway, the
        // qualified call changes nothing observable and sidesteps the whole
        // question.
        : Game(filesystem, Game::CreateEntitySpawner())
{}

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

        m_physicsSystem.Simulate(Game::PHYSICS_DELTA);
        m_physicsSystem.Update();
    }

    {
        ScopedPerfTimer timer(m_perfMonitor, "Game Logic");
        // DamageSystem applies this step's bullet hits and landing impacts, so
        // DeathSystem (next) sees final hp and can explode ships the same tick.
        m_damageSystem.Update();
        m_structureDefenseSystem.Update();
        m_landingStateSystem.Update();
        m_conquestSystem.Update();
        m_economySystem.Update();
        m_deathSystem.Update(m_step);
        // After DeathSystem: defeat/win checks should see this tick's freshest
        // colony/freighter/planet-ownership state, not last tick's.
        m_factionSystem.Update();
        // Detect a player death from DeathSystem before any system reads m_player.
        HandlePlayerRespawn();
        m_aiPilotSystem.Update(m_step, m_player);
        m_inputSystem.Update(m_step);
        m_shipControlsSystem.Update(m_step);
        m_bulletLifetimeSystem.Update(Game::PHYSICS_DELTA);
    }

    m_step++;
}

void Game::HandlePlayerRespawn()
{
    if (m_player && !m_player->is_alive()) {
        m_player.reset();
        m_playerRespawnTimer = RESPAWN_DELAY_TICKS;
    }

    if (m_playerRespawnTimer < 0) return;
    if (m_playerRespawnTimer > 0) {
        --m_playerRespawnTimer;
        return;
    }

    // Timer expired: single-player is always Blue (SpawnPlayer's own
    // default team, never overridden here). Keep retrying every tick from
    // here (m_playerRespawnTimer stays 0) until TryRespawn succeeds -- a
    // site exists but nothing can fund the fighter yet is a transient
    // wait, not a failure -- or permanently doesn't (no friendly
    // planet/High Port left at all -- docs/gravity-well-mode-plan.md
    // Phase 4's "for the player: game over", not otherwise surfaced yet).
    if (const std::optional<Vector2d> pos = m_factionSystem.TryRespawn(TeamId::Blue)) {
        m_player = m_entitySpawner->SpawnPlayer("models/ships/fighter-1"_id, *pos);
        m_playerRespawnTimer = -1;
    }
}

void Game::SpawnRandomAIShip()
{
    static constexpr AIPersonalityPreset PRESETS[] = {
            AIPersonalityPreset::Balanced, AIPersonalityPreset::Aggressive, AIPersonalityPreset::Cautious,
            AIPersonalityPreset::Sniper, AIPersonalityPreset::Reckless,
    };

    Vector2d pos{300.0, 200.0};
    const Transform* transform = m_player ? m_player->try_get<Transform>() : nullptr;
    if (transform) {
        pos = transform->pos + Vector2d{250.0, 150.0};
    }

    std::uint64_t rng = SplitMix64Seed(m_step, m_randomAIShipSpawnCount++);
    const AIPersonalityPreset preset = PRESETS[SplitMix64Next(rng) % std::size(PRESETS)];
    m_entitySpawner->SpawnAIShip("models/ships/fighter-1"_id, pos, preset);
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