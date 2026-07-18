#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
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
#include <gravitaris/game/component/planet.hpp>
#include <gravitaris/game/util/splitmix.hpp>
#include <gravitaris/game/resource/body.hpp>
#include <gravitaris/game/system/physics-system.hpp>
#include <gravitaris/game/system/ship-controls-system.hpp>

#include <gravitaris/cgame/spawner/centity-spawner.hpp>
#include <gravitaris/cgame/component/hit-flash.hpp>
#include <gravitaris/cgame/team-color.hpp>
#include <gravitaris/cgame/cgame.hpp>

namespace Gravitaris {

namespace {

// 0..1 envelope for the planet zoom-out: fades in as the surface distance
// drops from framingRange to a plateau, holds through the plateau, then fades
// back out approaching releaseDist so the final approach hands back to the
// speed-driven zoom-in (a slow landing then reads as "zooming in", not stuck
// zoomed out).
float PlanetFramingGoal(float surfaceDist, float releaseDist, float framingRange)
{
    if (surfaceDist <= 0.f || surfaceDist >= framingRange) return 0.f;

    const float plateauInner = releaseDist * 2.5f;
    const float plateauOuter = framingRange * 0.5f;

    if (surfaceDist >= plateauOuter) {
        return 1.f - (surfaceDist - plateauOuter) / std::max(framingRange - plateauOuter, 1.f);
    }
    if (surfaceDist >= plateauInner) return 1.f;
    if (surfaceDist >= releaseDist) {
        return (surfaceDist - releaseDist) / std::max(plateauInner - releaseDist, 1.f);
    }
    return 0.f;
}

} // namespace

CGame::CGame(IFilesystem &filesystem)
    : Game(filesystem, CreateEntitySpawner())
    , m_simpleModelRenderer(m_registry, filesystem, m_resourceLoader)
    , m_modelRenderer2(m_registry, filesystem, m_resourceLoader)
    , m_starfieldRenderer(filesystem)
    , m_minimapRenderer(m_registry, m_physicsSystem, filesystem)
    , m_audioSystem(m_registry, m_resourceLoader, m_eventQueue)
{
    m_camera.SetZoom(Defaults::cameraZoom);
    m_modelRenderer2.SetReferenceZoom(Defaults::cameraZoom);

    // Loading it is what bakes it into m_modelRenderer2 (via OnCreate<Model>);
    // the ResourcePtr member then keeps it baked -- see m_arrowModel.
    m_arrowModel = m_resourceLoader.Load<Model>("models/ui/arrow-1"_id);
}

void CGame::NudgeManualZoom(float notches)
{
    // Start the manual override from wherever the camera currently sits, so
    // the first scroll doesn't jump.
    if (!m_manualZoomActive) {
        m_manualZoom = m_cameraZoom;
    }
    m_manualZoom = std::clamp(m_manualZoom * std::pow(m_cameraParams.scrollSensitivity, notches),
                              Camera::MIN_ZOOM, Camera::MAX_ZOOM);
    m_manualZoomActive = true;
    m_manualZoomGraceRemaining = m_cameraParams.manualHold;
}

std::optional<Magnum::Vector2> CGame::SelectFramedEnemy(const Magnum::Vector2& from, TeamId playerTeam,
                                                        float& outCoverDist)
{
    const float radiusSq = m_cameraParams.enemyRadius * m_cameraParams.enemyRadius;

    flecs::entity nearest{};
    Magnum::Vector2 nearestPos{};
    float nearestSq = radiusSq;

    std::optional<Magnum::Vector2> currentPos;
    float currentSq = 0.f;

    // Farthest in-range enemy, so the zoom-fit can hold the whole group.
    float maxInRangeSq = 0.f;

    // Enemy = damageable (a ship, not a bullet) on a real opposing team
    // (excludes neutral planets, which have no Team, and None-team shrapnel).
    m_registry.each([&](flecs::entity entity, const Transform& t, const Team& team, const Damageable&) {
        if (team.id == playerTeam || team.id == TeamId::None) return;

        const Magnum::Vector2 pos{static_cast<float>(t.pos.x()), static_cast<float>(t.pos.y())};
        const float distSq = (pos - from).dot();

        if (entity == m_framedEnemy) {
            currentPos = pos;
            currentSq = distSq;
        }
        if (distSq < radiusSq && distSq > maxInRangeSq) {
            maxInRangeSq = distSq;
        }
        if (distSq < nearestSq) {
            nearestSq = distSq;
            nearestPos = pos;
            nearest = entity;
        }
    });

    // Sticky selection: keep the current target while it's alive and inside
    // the (slightly enlarged, exit-hysteresis) radius, unless the nearest
    // rival is decisively closer -- see FRAMING_SWITCH_FACTOR's comment.
    std::optional<Magnum::Vector2> framedPos;
    float framedSq = 0.f;
    if (m_framedEnemy.is_valid() && m_framedEnemy.is_alive() && currentPos) {
        const float exitRadius = m_cameraParams.enemyRadius * FRAMING_EXIT_RADIUS_FACTOR;
        const bool currentInRange = currentSq <= exitRadius * exitRadius;
        const bool rivalDecisivelyCloser =
                nearest && nearest != m_framedEnemy &&
                nearestSq < currentSq * (FRAMING_SWITCH_FACTOR * FRAMING_SWITCH_FACTOR);

        if (currentInRange && !rivalDecisivelyCloser) {
            framedPos = currentPos;
            framedSq = currentSq;
        }
    }
    if (!framedPos) {
        m_framedEnemy = nearest;
        if (nearest) {
            framedPos = nearestPos;
            framedSq = nearestSq;
        }
    }

    // Cover the farthest in-range enemy and the framed one (which may be held
    // slightly beyond the radius by the exit hysteresis).
    outCoverDist = std::sqrt(std::max(maxInRangeSq, framedPos ? framedSq : 0.f));
    return framedPos;
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
    float coverDist = 0.f;
    const std::optional<Magnum::Vector2> enemy =
            m_cameraParams.enemyFraming ? SelectFramedEnemy(playerPos, playerTeam, coverDist) : std::nullopt;
    const float framedDist = enemy ? (*enemy - playerPos).length() : 0.f;

    // Nearest planet/sun surface distance, for the zoom-out-to-see-it band
    // below. True world radius (not a minimap floor): a sun should need more
    // clearance than a small planet.
    float nearestSurfaceDist = std::numeric_limits<float>::max();
    float nearestPlanetRadius = 0.f;
    if (m_cameraParams.planetFraming) {
        m_registry.each([&](flecs::entity entity, const Transform& t, const PhysicsRef& ref) {
            if (!entity.has<Planet>()) return;
            float radius = 0.f;
            const PhysicsBody& body = m_physicsSystem.GetBody(ref);
            if (body.body && !body.body->GetCircleShapes().empty()) {
                radius = static_cast<float>(body.body->GetCircleShapes().front().radius)
                        * static_cast<float>(t.scale.x());
            }
            const Magnum::Vector2 pos{static_cast<float>(t.pos.x()), static_cast<float>(t.pos.y())};
            const float surfaceDist = (pos - playerPos).length() - radius;
            if (surfaceDist < nearestSurfaceDist) {
                nearestSurfaceDist = surfaceDist;
                nearestPlanetRadius = radius;
            }
        });
    }

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

    // Ease the framing amount in/out instead of snapping it the instant an
    // enemy enters/leaves radius -- see m_framingAmount's field comment.
    const float framingAlpha = 1.f - std::exp(-dtSeconds / std::max(m_cameraParams.framingTau, 1e-3f));
    const float framingGoal = enemy ? 1.f : 0.f;
    m_framingAmount += (framingGoal - m_framingAmount) * framingAlpha;

    // Same easing for the planet zoom-out band and the close-combat zoom-in,
    // so both blend in/out as smoothly as enemy framing does.
    const float planetFramingGoal = m_cameraParams.planetFraming
            ? PlanetFramingGoal(nearestSurfaceDist, m_cameraParams.planetReleaseDist, m_cameraParams.planetFramingRange)
            : 0.f;
    m_planetFramingAmount += (planetFramingGoal - m_planetFramingAmount) * framingAlpha;

    const float closeZoomGoal = enemy
            ? std::clamp(1.f - framedDist / std::max(m_cameraParams.closeZoomRange, 1.f), 0.f, 1.f)
            : 0.f;
    m_closeZoomAmount += (closeZoomGoal - m_closeZoomAmount) * framingAlpha;

    // Ease the offset vector too: a framed-target switch (or the enemy's own
    // motion) then glides rather than stepping. While framing is still nearly
    // disengaged, snap instead -- the bias is invisible at ~0 and this avoids
    // sweeping in from a stale offset left by an earlier fight.
    if (enemy) {
        const Magnum::Vector2 targetOffset = *enemy - playerPos;
        if (m_framingAmount < 0.05f) {
            m_framedEnemyOffset = targetOffset;
            m_framedReach = coverDist;
        } else {
            m_framedEnemyOffset += (targetOffset - m_framedEnemyOffset) * framingAlpha;
            m_framedReach += (coverDist - m_framedReach) * framingAlpha;
        }
    }
    const Magnum::Vector2& enemyOffset = m_framedEnemyOffset;

    // --- Position target: bias toward the enemy when framing, and shrink the
    //     dead zone so the pair is actually tracked rather than drifting. ---
    const Magnum::Vector2 posTarget = playerPos + enemyOffset * (m_cameraParams.framingBias * m_framingAmount);
    const float deadZoneFraction =
            DEAD_ZONE_FRACTION + (DEAD_ZONE_FRACTION_FRAMING - DEAD_ZONE_FRACTION) * m_framingAmount;
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

        // Zoom out further if needed to fit every nearby enemy alongside the
        // player (m_framedReach covers the farthest in-range one), fading the
        // requirement in/out with the same framing amount so it doesn't yank
        // the zoom target before the pan has caught up. The minZoom clamp below
        // still caps how far out this can go.
        if (m_framingAmount > 0.f) {
            const float span = m_framedReach + 2.f * m_cameraParams.framingMargin;
            const float fitZoom = std::min(m_viewportSize.x(), m_viewportSize.y()) / std::max(span, 1.f);
            zoomTarget += (std::min(zoomTarget, fitZoom) - zoomTarget) * m_framingAmount;
        }

        // Zoom out further to fit a nearby planet/sun while approaching it
        // (see m_planetFramingAmount/PlanetFramingGoal) -- only ever pulls the
        // target wider, same min() pattern as the enemy fit above.
        if (m_planetFramingAmount > 0.f) {
            const float span = 2.f * nearestPlanetRadius + 2.f * m_cameraParams.planetFramingMargin;
            const float fitZoom = std::min(m_viewportSize.x(), m_viewportSize.y()) / std::max(span, 1.f);
            zoomTarget += (std::min(zoomTarget, fitZoom) - zoomTarget) * m_planetFramingAmount;
        }

        // Close-combat zoom-in: pulls toward closeZoomFraction of maxZoom as
        // the framed enemy nears point-blank range -- readable clash, not a
        // full snap-in (closeZoomFraction < 1). Only ever tightens (max()),
        // so it can't undo the planet/enemy zoom-out above unless it's
        // actually closer-in than what they already want.
        if (m_closeZoomAmount > 0.f) {
            const float closeZoomTarget = m_cameraParams.maxZoom * m_cameraParams.closeZoomFraction;
            zoomTarget += (std::max(zoomTarget, closeZoomTarget) - zoomTarget) * m_closeZoomAmount;
        }

        zoomTarget = std::clamp(zoomTarget, m_cameraParams.minZoom, m_cameraParams.maxZoom);
    } else {
        zoomTarget = m_cameraParams.maxZoom;
    }

    // Exponential smoothing toward the target: frame-rate independent, and it
    // gives the free "interpolate back" when the manual override expires. A
    // wheel-driven target uses its own (snappier) tau so scrolling reads
    // immediately without changing how smooth the dynamic zoom (speed/enemy
    // framing) feels.
    const float tau = m_manualZoomActive ? m_cameraParams.manualZoomTau : m_cameraParams.zoomTau;
    const float alpha = 1.f - std::exp(-dtSeconds / std::max(tau, 1e-3f));
    m_cameraZoom += (zoomTarget - m_cameraZoom) * alpha;
    m_camera.SetZoom(m_cameraZoom);
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
    const Magnum::Vector2 cameraPos = m_camera.GetPosition();
    const float zoom = m_camera.GetZoom();
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

    const Magnum::Vector2 playerPos{static_cast<float>(transform->pos.x()),
                                    static_cast<float>(transform->pos.y())};
    const Magnum::Vector2 viewHalfExtent = m_viewportSize / (2.f * std::max(m_cameraZoom, 1e-3f));

    // Static, not player-centered: the solar system is laid out symmetrically
    // around the origin (see Game::Start), so that's the whole map's center.
    m_minimapRenderer.Render(Magnum::Vector2{0.f, 0.f}, playerPos, m_camera.GetPosition(), viewHalfExtent);
}

void CGame::UpdateHitFlashes(float dtSeconds)
{
    // The flash previously decayed 1/8 per 60Hz tick inside DamageSystem;
    // 7.5/s is that same rate, now applied client-side with frame dt.
    constexpr float FLASH_DECAY_PER_SECOND = 7.5f;

    m_flashEventCursor = m_eventQueue.ConsumeSince(m_flashEventCursor, [&](const GameEvent& event) {
        if (event.type != GameEventType::Impact && event.type != GameEventType::LandingCrash) return;
        const flecs::entity entity = m_entitySpawner->EntityForNetId(event.sourceNetId);
        if (!entity.is_alive()) return; // e.g. the hit killed it this tick
        if (HitFlash* flash = entity.try_get_mut<HitFlash>()) {
            flash->amount = 1.f;
        }
    });

    m_registry.each([&](HitFlash& flash) {
        if (flash.amount > 0.f) {
            flash.amount = std::max(0.f, flash.amount - FLASH_DECAY_PER_SECOND * dtSeconds);
        }
    });
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
    UpdateHitFlashes(dtSeconds);

    // Debug/tuning only: reapplies every frame (cheap, one cpBodySetMass
    // call) so it stays in effect across a respawn's fresh body without
    // extra bookkeeping -- see m_shipWeightMultiplier's field comment.
    if (const std::optional<flecs::entity> player = GetPlayer()) {
        if (const PhysicsRef* ref = player->try_get<PhysicsRef>()) {
            m_physicsSystem.SetMassMultiplier(*ref, m_shipWeightMultiplier);
        }
    }

    m_simpleModelRenderer.SetZoom(m_camera.GetZoom());
    m_simpleModelRenderer.SetCameraPosition(m_camera.GetPosition());
    m_modelRenderer2.SetZoom(m_camera.GetZoom());
    m_modelRenderer2.SetCameraPosition(m_camera.GetPosition());
    m_modelRenderer2.SetLineWidth(m_lineWidthPixels);
    m_modelRenderer2.SetZoomWidthFactor(m_zoomWidthFactor);

    m_starfieldRenderer.SetZoom(m_camera.GetZoom());
    m_starfieldRenderer.SetCameraPosition(m_camera.GetPosition());

    // Overlays ride the model renderer's instanced draw, so they must be
    // submitted before it runs (and after UpdateCamera settles the view).
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

    std::uint64_t rng = SplitMix64Seed(GetStep(), m_randomAIShipSpawnCount++);
    const AIPersonalityPreset preset = PRESETS[SplitMix64Next(rng) % std::size(PRESETS)];
    GetEntitySpawner().SpawnAIShip("models/ships/fighter-1"_id, pos, preset);
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
