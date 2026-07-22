#include <algorithm>
#include <cmath>
#include <limits>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/planet.hpp>

#include <gravitaris/cgame/camera-director.hpp>

namespace Gravitaris {

    // Claude: move to a new camera/ folder
    // Claude: i've told you/documented I prefer static over anon. namespace
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

CameraDirector::CameraDirector(flecs::world& registry, float initialZoom)
        : m_registry(registry)
        , m_cameraZoom(initialZoom)
        , m_manualZoom(initialZoom)
{
    m_camera.SetZoom(initialZoom);
}

bool CameraDirector::SameEntity(const flecs::entity& a, const flecs::entity& b)
{
    // flecs::entity's implicit operator id_t() only exposes the raw 64-bit
    // id -- two different flecs::world instances assign ids independently,
    // so raw == alone can spuriously match an m_registry entity against an
    // unrelated m_remoteWorld one that happens to share a numeric id. Compare
    // the owning world too.
    return a == b && a.world().c_ptr() == b.world().c_ptr();
}

void CameraDirector::NudgeManualZoom(float notches)
{
    // Start the manual override from wherever the camera currently sits, so
    // the first scroll doesn't jump.
    if (!m_manualZoomActive) {
        m_manualZoom = m_cameraZoom;
    }
    m_manualZoom = std::clamp(m_manualZoom * std::pow(m_params.scrollSensitivity, notches),
                              Camera::MIN_ZOOM, Camera::MAX_ZOOM);
    m_manualZoomActive = true;
    m_manualZoomGraceRemaining = m_params.manualHold;
}

std::optional<Magnum::Vector2> CameraDirector::SelectFramedEnemy(const Magnum::Vector2& from, TeamId playerTeam,
                                                                  float& outCoverDist)
{
    const float radiusSq = m_params.enemyRadius * m_params.enemyRadius;

    flecs::entity nearest{};
    Magnum::Vector2 nearestPos{};
    float nearestSq = radiusSq;

    std::optional<Magnum::Vector2> currentPos;
    float currentSq = 0.f;

    // Farthest in-range enemy, so the zoom-fit can hold the whole group.
    float maxInRangeSq = 0.f;

    // Enemy = damageable (a ship, not a bullet) on a real opposing team
    // (excludes neutral planets, which have no Team, and None-team shrapnel).
    // Swept across m_registry and, in multiplayer, m_remoteWorld too -- every
    // ship other than the local player's own predicted one lives there (see
    // m_remoteWorld's field comment), so this one lambda finds enemies
    // regardless of which world they're actually simulated/mirrored in.
    const auto considerShip = [&](flecs::entity entity, const Transform& t, const Team& team, const Damageable&) {
        if (team.id == playerTeam || team.id == TeamId::None) return;

        const Magnum::Vector2 pos{static_cast<float>(t.pos.x()), static_cast<float>(t.pos.y())};
        const float distSq = (pos - from).dot();

        if (SameEntity(entity, m_framedEnemy)) {
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
    };
    m_registry.each(considerShip);
    if (m_remoteWorld) m_remoteWorld->each(considerShip);

    // Sticky selection: keep the current target while it's alive and inside
    // the (slightly enlarged, exit-hysteresis) radius, unless the nearest
    // rival is decisively closer -- see FRAMING_SWITCH_FACTOR's comment.
    std::optional<Magnum::Vector2> framedPos;
    float framedSq = 0.f;
    if (m_framedEnemy.is_valid() && m_framedEnemy.is_alive() && currentPos) {
        const float exitRadius = m_params.enemyRadius * FRAMING_EXIT_RADIUS_FACTOR;
        const bool currentInRange = currentSq <= exitRadius * exitRadius;
        const bool rivalDecisivelyCloser =
                nearest && !SameEntity(nearest, m_framedEnemy) &&
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

void CameraDirector::Update(std::optional<flecs::entity> player, const Magnum::Vector2& viewportSize,
                            float dtSeconds, flecs::world* remoteWorld,
                            std::optional<Magnum::Vector2> positionOverride)
{
    if (!m_cameraFollow) return;
    if (!player) return;

    // Valid only for the duration of this call (see field comment) --
    // SelectFramedEnemy and the planet sweep below both read it.
    m_remoteWorld = remoteWorld;

    const Transform* transform = player->try_get<Transform>();
    if (!transform) return;

    // See positionOverride's own doc comment: multiplayer substitutes an
    // already-smoothed position here instead of the real (possibly just
    // reconciliation-snapped) Transform.
    const Magnum::Vector2 playerPos = positionOverride.value_or(Magnum::Vector2{
            static_cast<float>(transform->pos.x()), static_cast<float>(transform->pos.y())});
    const Magnum::Vector2 playerVel{static_cast<float>(transform->vel.x()),
                                    static_cast<float>(transform->vel.y())};
    const float speed = playerVel.length();

    const Team* playerTeamComp = player->try_get<Team>();
    const TeamId playerTeam = playerTeamComp ? playerTeamComp->id : TeamId::Blue;
    float coverDist = 0.f;
    const std::optional<Magnum::Vector2> enemy =
            m_params.enemyFraming ? SelectFramedEnemy(playerPos, playerTeam, coverDist) : std::nullopt;
    const float framedDist = enemy ? (*enemy - playerPos).length() : 0.f;

    // Nearest planet/sun surface distance, for the zoom-out-to-see-it band
    // below. True world radius (not a minimap floor): a sun should need more
    // clearance than a small planet.
    float nearestSurfaceDist = std::numeric_limits<float>::max();
    float nearestPlanetRadius = 0.f;
    if (m_params.planetFraming) {
        // Radius comes straight off the replicated Planet component now (see
        // its own doc comment) -- no PhysicsSystem/PhysicsRef needed, so this
        // sweeps m_remoteWorld exactly like the enemy search above.
        const auto considerPlanet = [&](flecs::entity, const Transform& t, const Planet& planet) {
            const float radius = planet.radius * static_cast<float>(t.scale.x());
            const Magnum::Vector2 pos{static_cast<float>(t.pos.x()), static_cast<float>(t.pos.y())};
            const float surfaceDist = (pos - playerPos).length() - radius;
            if (surfaceDist < nearestSurfaceDist) {
                nearestSurfaceDist = surfaceDist;
                nearestPlanetRadius = radius;
            }
        };
        m_registry.each(considerPlanet);
        if (m_remoteWorld) m_remoteWorld->each(considerPlanet);
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
    const float framingAlpha = 1.f - std::exp(-dtSeconds / std::max(m_params.framingTau, 1e-3f));
    const float framingGoal = enemy ? 1.f : 0.f;
    m_framingAmount += (framingGoal - m_framingAmount) * framingAlpha;

    // Same easing for the planet zoom-out band and the close-combat zoom-in,
    // so both blend in/out as smoothly as enemy framing does.
    const float planetFramingGoal = m_params.planetFraming
            ? PlanetFramingGoal(nearestSurfaceDist, m_params.planetReleaseDist, m_params.planetFramingRange)
            : 0.f;
    m_planetFramingAmount += (planetFramingGoal - m_planetFramingAmount) * framingAlpha;

    const float closeZoomGoal = enemy
            ? std::clamp(1.f - framedDist / std::max(m_params.closeZoomRange, 1.f), 0.f, 1.f)
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
    const Magnum::Vector2 posTarget = playerPos + enemyOffset * (m_params.framingBias * m_framingAmount);
    const float deadZoneFraction =
            DEAD_ZONE_FRACTION + (DEAD_ZONE_FRACTION_FRAMING - DEAD_ZONE_FRACTION) * m_framingAmount;
    const Magnum::Vector2 halfExtent = viewportSize / (2.f * m_cameraZoom);
    m_camera.FollowWithDeadZone(posTarget, halfExtent * deadZoneFraction);

    // --- Zoom target. ---
    float zoomTarget;
    if (m_manualZoomActive) {
        // Wheel override: hold exactly at the user's zoom until they fly.
        zoomTarget = m_manualZoom;
    } else if (m_params.dynamicZoom) {
        // Faster -> zoomed out. Monotone, smooth, bounded.
        zoomTarget = m_params.maxZoom / (1.f + speed / m_params.speedFalloff);

        // Zoom out further if needed to fit every nearby enemy alongside the
        // player (m_framedReach covers the farthest in-range one), fading the
        // requirement in/out with the same framing amount so it doesn't yank
        // the zoom target before the pan has caught up. The minZoom clamp below
        // still caps how far out this can go.
        if (m_framingAmount > 0.f) {
            const float span = m_framedReach + 2.f * m_params.framingMargin;
            const float fitZoom = std::min(viewportSize.x(), viewportSize.y()) / std::max(span, 1.f);
            zoomTarget += (std::min(zoomTarget, fitZoom) - zoomTarget) * m_framingAmount;
        }

        // Zoom out further to fit a nearby planet/sun while approaching it
        // (see m_planetFramingAmount/PlanetFramingGoal) -- only ever pulls the
        // target wider, same min() pattern as the enemy fit above.
        if (m_planetFramingAmount > 0.f) {
            const float span = 2.f * nearestPlanetRadius + 2.f * m_params.planetFramingMargin;
            const float fitZoom = std::min(viewportSize.x(), viewportSize.y()) / std::max(span, 1.f);
            zoomTarget += (std::min(zoomTarget, fitZoom) - zoomTarget) * m_planetFramingAmount;
        }

        // Close-combat zoom-in: pulls toward closeZoomFraction of maxZoom as
        // the framed enemy nears point-blank range -- readable clash, not a
        // full snap-in (closeZoomFraction < 1). Only ever tightens (max()),
        // so it can't undo the planet/enemy zoom-out above unless it's
        // actually closer-in than what they already want.
        if (m_closeZoomAmount > 0.f) {
            const float closeZoomTarget = m_params.maxZoom * m_params.closeZoomFraction;
            zoomTarget += (std::max(zoomTarget, closeZoomTarget) - zoomTarget) * m_closeZoomAmount;
        }

        zoomTarget = std::clamp(zoomTarget, m_params.minZoom, m_params.maxZoom);
    } else {
        zoomTarget = m_params.maxZoom;
    }

    // Exponential smoothing toward the target: frame-rate independent, and it
    // gives the free "interpolate back" when the manual override expires. A
    // wheel-driven target uses its own (snappier) tau so scrolling reads
    // immediately without changing how smooth the dynamic zoom (speed/enemy
    // framing) feels.
    const float tau = m_manualZoomActive ? m_params.manualZoomTau : m_params.zoomTau;
    const float alpha = 1.f - std::exp(-dtSeconds / std::max(tau, 1e-3f));
    m_cameraZoom += (zoomTarget - m_cameraZoom) * alpha;
    m_camera.SetZoom(m_cameraZoom);
}

} // namespace Gravitaris
