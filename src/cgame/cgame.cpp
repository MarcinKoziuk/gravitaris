#include <gravitaris/game/logging.hpp>

#include <gravitaris/game/resource/common/resource-loader.hpp>

#include <gravitaris/cgame/spawner/centity-spawner.hpp>
#include <gravitaris/cgame/cgame.hpp>

namespace Gravitaris {

CGame::CGame(IFilesystem &filesystem)
    : Game(filesystem, CreateEntitySpawner())
    , m_simpleModelRenderer(m_registry, filesystem, m_resourceLoader)
    , m_modelRenderer(m_registry, filesystem, m_resourceLoader)
    , m_modelRenderer2(m_registry, filesystem, m_resourceLoader)
{}

void CGame::Render(double delta)
{
    m_modelRenderer.SetZoom(m_camera.GetZoom());
    m_modelRenderer.SetCameraPosition(m_camera.GetPosition());
    m_modelRenderer.SetLineWidth(m_lineWidthPixels);
    m_modelRenderer2.SetZoom(m_camera.GetZoom());
    m_modelRenderer2.SetCameraPosition(m_camera.GetPosition());
    m_modelRenderer2.SetLineWidth(m_lineWidthPixels);

    // Renderers are mutually exclusive; swap which line is active to compare.
    //m_simpleModelRenderer.Render(delta);
    //m_modelRenderer.Render(delta);
    m_modelRenderer2.Render(delta);
}

std::unique_ptr<EntitySpawner> CGame::CreateEntitySpawner()
{
    return std::make_unique<CEntitySpawner>(m_registry, m_resourceLoader);
}

} // Gravitaris
