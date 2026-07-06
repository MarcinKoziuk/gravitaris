#include <gravitaris/game/logging.hpp>

#include <gravitaris/game/resource/common/resource-loader.hpp>

#include <gravitaris/cgame/spawner/centity-spawner.hpp>
#include <gravitaris/cgame/cgame.hpp>

namespace Gravitaris {

CGame::CGame(IFilesystem &filesystem)
    : Game(filesystem, CreateEntitySpawner())
    , m_simpleModelRenderer(m_registry, filesystem, m_resourceLoader)
    , m_modelRenderer(m_registry, filesystem, m_resourceLoader)
{}

void CGame::Render(double delta)
{
    m_simpleModelRenderer.Render(delta);
    //m_modelRenderer.Render(delta);
}

std::unique_ptr<EntitySpawner> CGame::CreateEntitySpawner()
{
    return std::make_unique<CEntitySpawner>(m_registry, m_resourceLoader);
}

} // Gravitaris
