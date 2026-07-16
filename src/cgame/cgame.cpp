#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <optional>

#include <gravitaris/game/logging.hpp>

#include <gravitaris/game/resource/common/resource-loader.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/system/ship-controls-system.hpp>

#include <gravitaris/cgame/spawner/centity-spawner.hpp>
#include <gravitaris/cgame/cgame.hpp>

namespace Gravitaris {

CGame::CGame(IFilesystem &filesystem)
    : Game(filesystem, CreateEntitySpawner())
    , m_simpleModelRenderer(m_registry, filesystem, m_resourceLoader)
    , m_modelRenderer2(m_registry, filesystem, m_resourceLoader)
    , m_starfieldRenderer(filesystem)
    , m_audioSystem(m_registry, m_resourceLoader)
{
    m_camera.SetZoom(Defaults::cameraZoom);
    m_modelRenderer2.SetReferenceZoom(Defaults::cameraZoom);
}

void CGame::NudgeManualZoom(float notches)
{
    // Start the manual override from wherever the camera currently sits, so
    // the first scroll doesn't jump.
    if (!m_manualZoomActive) {
        m_manualZoom = m_cameraZoom;
    }
    m_manualZoom = std::clamp(m_manualZoom * std::pow(1.15f, notches),
                              Camera::MIN_ZOOM, Camera::MAX_ZOOM);
    m_manualZoomActive = true;
    m_manualZoomGraceRemaining = m_cameraParams.manualHold;
}

std::optional<Magnum::Vector2> CGame::FindNearestEnemy(const Magnum::Vector2& from, TeamId playerTeam)
{
    const float radiusSq = m_cameraParams.enemyRadius * m_cameraParams.enemyRadius;

    std::optional<Magnum::Vector2> nearest;
    float nearestSq = radiusSq;

    // Enemy = damageable (a ship, not a bullet) on a real opposing team
    // (excludes neutral planets, which have no Team, and None-team shrapnel).
    m_registry.each([&](flecs::entity, const Transform& t, const Team& team, const Damageable&) {
        if (team.id == playerTeam || team.id == TeamId::None) return;

        const Magnum::Vector2 pos{static_cast<float>(t.pos.x()), static_cast<float>(t.pos.y())};
        const float distSq = (pos - from).dot();
        if (distSq < nearestSq) {
            nearestSq = distSq;
            nearest = pos;
        }
    });

    return nearest;
}

void CGame::UpdateCamera(float dtSeconds)
{
    if (!m_cameraFollow) return;

    const std::optional<flecs::entity> player = GetPlayer();
    if (!player) return;

    const Transform* transform = player->try_get<Transform>();
    if (!transform) return;

    const Magnum::Vector2 playerPos{static_cast<float>(transform->pos.x()),
                                    static_cast<float>(transform->pos.y())};
    const Magnum::Vector2 playerVel{static_cast<float>(transform->vel.x()),
                                    static_cast<float>(transform->vel.y())};
    const float speed = playerVel.length();

    const Team* playerTeamComp = player->try_get<Team>();
    const TeamId playerTeam = playerTeamComp ? playerTeamComp->id : TeamId::Blue;
    const std::optional<Magnum::Vector2> enemy =
            m_cameraParams.enemyFraming ? FindNearestEnemy(playerPos, playerTeam) : std::nullopt;

    // Cancel a manual zoom override once the player actively flies the ship
    // (thrust/rotate), past the post-nudge grace period -- see field comment.
    if (m_manualZoomActive) {
        if (m_manualZoomGraceRemaining > 0.f) {
            m_manualZoomGraceRemaining -= dtSeconds;
        } else {
            const Controls* controls = player->try_get<Controls>();
            const bool flying = controls && (controls->actionFlags.thrustForward ||
                                              controls->actionFlags.rotateLeft ||
                                              controls->actionFlags.rotateRight);
            if (flying) m_manualZoomActive = false;
        }
    }

    // --- Position target: bias toward the enemy when framing, and shrink the
    //     dead zone so the pair is actually tracked rather than drifting. ---
    Magnum::Vector2 posTarget = playerPos;
    float deadZoneFraction = DEAD_ZONE_FRACTION;
    if (enemy) {
        posTarget = playerPos + (*enemy - playerPos) * m_cameraParams.framingBias;
        deadZoneFraction = DEAD_ZONE_FRACTION_FRAMING;
    }
    const Magnum::Vector2 halfExtent = m_viewportSize / (2.f * m_cameraZoom);
    m_camera.FollowWithDeadZone(posTarget, halfExtent * deadZoneFraction);

    // --- Zoom target. ---
    float zoomTarget;
    if (m_manualZoomActive) {
        // Wheel override: hold exactly at the user's zoom until they fly.
        zoomTarget = m_manualZoom;
    } else if (m_cameraParams.dynamicZoom) {
        // Faster -> zoomed out. Monotone, smooth, bounded.
        zoomTarget = m_cameraParams.maxZoom / (1.f + speed / m_cameraParams.speedFalloff);

        // Zoom out further if needed to fit the enemy alongside the player.
        if (enemy) {
            const float span = (*enemy - playerPos).length() + 2.f * m_cameraParams.framingMargin;
            const float fitZoom = std::min(m_viewportSize.x(), m_viewportSize.y()) / std::max(span, 1.f);
            zoomTarget = std::min(zoomTarget, fitZoom);
        }
        zoomTarget = std::clamp(zoomTarget, m_cameraParams.minZoom, m_cameraParams.maxZoom);
    } else {
        zoomTarget = m_cameraParams.maxZoom;
    }

    // Exponential smoothing toward the target: frame-rate independent, and it
    // gives the free "interpolate back" when the manual override expires.
    const float alpha = 1.f - std::exp(-dtSeconds / std::max(m_cameraParams.zoomTau, 1e-3f));
    m_cameraZoom += (zoomTarget - m_cameraZoom) * alpha;
    m_camera.SetZoom(m_cameraZoom);
}

void CGame::Render(double delta)
{
    // Real wall-clock dt for the camera director (Render's `delta` is a fixed-
    // step interpolation fraction, not seconds). Clamped so a stall doesn't
    // snap the camera.
    const auto now = std::chrono::steady_clock::now();
    float dtSeconds = 1.f / 60.f;
    if (m_cameraTimeValid) {
        dtSeconds = std::chrono::duration<float>(now - m_lastCameraTime).count();
        dtSeconds = std::clamp(dtSeconds, 0.f, 0.1f);
    }
    m_lastCameraTime = now;
    m_cameraTimeValid = true;

    UpdateCamera(dtSeconds);

    m_simpleModelRenderer.SetZoom(m_camera.GetZoom());
    m_simpleModelRenderer.SetCameraPosition(m_camera.GetPosition());
    m_modelRenderer2.SetZoom(m_camera.GetZoom());
    m_modelRenderer2.SetCameraPosition(m_camera.GetPosition());
    m_modelRenderer2.SetLineWidth(m_lineWidthPixels);
    m_modelRenderer2.SetZoomWidthFactor(m_zoomWidthFactor);

    m_starfieldRenderer.SetZoom(m_camera.GetZoom());
    m_starfieldRenderer.SetCameraPosition(m_camera.GetPosition());

    {
        ScopedPerfTimer timer(m_perfMonitor, "Starfield");
        m_starfieldRenderer.Render();
    }

    {
        ScopedPerfTimer timer(m_perfMonitor, "Rendering");
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

    {
        ScopedPerfTimer timer(m_perfMonitor, "Audio");
        m_audioSystem.Update(m_camera.GetPosition());
    }
}

std::unique_ptr<EntitySpawner> CGame::CreateEntitySpawner()
{
    return std::make_unique<CEntitySpawner>(m_registry, m_resourceLoader);
}

void CGame::SpawnRandomAIShip()
{
    static constexpr AIPersonalityPreset PRESETS[] = {
            AIPersonalityPreset::Balanced, AIPersonalityPreset::Aggressive, AIPersonalityPreset::Cautious,
            AIPersonalityPreset::Sniper, AIPersonalityPreset::Reckless,
    };

    Vector2d pos{300.0, 200.0};
    const std::optional<flecs::entity> player = GetPlayer();
    const Transform* transform = player ? player->try_get<Transform>() : nullptr;
    if (transform) {
        pos = transform->pos + Vector2d{250.0, 150.0};
    }

    const AIPersonalityPreset preset = PRESETS[std::rand() % std::size(PRESETS)];
    GetEntitySpawner().SpawnAIShip("models/ships/fighter-1"_id, pos, preset);
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
