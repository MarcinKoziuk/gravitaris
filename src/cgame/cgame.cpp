#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>

#include <gravitaris/game/logging.hpp>

#include <gravitaris/game/resource/common/resource-loader.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/gravity-source.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/system/physics-system.hpp>
#include <gravitaris/game/system/ship-controls-system.hpp>

#include <gravitaris/cgame/spawner/centity-spawner.hpp>
#include <gravitaris/cgame/team-color.hpp>
#include <gravitaris/cgame/cgame.hpp>

namespace Gravitaris {

CGame::CGame(IFilesystem &filesystem)
    : Game(filesystem, CreateEntitySpawner())
    , m_simpleModelRenderer(m_registry, filesystem, m_resourceLoader)
    , m_modelRenderer2(m_registry, filesystem, m_resourceLoader)
    , m_starfieldRenderer(filesystem)
    , m_minimapRenderer(m_registry, m_physicsSystem, filesystem)
    , m_audioSystem(m_registry, m_resourceLoader, m_eventQueue)
    , m_hitFlashSystem(m_registry, m_eventQueue, *m_entitySpawner)
    , m_cameraDirector(m_registry, m_physicsSystem, Defaults::cameraZoom)
{
    m_modelRenderer2.SetReferenceZoom(Defaults::cameraZoom);

    // Loading it is what bakes it into m_modelRenderer2 (via OnCreate<Model>);
    // the ResourcePtr member then keeps it baked -- see m_arrowModel.
    m_arrowModel = m_resourceLoader.Load<Model>("models/ui/arrow-1"_id);

    // This game's tuned default (Game's own default is 1 = unmodified): a
    // lighter ship reads better against the solar system's gravity wells.
    // Headless Games (sim-test) never call this, so their determinism is
    // unaffected by this specific value.
    SetShipWeightMultiplier(0.667f);
}

void CGame::UpdateIndicators()
{
    if (!m_indicatorParams.enabled || !m_arrowModel) return;

    const std::optional<flecs::entity> player = GetPlayer();
    if (!player) return;
    const Transform* playerTransf = player->try_get<Transform>();
    if (!playerTransf) return;

    const Team* playerTeamComp = player->try_get<Team>();
    const TeamId playerTeam = playerTeamComp ? playerTeamComp->id : TeamId::Blue;

    const Magnum::Vector2 playerPos{static_cast<float>(playerTransf->pos.x()),
                                    static_cast<float>(playerTransf->pos.y())};
    const Camera& camera = m_cameraDirector.GetCamera();
    const Magnum::Vector2 cameraPos = camera.GetPosition();
    const float zoom = camera.GetZoom();
    if (zoom <= 0.f) return;

    // The renderers map world->screen at `zoom` px per world unit, camera-
    // centered (see ModelRenderer2::ViewProjection), so screen offsets are just
    // world offsets from the camera scaled by zoom.
    const Magnum::Vector2 halfExtentPx = m_viewportSize * 0.5f;
    const float ringWorld = m_indicatorParams.ringRadiusPx * m_pixelScale / zoom;
    const float arrowWorld = m_indicatorParams.arrowSizePx * m_pixelScale / zoom;

    struct Candidate {
        Magnum::Vector2 pos;
        Magnum::Vector3 color;
        float distance;
    };
    std::vector<Candidate> enemies;

    // Enemy = damageable ship on a real opposing team -- same notion
    // SelectFramedEnemy uses for the camera.
    m_registry.each([&](flecs::entity, const Transform& t, const Team& team, const Damageable&) {
        if (team.id == playerTeam || team.id == TeamId::None) return;
        const Magnum::Vector2 pos{static_cast<float>(t.pos.x()), static_cast<float>(t.pos.y())};
        const float dist = (pos - playerPos).length();
        if (dist > m_indicatorParams.enemyRange) return;
        enemies.push_back({pos, Magnum::Vector3{TeamColor(team.id)}, dist});
    });

    // Nearest-first, then cap: with a crowded field the closest threats are the
    // ones worth the screen space.
    const auto byDistance = [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; };
    std::sort(enemies.begin(), enemies.end(), byDistance);
    enemies.resize(std::min<std::size_t>(enemies.size(), m_indicatorParams.maxEnemies));

    const auto submit = [&](const Candidate& c, float range) {
        const Magnum::Vector2 fromCamera = c.pos - cameraPos;

        // How far outside the view the target is, in px past the (inset) edge.
        // Ramping the arrow in over fadeBandPx instead of switching it on at the
        // boundary keeps a target that's drifting across the edge from popping.
        const Magnum::Vector2 screenPx = fromCamera * zoom;
        const Magnum::Vector2 inset = halfExtentPx - Magnum::Vector2{m_indicatorParams.edgeMarginPx * m_pixelScale};
        const float past = std::max(std::abs(screenPx.x()) - std::max(inset.x(), 1.f),
                                    std::abs(screenPx.y()) - std::max(inset.y(), 1.f));
        if (past <= 0.f) return; // comfortably on screen: the target speaks for itself
        const float edgeFade = std::clamp(past / std::max(m_indicatorParams.fadeBandPx * m_pixelScale, 1.f), 0.f, 1.f);

        // Near targets read loud, distant ones stay legible but recede; also
        // fades an arrow out as its target leaves range, so nothing blinks off.
        const float nearness = std::clamp(1.f - c.distance / std::max(range, 1.f), 0.f, 1.f);
        const float strength = edgeFade * (m_indicatorParams.minStrength
                                           + (1.f - m_indicatorParams.minStrength) * nearness);
        if (strength <= 0.01f) return;

        // Ring center and pointing direction are player-relative, not camera-
        // relative: enemy framing can offset the camera from the player, and
        // the arrows should read as "which way from my ship", staying anchored
        // on the player's screen position rather than the viewport's.
        const Magnum::Vector2 fromPlayer = c.pos - playerPos;
        const float len = fromPlayer.length();
        if (len < 1e-3f) return;
        const Magnum::Vector2 dir = fromPlayer / len;

        // rot 0 points the glyph along -Y (ship convention, see arrow-1.svg), so
        // adding a quarter turn to the direction's angle aims it outward.
        const float rot = std::atan2(dir.y(), dir.x()) + Magnum::Constants::piHalf();

        // Width only fades in/out at the screen edge (edgeFade), never shrinks
        // with distance; height additionally stretches as the target closes
        // in, so proximity reads as "taller", not "bigger" -- the local X/Y
        // scaling is pre-rotation, so this is the arrow's own width/height
        // regardless of which way it's pointing.
        const float widthScale = arrowWorld * edgeFade;
        const float heightNearness = std::clamp(nearness * m_indicatorParams.heightRampFactor, 0.f, 1.f);
        const float heightScale = widthScale * (1.f + (m_indicatorParams.maxHeightFactor - 1.f) * heightNearness);

        const Matrix3 transform = Matrix3::translation(playerPos + dir * ringWorld)
                                * Matrix3::rotation(Magnum::Rad(rot))
                                * Matrix3::scaling({widthScale, heightScale});

        // No alpha in the line shader: on the black backdrop, scaling the color
        // toward black is the fade.
        m_modelRenderer2.SubmitOverlay(m_arrowModel.Id(), transform, c.color * strength);
    };

    for (const Candidate& c : enemies) submit(c, m_indicatorParams.enemyRange);
}

void CGame::RenderMinimap()
{
    const std::optional<flecs::entity> player = GetPlayer();
    const Transform* transform = player ? player->try_get<Transform>() : nullptr;
    if (!transform) return; // between death and respawn: freeze the last frame

    const Camera& camera = m_cameraDirector.GetCamera();
    const Magnum::Vector2 playerPos{static_cast<float>(transform->pos.x()),
                                    static_cast<float>(transform->pos.y())};
    const Magnum::Vector2 viewHalfExtent = m_viewportSize / (2.f * std::max(camera.GetZoom(), 1e-3f));

    // Static, not player-centered: the solar system is laid out symmetrically
    // around the origin (see Game::Start), so that's the whole map's center.
    m_minimapRenderer.Render(Magnum::Vector2{0.f, 0.f}, playerPos, camera.GetPosition(), viewHalfExtent);
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

    m_cameraDirector.Update(GetPlayer(), m_viewportSize, dtSeconds);
    m_hitFlashSystem.Update(dtSeconds);

    const Camera& camera = m_cameraDirector.GetCamera();
    m_simpleModelRenderer.SetZoom(camera.GetZoom());
    m_simpleModelRenderer.SetCameraPosition(camera.GetPosition());
    m_modelRenderer2.SetZoom(camera.GetZoom());
    m_modelRenderer2.SetCameraPosition(camera.GetPosition());
    m_modelRenderer2.SetLineWidth(m_lineWidthPixels);
    m_modelRenderer2.SetZoomWidthFactor(m_zoomWidthFactor);

    m_starfieldRenderer.SetZoom(camera.GetZoom());
    m_starfieldRenderer.SetCameraPosition(camera.GetPosition());

    // Overlays ride the model renderer's instanced draw, so they must be
    // submitted before it runs (and after the camera director settles the view).
    UpdateIndicators();

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
        m_audioSystem.Update(camera.GetPosition());
    }
}

std::unique_ptr<EntitySpawner> CGame::CreateEntitySpawner()
{
    return std::make_unique<CEntitySpawner>(m_registry, m_resourceLoader);
}

std::optional<CGame::GravitySource> CGame::FindHeaviestGravitySource()
{
    // GravitySource.mass, not cpBodyGetMass: celestials are kinematic bodies
    // (infinite Chipmunk mass), so their gravitational mass lives in the
    // component instead (see include/gravitaris/game/component/gravity-source.hpp).
    std::optional<GravitySource> best;
    m_registry.each([&](flecs::entity, const Transform& transf, const Gravitaris::GravitySource& gs) {
        const double mass = gs.mass * gs.multiplier;
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
