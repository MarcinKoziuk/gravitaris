#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/game.hpp>

namespace Gravitaris {

Game::Game(IFilesystem& filesystem, std::unique_ptr<EntitySpawner> entitySpawner)
        : m_filesystem(filesystem)
        , m_resourceLoader(filesystem)
        , m_entitySpawner(std::move(entitySpawner))
        , m_physicsSystem(m_registry)
        , m_shipControlsSystem(m_registry, *m_entitySpawner)
        , m_step(0L)
{}

void Game::Start()
{
    m_player = m_entitySpawner->SpawnPlayer("models/ships/fighter-1"_id, { 1, 1 });
    m_entitySpawner->SpawnPlanet("models/planets/simple"_id, { -100, -100 });
}

Game::Game(IFilesystem& filesystem)
        : Game(filesystem, CreateEntitySpawner())
{}

void Game::Update()
{
    m_physicsSystem.Simulate(Game::PHYSICS_DELTA);
    m_physicsSystem.Update();
    m_shipControlsSystem.Update(m_step);

    m_step++;
}

std::unique_ptr<EntitySpawner> Game::CreateEntitySpawner()
{
    return std::make_unique<EntitySpawner>(m_registry, m_resourceLoader);
}

} // namespace Gravitaris