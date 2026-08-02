#pragma once

#include <cstdint>

#include <flecs.h>

#include <gravitaris/game/gnc/control/flight-controller.hpp>
#include <gravitaris/game/gnc/guidance/behaviors.hpp>

namespace Gravitaris {

enum class AIBehavior {
    Idle,
    Evade,     // climbing out of a gravity well
    Intercept, // pursuing the target
    Orbit,     // patrolling the heaviest body
    Land,      // descending onto the order's subject body
    Landed,    // parked on the order's subject body, holding the pad
    Depart,    // climbing clear of the body it launched from
    Flee,      // breaking off from a threat while too hurt to fight it
};

// What the strategy layer (AIStrategySystem, docs/ai-ships.md's table) hands
// down to the tactical one. Tactics still own every reflex -- danger
// evasion, firing, retargeting when the subject dies -- so an order steers a
// pilot rather than driving it; with no order (or a dead subject) the pilot
// falls back to its own nearest-enemy dogfight pick, which is exactly what
// AIGoal::Dogfight wants anyway.
enum class AIOrderKind : std::uint8_t {
    None,
    Attack, // shoot the subject entity (ship, structure or freighter)
    Land,   // set down on the subject planet
    Patrol, // hold an orbit around the subject planet
};

struct AIOrder {
    AIOrderKind kind = AIOrderKind::None;
    flecs::entity subject;
};

// Tactical/temperament knobs for one AI pilot -- the tunable "personality"
// behind presets like Aggressive/Cautious (see ai/ai-preset-library.hpp).
// GuidanceParams/FlightControllerParams (also stored per-pilot on AIPilot)
// cover flight-dynamics tuning; this covers the decision layer above them.
struct AIPersonality {
    double engageRange = 6000.0;     // pursue the target inside this
    double standoffDistance = 50.0;  // desired range to hold during Intercept
    double fireRange = 350.0;        // opens fire this far out (was 250)
    double fireTolerance = 0.10;     // rad off the lead solution, still fires

    // Dogfight heading priority (units/s). Inside fireRange the nose tracks
    // the firing solution (FlightController's TrackBearing) rather than the
    // velocity correction, for as long as that correction stays under this;
    // past it the burn takes the heading back, so an arrival still flips and
    // brakes. A pilot whose attitude is only ever a byproduct of
    // station-keeping lands a shot inside fireTolerance by accident at best.
    double aimPriorityError = 45.0;

    // Gravity-well danger avoidance. A predicted approach inside evadeRadius
    // triggers Evade; once evading, the ship must clear evadeRadius*evadeMargin
    // before handing control back (hysteresis, avoids flapping at the boundary).
    // Authored against the 120-unit planets; AIPilotSystem raises it for any
    // body too big for the authored value (a sun) so it always clears the
    // surface (EVADE_SURFACE_CLEARANCE).
    double evadeRadius = 180.0;
    double evadeMargin = 1.5;
    int dangerLookaheadSteps = 120;  // ~2s at the fixed tick

    // Breaking off. A pilot under this fraction of its max hull stops
    // pursuing and runs from any threat inside fleeRange, until the gap
    // opens past fleeRange*fleeMargin (hysteresis, as with evadeMargin) --
    // then it patrols instead of re-engaging. There is no repair, so the
    // hull fraction never recovers: range is the only way back out of Flee.
    // 0 disables breaking off entirely (fights to the death).
    double fleeHealthFraction = 0.3;
    double fleeRange = 700.0;
    double fleeMargin = 1.6;

    // Jinking. A pilot that has lost hull within the last underFireTicks
    // weaves across its line of sight to the target instead of closing in a
    // straight line. jinkSpeed is the lateral velocity added (0 disables);
    // jinkPeriod is the ticks between reversals.
    double jinkSpeed = 30.0;
    std::uint32_t jinkPeriod = 26;
    std::uint32_t underFireTicks = 90;

    // Parked on a pad, an enemy this close is reason enough to leave: nothing
    // is winnable from the ground, and a stationary target is a free shot.
    double groundedThreatRange = 600.0;

    std::uint32_t decisionInterval = 15; // ticks between tactical re-evaluations

    // Firing cadence: burstCount shots fired burstShotInterval ticks apart,
    // then fireInterval ticks of cooldown before the next burst starts.
    // burstCount = 1 is a plain single-shot cadence (burstShotInterval
    // unused); fireInterval is what gates the wait between shots either way.
    std::uint32_t fireInterval = 8;       // ticks of cooldown after a burst completes
    std::uint32_t burstCount = 1;         // shots fired back-to-back per opportunity
    std::uint32_t burstShotInterval = 6;  // ticks between shots within a burst

    // "Fuzzy"/character knobs -- 0 disables each. Deterministic per (entity,
    // tick) seed (see AIPilotSystem), so replays stay bit-exact.
    double reactionJitter = 0.0;     // +/- fraction of decisionInterval
    double aimJitter = 0.04;         // +/- rad added to fireTolerance per shot
    double dangerIgnoreChance = 0.0; // odds [0,1) of shrugging off a fresh
                                     // danger episode entirely (Reckless) --
                                     // rolled once per episode, not every
                                     // tick, so it can actually commit to
                                     // flying into the well rather than
                                     // re-rolling itself into evading anyway.
};

// Server-only pilot state, never serialized (ADR 0001). AIPilotSystem
// re-evaluates the behavior on a decision interval and pushes one
// InputCommand per tick through the ship's InputQueue -- the same seam
// human input uses.
struct AIPilot {
    AIBehavior behavior = AIBehavior::Idle;
    flecs::entity target; // becomes NetId when netcode lands
    AIOrder order;        // written by AIStrategySystem; None on a pilot with no AIStrategy
    std::uint32_t decisionCooldown = 0;
    std::uint32_t fireCooldown = 0;

    // The body the pilot is climbing clear of, while it is. Held across the
    // climb so the hysteresis margin has something to compare against.
    flecs::entity departureSite;

    // The body a danger episode named, held the same way and for the same
    // reason: the prediction stops firing well before the ship is out of it.
    flecs::entity evadeSite;

    // Captured when entering Orbit so the patrol ring stays put. patrolBody
    // is the body being circled: the dominant well by default, the order's
    // subject under Patrol.
    flecs::entity patrolBody;
    double patrolRadius = 0.0;
    double patrolDirection = 1.0;

    FlightControllerParams flight;
    GuidanceParams guidance;
    AIPersonality personality;

    ThrottleState throttle;

    // Transient danger-episode tracking for AIPersonality::dangerIgnoreChance;
    // not part of the personality itself.
    bool wasInDanger = false;
    bool dangerSuppressed = false;

    // Whether the pilot is currently breaking off (AIPersonality::fleeRange
    // hysteresis), and the threat it is running from.
    bool fleeing = false;
    flecs::entity fleeThreat;

    // Hull seen at the previous tick, so a drop reads as "being shot at"
    // without a damage event to subscribe to, and the ticks of jinking that
    // a drop buys.
    float lastHp = -1.f;
    std::uint32_t underFireCooldown = 0;

    // Transient per-shot-attempt aim bias for AIPersonality::aimJitter: rolled
    // once when a firing opportunity opens and held steady while waiting for
    // an aligned shot, so a "sloppy" shot is a real, fixed aiming error rather
    // than the fire threshold flickering randomly tick to tick.
    double aimBias = 0.0;
    bool aimBiasRolled = false;

    // Shots left in the burst currently underway; 0 = between bursts (the
    // next successful shot starts a fresh one of personality.burstCount).
    std::uint32_t burstShotsRemaining = 0;
};

} // namespace Gravitaris
