#include <optional>

#include <gravitaris/game/logging.hpp>

#include <gravitaris/game/resource/common/resource-loader.hpp>
#include <gravitaris/game/component/transform.hpp>

#include <gravitaris/cgame/spawner/centity-spawner.hpp>
#include <gravitaris/cgame/cgame.hpp>

namespace Gravitaris {

CGame::CGame(IFilesystem &filesystem)
    : Game(filesystem, CreateEntitySpawner())
    , m_simpleModelRenderer(m_registry, filesystem, m_resourceLoader)
    , m_modelRenderer(m_registry, filesystem, m_resourceLoader)
    , m_modelRenderer2(m_registry, filesystem, m_resourceLoader)
{
    m_camera.SetZoom(2.f);
}

void CGame::UpdateCameraFollow()
{
    if (!m_cameraFollow) return;

    const std::optional<entt::entity> player = GetPlayer();
    if (!player) return;

    const Transform* transform = m_registry.try_get<Transform>(*player);
    if (!transform) return;

    const Magnum::Vector2 target{static_cast<float>(transform->pos.x()),
                                 static_cast<float>(transform->pos.y())};

    // Visible half-extent in world units (ModelRenderer2 uses 1 px/unit at
    // zoom 1). Dead zone is a fraction of it.
    const Magnum::Vector2 halfExtent = m_viewportSize / (2.f * m_camera.GetZoom());
    m_camera.FollowWithDeadZone(target, halfExtent * DEAD_ZONE_FRACTION);
}

void CGame::Render(double delta)
{
    UpdateCameraFollow();

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
