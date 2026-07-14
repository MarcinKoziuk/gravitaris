#include <optional>

#include <gravitaris/game/logging.hpp>

#include <gravitaris/game/resource/common/resource-loader.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/system/ship-controls-system.hpp>

#include <gravitaris/cgame/spawner/centity-spawner.hpp>
#include <gravitaris/cgame/cgame.hpp>

namespace Gravitaris {

CGame::CGame(IFilesystem &filesystem)
    : Game(filesystem, CreateEntitySpawner())
    , m_simpleModelRenderer(m_registry, filesystem, m_resourceLoader)
    , m_modelRenderer2(m_registry, filesystem, m_resourceLoader)
    , m_audioSystem(m_registry, filesystem)
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

    m_audioSystem.Update(m_camera.GetPosition());
}

std::unique_ptr<EntitySpawner> CGame::CreateEntitySpawner()
{
    return std::make_unique<CEntitySpawner>(m_registry, m_resourceLoader);
}

std::optional<CGame::GravitySource> CGame::FindHeaviestGravitySource()
{
    const std::optional<flecs::entity> player = GetPlayer();

    std::optional<GravitySource> best;
    m_registry.each([&](flecs::entity ent, Transform& transf, PhysicsRef& ref) {
        if ((player && ent == *player) || ent.has<Bullet>()) return;
        const double mass = cpBodyGetMass(m_physicsSystem.GetBody(ref).cp.body.get());
        if (!best || mass > best->mass) {
            best = GravitySource{transf.pos, mass};
        }
    });
    return best;
}

void CGame::SetAutopilotMode(AutopilotMode mode)
{
    const std::optional<flecs::entity> player = GetPlayer();
    const Transform* transform = player ? player->try_get<Transform>() : nullptr;

    if (mode != AutopilotMode::Off) {
        if (!transform) return;
        const PhysicsRef* ref = player->try_get<PhysicsRef>();
        if (ref) {
            const double mass = cpBodyGetMass(m_physicsSystem.GetBody(*ref).cp.body.get());
            m_guidanceParams.accel = ShipControlsSystem::THRUST_FORCE / mass;
        }
    }

    if (mode == AutopilotMode::HoldPosition) {
        m_autopilotAnchor = transform->pos;
    }

    if (mode == AutopilotMode::Orbit) {
        const std::optional<GravitySource> source = FindHeaviestGravitySource();
        if (!source) return;
        m_orbitCenter = source->pos;
        m_orbitMass = source->mass;

        const Magnum::Math::Vector2<double> r = transform->pos - m_orbitCenter;
        m_orbitRadius = r.length();

        // Keep the current sense of rotation; default counter-clockwise.
        const double cross = r.x() * transform->vel.y() - r.y() * transform->vel.x();
        m_orbitDirection = (cross < 0.0) ? -1.0 : 1.0;
    }

    m_autopilotMode = mode;
}

std::optional<ControlFlags> CGame::ComputeAutopilotControls()
{
    if (m_autopilotMode == AutopilotMode::Off) return std::nullopt;

    const std::optional<flecs::entity> player = GetPlayer();
    const Transform* transform = player ? player->try_get<Transform>() : nullptr;
    if (!transform) return std::nullopt;

    Magnum::Math::Vector2<double> desiredVel{0.0, 0.0};
    switch (m_autopilotMode) {
        case AutopilotMode::KillVelocity:
            break;
        case AutopilotMode::HoldPosition:
            desiredVel = HoldPositionDesiredVelocity(*transform, m_autopilotAnchor, m_flightParams);
            break;
        case AutopilotMode::GotoPoint:
            desiredVel = GotoPoint(*transform, m_gotoTarget, m_guidanceParams);
            break;
        case AutopilotMode::Orbit:
            desiredVel = OrbitBody(*transform, m_orbitCenter, m_orbitMass,
                                   m_orbitRadius, m_orbitDirection, m_guidanceParams);
            break;
        case AutopilotMode::Off:
            return std::nullopt;
    }

    return FlyToVelocity(*transform, desiredVel, m_flightParams);
}

} // Gravitaris
