#pragma once

#include <algorithm>
#include <optional>

#include <flecs.h>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/fwd.hpp>
#include <gravitaris/game/component/team.hpp>

#include <gravitaris/cgame/scene-view.hpp>
#include <gravitaris/cgame/camera.hpp>

namespace Gravitaris {

// Owns the camera's position/zoom state and the per-frame logic that drives
// it: dead-zone follow, sticky enemy framing, dynamic (speed-driven) zoom,
// wheel-manual zoom override, plus zoom-out-to-fit for a nearby enemy or
// planet/sun and a zoom-in for close combat. Nothing here mutates the sim --
// it only reads Transform/Team/Planet/Damageable and writes its own Camera.
class CameraDirector {
public:
    // Tunables for the camera director (exposed in the Camera debug tab).
    struct CameraParams {
        bool dynamicZoom = true;
        // The speed curve runs from maxZoom at rest down toward cruiseZoom,
        // which it approaches but never crosses. That floor is the speed
        // term's alone, which is what lets minZoom stay wide enough for the
        // planet/enemy fits below without a fast ship shrinking to a speck.
        float minZoom = 0.25f;      // hard clamp -- only the zoom-out-to-fit paths reach this far
        float maxZoom = 2.5f;       // most zoomed-in (at rest) end
        float cruiseZoom = 0.6f;    // asymptote of the speed curve: as small as speed alone makes the ship
        float speedFalloff = 220.f; // world units/sec at which zoom noticeably backs off
        float zoomTau = 2.f;        // zoom smoothing time constant (s), for the dynamic (speed/framing) target
        float manualZoomTau = 0.25f;// zoom smoothing time constant (s), for a wheel-driven target -- snappier
                                    // than zoomTau so a manual scroll reads immediately, without touching
                                    // the dynamic-zoom feel (enemy framing, speed falloff).
        float manualHold = 5.f;     // grace period (s) after a wheel nudge before ship control can cancel it
        float scrollSensitivity = 1.08f; // zoom multiplier per wheel notch (was 1.15 -- felt oversensitive)

        bool enemyFraming = true;
        float enemyRadius = 1100.f; // consider enemies within this for framing
        float framingBias = 0.35f;  // 0 = stay on player, 1 = midpoint between player and enemy
        float framingMargin = 220.f;// extra world units kept around the framed pair
        float framingTau = 1.2f;    // time constant for easing framing in/out as an enemy appears/leaves

        // Celestial (planet/sun) framing: as the player nears a body, zoom out
        // to fit it so it's actually visible instead of filling the screen at
        // the speed-driven zoom-in; then release near the surface so the final
        // approach zooms back in (nice for a slow landing). Engaged only in a
        // band of surface distances (planetReleaseDist..planetFramingRange), so
        // it does nothing far out and hands back control right at the surface.
        bool planetFraming = true;
        float planetFramingRange = 3000.f; // surface distance at which framing starts fading in
        float planetReleaseDist = 180.f;   // surface distance below which framing releases (zoom in to land)
        float planetFramingMargin = 350.f; // extra world units kept around the fitted body

        // Close-combat zoom-in: when the framed enemy gets near (almost
        // colliding), pull the zoom back in toward closeZoomFraction of maxZoom
        // -- readable clash without going fully zoomed in. Overrides the
        // speed/framing zoom-out only at point-blank range.
        float closeZoomRange = 320.f;    // enemy distance under which the zoom-in ramps
        float closeZoomFraction = 0.7f;  // fraction of maxZoom reached at contact (not 1 = "not fully")

        // Look-ahead: lead the follow target along the ship's predicted path,
        // so the screen fills with where it is going instead of where it has
        // been. Second order (pos + v*t + a*t^2/2) rather than straight-line
        // velocity: at speed near a gravity well the path is an arc, and a
        // linear lead points off it exactly when the view matters most.
        bool lookAhead = true;
        float leadTime = 1.1f;         // seconds of trajectory to lead by
        float leadMaxFraction = 0.55f; // cap on the lead, as a fraction of the visible half-extent
        float leadTau = 0.7f;          // smoothing (s) for the lead vector -- a turn must not swing the view
    };

private:
    // The frame's worlds, set for the duration of one Update() call (see its
    // own parameter doc). Enemy and planet framing sweep it.
    const SceneView* m_view = nullptr;

    Camera m_camera;
    bool m_cameraFollow = true;
    // Dead-zone half-size as a fraction of the visible half-extent: the ship
    // roams the central (2*fraction) of the view before the camera follows.
    // Shrinks toward FRAMING while an enemy is being framed so the view tracks
    // the pair instead of letting it drift in the dead zone, and toward LEAD
    // as look-ahead engages -- a dead zone wider than the lead would simply
    // swallow it, leaving the ship centred and the lead invisible.
    static constexpr float DEAD_ZONE_FRACTION = 0.35f;
    static constexpr float DEAD_ZONE_FRACTION_FRAMING = 0.08f;
    static constexpr float DEAD_ZONE_FRACTION_LEAD = 0.12f;

    // Camera director state (see Update). m_cameraZoom is the smoothed zoom
    // actually applied to the camera each frame. The wheel writes m_manualZoom
    // and sets m_manualZoomActive, which locks the zoom there regardless of
    // speed/framing. It stays locked until the player actively controls the
    // ship (thrust/rotate) -- m_manualZoomGraceRemaining is a grace period
    // (CameraParams::manualHold) after a wheel nudge during which control
    // input doesn't cancel it, so scrolling while already moving doesn't
    // instantly undo the pick. Once cancelled, m_cameraZoom eases back to the
    // dynamic target via the usual tau-smoothing below.
    float m_cameraZoom;
    float m_manualZoom;
    bool m_manualZoomActive = false;
    float m_manualZoomGraceRemaining = 0.f;

    // Eased 0..1: how "framed" the view currently is. FollowWithDeadZone moves
    // the camera instantly to keep its target inside the dead zone, so feeding
    // it a target/dead-zone-size that itself jumps (enemy appears/leaves
    // radius) would snap the camera in one frame. Easing this amount in/out
    // instead means both the framing bias and the dead-zone shrink ramp in
    // smoothly, and FollowWithDeadZone's per-frame correction stays small.
    float m_framingAmount = 0.f;
    // Which enemy the camera is framing. Sticky: with several enemies in
    // range the raw "nearest" flips identity constantly as ships orbit, so
    // the framed target only switches when a rival is decisively closer
    // (FRAMING_SWITCH_FACTOR) -- pure nearest-wins would snap the pan target
    // between ships every few frames.
    //
    // May come from either of the view's worlds -- two *different*
    // flecs::world instances, each assigning ids independently, so a
    // same-world identity check is required wherever this is compared
    // (flecs::entity's operator== only compares the raw 64-bit id via its
    // implicit conversion, not which world it came from -- ids from two
    // worlds can and do collide). See SameEntity().
    flecs::entity m_framedEnemy{};
    // Eased enemy-relative offset (world units) used for the pan bias.
    // Smoothed toward the framed enemy's true offset so a target switch that
    // does happen glides instead of stepping; kept after the enemy leaves
    // range so the bias fades back out along the direction it faded in from.
    Magnum::Vector2 m_framedEnemyOffset{0.f, 0.f};
    // Eased distance (world units) from the player to the farthest in-range
    // enemy, driving the zoom-fit so *all* nearby enemies fit in view (not
    // just the framed one). Separate from the offset: pan follows one sticky
    // target, zoom must contain the whole group. Smoothed so a far enemy
    // entering/leaving range doesn't snap the zoom.
    float m_framedReach = 0.f;

    // Eased 0..1 engage amounts for the planet-fit zoom-out and the
    // close-combat zoom-in (see Update and CameraParams). Smoothed the
    // same way m_framingAmount is, so both blend in/out without snapping.
    float m_planetFramingAmount = 0.f;
    float m_closeZoomAmount = 0.f;

    // Eased look-ahead offset (world units) added to the follow target. Eased
    // for the same reason m_framedEnemyOffset is: the raw trajectory lead
    // reverses the instant the player flips to retro-thrust, and feeding that
    // straight to FollowWithDeadZone would whip the view across the ship.
    Magnum::Vector2 m_leadOffset{0.f, 0.f};

    // A rival enemy must be closer than (this * current target's distance)
    // to steal the framing. Exit hysteresis: the current target is kept
    // until it exceeds enemyRadius by 15%, so a ship hovering right at the
    // radius doesn't strobe the framing on/off.
    static constexpr float FRAMING_SWITCH_FACTOR = 0.7f;
    static constexpr float FRAMING_EXIT_RADIUS_FACTOR = 1.15f;

    CameraParams m_params;

    // True identity check across possibly-different worlds -- see
    // m_framedEnemy's field comment for why raw == isn't enough.
    static bool SameEntity(const flecs::entity& a, const flecs::entity& b);

    // Updates m_framedEnemy (sticky nearest hostile, see field comment) and
    // returns the framed enemy's current position, or nullopt when nothing is
    // in range. `outCoverDist` receives the distance to the farthest in-range
    // enemy (0 if none), for the group zoom-fit. Sweeps the whole view.
    std::optional<Magnum::Vector2> SelectFramedEnemy(const Magnum::Vector2& from, TeamId playerTeam,
                                                     float& outCoverDist);

    void SmoothZoom(float zoomTarget, float dtSeconds);

public:
    explicit CameraDirector(float initialZoom);

    Camera& GetCamera() { return m_camera; }
    CameraParams& GetCameraParams() { return m_params; }
    [[nodiscard]] float GetCameraZoom() const { return m_cameraZoom; }
    [[nodiscard]] bool IsManualZoomActive() const { return m_manualZoomActive; }

    // Mouse-wheel zoom: multiplicatively nudges a manual zoom target that
    // overrides the dynamic zoom until the player next thrusts/rotates (after
    // an initial CameraParams::manualHold grace period), then eases back.
    // `notches` is the scroll delta (positive = zoom in).
    void NudgeManualZoom(float notches);

    void ToggleCameraFollow() { m_cameraFollow = !m_cameraFollow; }

    [[nodiscard]] bool IsFollowing() const { return m_cameraFollow; }

    // Free-look: parks the camera on `position` and stops following the
    // subject, so Update() leaves it there until FocusSubject() re-engages.
    // Zoom stops easing too -- the whole director is idle while unfollowed.
    void LookAt(const Magnum::Vector2& position)
    {
        m_camera.SetPosition(position);
        m_cameraFollow = false;
    }

    void FocusSubject() { m_cameraFollow = true; }

    // Per-frame camera director: eases position (with enemy framing) and zoom
    // (speed-driven, enemy-fit, planet-fit, close-combat, or manual override)
    // toward their targets. `player` may be a dead/invalid entity (between
    // death and respawn) -- the update is then a no-op. Every world in `view`
    // is swept for enemy/planet framing, so a remote ship/planet the player
    // never locally simulates is framed exactly like a single-player local
    // one.
    //
    // `positionOverride`, when set, replaces `player`'s own Transform::pos as
    // the position fed into dead-zone follow, enemy-framing distance, and
    // planet-surface distance -- everything downstream of "where is the
    // player" in this function. Single-player never sets it (nullopt: reads
    // the real Transform, as before). Multiplayer's own predicted ship needs
    // it: Phase 5 reconciliation can hard-snap that Transform mid-frame
    // (ClientPrediction::Reconcile), and feeding the *raw* post-snap position
    // straight into dead-zone follow/framing here -- before any smoothing --
    // is what caused a fast, repeating position jitter distinct from (and
    // easily mistaken for) network lag in synced enemy/planet data. The
    // caller is expected to pass an already-smoothed position instead (see
    // CGame::m_visualCorrectionOffset).
    //
    // `gravityAccel` is the gravity field at the player, in world units/s^2 --
    // the second-order term of the look-ahead lead. The caller supplies it
    // because summing it means knowing the gravity constant and multiplier,
    // which are the sim's business, not the camera's (see CGame::GravityAt).
    // Zero is a safe value: the lead degrades to plain straight-line velocity.
    void Update(const SceneView& view, std::optional<flecs::entity> player, const Magnum::Vector2& viewportSize,
               float dtSeconds, std::optional<Magnum::Vector2> positionOverride = std::nullopt,
               const Magnum::Vector2& gravityAccel = {});
};

} // namespace Gravitaris
