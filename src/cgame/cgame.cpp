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
    , m_modelRenderer2(m_registry, filesystem, m_resourceLoader)
{
    m_camera.SetZoom(2.f);
}

void CGame::UpdateCameraFollow()
{
    if (!m_cameraFollow) return;

    const std::optional<flecs::entity> player = GetPlayer();
    if (!player) return;

    const Transform* transform = player->try_get<Transform>();
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

    m_simpleModelRenderer.SetZoom(m_camera.GetZoom());
    m_simpleModelRenderer.SetCameraPosition(m_camera.GetPosition());
    m_modelRenderer2.SetZoom(m_camera.GetZoom());
    m_modelRenderer2.SetCameraPosition(m_camera.GetPosition());
    m_modelRenderer2.SetLineWidth(m_lineWidthPixels);

    // Renderers are mutually exclusive; the debug UI picks the active one.
    switch (m_activeRenderer) {
        case RendererKind::Simple:
            m_simpleModelRenderer.Render(delta);
            break;
        case RendererKind::Baked:
            m_modelRenderer2.Render(delta);
            break;
    }
}

std::unique_ptr<EntitySpawner> CGame::CreateEntitySpawner()
{
    return std::make_unique<CEntitySpawner>(m_registry, m_resourceLoader);
}

void CGame::SetAutopilotMode(AutopilotMode mode)
{
    if (mode == AutopilotMode::HoldPosition) {
        const std::optional<flecs::entity> player = GetPlayer();
        const Transform* transform = player ? player->try_get<Transform>() : nullptr;
        if (!transform) return; // nothing to anchor to; stay in current mode
        m_autopilotAnchor = transform->pos;
    }
    m_autopilotMode = mode;
}

std::optional<ControlFlags> CGame::ComputeAutopilotControls()
{
    if (m_autopilotMode == AutopilotMode::Off) return std::nullopt;

    const std::optional<flecs::entity> player = GetPlayer();
    const Transform* transform = player ? player->try_get<Transform>() : nullptr;
    if (!transform) return std::nullopt;

    const Magnum::Math::Vector2<double> desiredVel =
            (m_autopilotMode == AutopilotMode::KillVelocity)
                    ? Magnum::Math::Vector2<double>{0.0, 0.0}
                    : HoldPositionDesiredVelocity(*transform, m_autopilotAnchor, m_flightParams);

    return FlyToVelocity(*transform, desiredVel, m_flightParams);
}

} // Gravitaris
