#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/game.hpp>

namespace Gravitaris {

Game::Game(IFilesystem& filesystem, std::unique_ptr<EntitySpawner> entitySpawner)
        : m_filesystem(filesystem)
        , m_resourceLoader(filesystem)
        , m_entitySpawner(std::move(entitySpawner))
        , m_physicsSystem(m_registry)
        , m_inputSystem(m_registry)
        , m_shipControlsSystem(m_registry, *m_entitySpawner, m_physicsSystem)
        , m_bulletLifetimeSystem(m_registry)
        , m_damageSystem(m_registry, m_physicsSystem)
        , m_deathSystem(m_registry, *m_entitySpawner)
        , m_trajectoryPredictor(m_registry, m_physicsSystem)
        , m_aiPilotSystem(m_registry, m_physicsSystem, m_trajectoryPredictor)
        , m_step(0L)
{}

void Game::Start()
{
    m_player = m_entitySpawner->SpawnPlayer("models/ships/fighter-1"_id, m_playerSpawnPos);
    m_entitySpawner->SpawnPlanet("models/planets/simple"_id, { -100, -100 });
}

Game::Game(IFilesystem& filesystem)
        : Game(filesystem, CreateEntitySpawner())
{}

void Game::Update()
{
    {
        ScopedPerfTimer timer(m_perfMonitor, "Physics");
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

std::unique_ptr<EntitySpawner> Game::CreateEntitySpawner()
{
    return std::make_unique<EntitySpawner>(m_registry, m_resourceLoader);
}

} // namespace Gravitaris