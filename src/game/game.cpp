#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/gravity-source.hpp>
#include <gravitaris/game/gnc/ai-personality-presets.hpp>
#include <gravitaris/game/util/splitmix.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/game.hpp>

namespace Gravitaris {

Game::Game(IFilesystem& filesystem, std::unique_ptr<EntitySpawner> entitySpawner)
        : m_filesystem(filesystem)
        , m_resourceLoader(filesystem)
        , m_entitySpawner(std::move(entitySpawner))
        , m_physicsSystem(m_registry)
        , m_orbitSystem(m_registry, m_physicsSystem)
        , m_inputSystem(m_registry)
        , m_shipControlsSystem(m_registry, *m_entitySpawner, m_physicsSystem, m_eventQueue)
        , m_bulletLifetimeSystem(m_registry)
        , m_damageSystem(m_registry, m_physicsSystem, m_eventQueue)
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

    // Hardcoded "classic mode" solar system: two still suns, each with a few
    // green planets on pre-calculated circular orbits. The suns are the
    // dominant gravity wells; the orbiting planets attract too, far less.
    const id_t sun = "models/stars/sun"_id;
    const id_t planet = "models/planets/simple"_id;

    const Vector2d sunA{-5600., 0.};
    const Vector2d sunB{5600., 0.};

    // Orbit angular speed is derived from centerMass at the actual gravity
    // settings (see OrbitSystem), so this is the star's effective attracting
    // mass -- mass * its own gravity multiplier, matching what ApplyGravity
    // computes for it as a source.
    const auto effectiveMass = [](flecs::entity star) {
        return star.get<GravitySource>().mass * star.get<GravitySource>().multiplier;
    };

    const double sunAMass = effectiveMass(m_entitySpawner->SpawnStar(sun, sunA));
    m_entitySpawner->SpawnOrbitingPlanet(planet, sunA, sunAMass, 2000., 1.0, 0.0);
    m_entitySpawner->SpawnOrbitingPlanet(planet, sunA, sunAMass, 3400., -1.0, 2.1);
    m_entitySpawner->SpawnOrbitingPlanet(planet, sunA, sunAMass, 4800., 1.0, 4.0);

    const double sunBMass = effectiveMass(m_entitySpawner->SpawnStar(sun, sunB));
    m_entitySpawner->SpawnOrbitingPlanet(planet, sunB, sunBMass, 2200., -1.0, 1.0);
    m_entitySpawner->SpawnOrbitingPlanet(planet, sunB, sunBMass, 4000., 1.0, 3.5);
}

Game::Game(IFilesystem& filesystem)
        : Game(filesystem, CreateEntitySpawner())
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
        m_deathSystem.Update(m_step);
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

    if (m_playerRespawnTimer > 0 && --m_playerRespawnTimer == 0) {
        m_player = m_entitySpawner->SpawnPlayer("models/ships/fighter-1"_id, m_playerSpawnPos);
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