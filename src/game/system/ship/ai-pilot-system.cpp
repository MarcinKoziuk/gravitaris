#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/gravity-source.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/freighter.hpp>
#include <gravitaris/game/component/input-queue.hpp>
#include <gravitaris/game/component/planet.hpp>
#include <gravitaris/game/component/structure.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/ai-pilot.hpp>
#include <gravitaris/game/component/ship-loadout.hpp>
#include <gravitaris/game/upgrade/upgrade-catalog.hpp>
#include <gravitaris/game/gnc/nav/trajectory-predictor.hpp>
#include <gravitaris/game/gnc/guidance/behaviors.hpp>
#include <gravitaris/game/gnc/control/flight-controller.hpp>
#include <gravitaris/game/util/splitmix.hpp>
#include <gravitaris/game/system/core/physics-system.hpp>
#include <gravitaris/game/system/ship/ship-controls-system.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/system/ship/ai-pilot-system.hpp>

namespace Gravitaris {

using Magnum::Vector2d;

static constexpr double PI = 3.14159265358979323846;

// A rival target has to be this much closer (squared distance) than the
// current one before a pilot switches to it.
static constexpr double RETARGET_RATIO = 0.5;

// Floor under a personality's evadeRadius, as a multiple of the body's own
// radius. Personalities are authored against planets; a sun is 2.5x a planet
// and would otherwise sit entirely inside even a Cautious pilot's evade
// radius, i.e. flying into it would never read as danger at all.
static constexpr double EVADE_SURFACE_CLEARANCE = 1.25;

// Patrol ring radius, as a multiple of the patrolled planet's own radius --
// outside the High Port's orbit (StructureLayout::ORBIT_RADIUS_FACTOR) so a
// defending pilot circles the complex rather than through it.
static constexpr double PATROL_RADIUS_FACTOR = 4.0;

// Beyond this multiple of the patrol radius, fly to the ring instead of
// orbiting toward it.
static constexpr double PATROL_APPROACH_FACTOR = 1.5;

// Added to a landing site's radius so the descent flares against the ship's
// own hull rather than the planet's surface (a fighter is ~18 units long).
static constexpr double SHIP_LANDING_CLEARANCE = 12.0;

// How far past a departure planet's surface a pilot climbs before it may
// turn onto its course -- outside the High Port's own orbit
// (StructureLayout::ORBIT_RADIUS_FACTOR), so leaving means leaving the whole
// complex rather than sliding through the middle of it.
static constexpr double DEPARTURE_CLEARANCE = 260.0;

// Hysteresis on that radius, so a course that dips back inside doesn't flap
// the pilot between departing and flying it (same idea as evadeMargin).
static constexpr double DEPARTURE_MARGIN = 1.25;

// Padding on a body's radius when testing whether a shot would hit it, so a
// pilot doesn't graze the surface trying to shoot past a limb.
static constexpr double SHOT_CLEARANCE = 15.0;

static double WrapToPi(double angle);
static std::optional<double> SolveInterceptTime(const Vector2d& relPos, const Vector2d& relVel,
                                                double projectileSpeed);
static double DepartureRadius(flecs::entity site);
static bool SegmentHitsCircle(const Vector2d& from, const Vector2d& to, const Vector2d& center,
                              double radius);

AIPilotSystem::AIPilotSystem(flecs::world& registry, PhysicsSystem& physicsSystem,
                             TrajectoryPredictor& predictor, const UpgradeCatalog& catalog)
        : m_registry(registry)
        , m_physicsSystem(physicsSystem)
        , m_predictor(predictor)
        , m_catalog(catalog)
{}

void AIPilotSystem::Update(std::uint64_t step)
{
    // Celestial attractors, for picking each pilot's dominant gravity source.
    struct Source {
        flecs::entity entity;
        Vector2d pos;
        Vector2d vel;
        double mass;
        double radius;
    };
    std::vector<Source> sources;
    m_registry.each([&](flecs::entity ent, const Transform& transf, const GravitySource& gs) {
        const Planet* planet = ent.try_get<Planet>();
        sources.push_back({ent, transf.pos, transf.vel, gs.mass * gs.multiplier,
                           planet ? planet->radius * transf.scale.x() : 0.0});
    });

    const auto findSource = [&sources](flecs::entity ent) -> const Source* {
        for (const Source& src : sources) {
            if (src.entity == ent) return &src;
        }
        return nullptr;
    };

    const auto nearestSource = [&sources](const Vector2d& pos) -> const Source* {
        const Source* nearest = nullptr;
        double nearestDistSq = std::numeric_limits<double>::max();
        for (const Source& src : sources) {
            const double distSq = (src.pos - pos).dot();
            if (distSq < nearestDistSq) {
                nearestDistSq = distSq;
                nearest = &src;
            }
        }
        return nearest;
    };

    // LandOnBody subtracts gravity from available thrust, so it needs the
    // world's real attraction, not the per-body figure Source carries.
    const double gravityMultiplier = static_cast<double>(m_physicsSystem.GetGravityMultiplier());

    // Every potential combat target -- a piloted ship (player or AI), not a
    // Freighter (background economy actor, never fought) or a Structure
    // (Base/Colony/etc. are also Team+Damageable, but StructureDefenseSystem
    // handles those separately). There's no single "the player" to fall back
    // on here the way single-player's Game::m_player used to be: a dedicated
    // server never calls Game::Start(), so that field is always nullopt
    // there regardless of how many peers are actually connected -- nearest
    // live enemy works identically in single-player (only one candidate
    // exists) and multiplayer (any number of peers/AI on either side).
    struct Candidate {
        flecs::entity entity;
        Vector2d pos;
        TeamId team;
    };
    std::vector<Candidate> candidates;
    m_registry.each([&](flecs::entity ent, const Transform& transf, const Team& team, const Damageable&) {
        if (ent.has<Freighter>() || ent.has<Structure>()) return;
        candidates.push_back({ent, transf.pos, team.id});
    });

    m_registry.each([&](flecs::entity ent, Transform& transf, PhysicsRef& ref,
                        AIPilot& pilot, InputQueue& queue, const Team& myTeam) {
        const AIPersonality& personality = pilot.personality;

        // This pilot's own fitted gun -- what it actually has to lead with.
        const ShipLoadout* loadout = ent.try_get<ShipLoadout>();
        const WeaponDef* gun = m_catalog.ResolveStats(loadout ? loadout->levels : UpgradeLevels{}).gun;
        const double bulletSpeed = gun ? gun->speed : 0.0;

        // Hull state drives both breaking off and jinking. A drop since last
        // tick is the "being shot at" signal -- damage is resolved before
        // this system runs and nothing else needs an event for it.
        const Damageable* hull = ent.try_get<Damageable>();
        if (hull) {
            if (pilot.lastHp >= 0.f && hull->hp < pilot.lastHp) {
                pilot.underFireCooldown = personality.underFireTicks;
            }
            pilot.lastHp = hull->hp;
        }
        if (pilot.underFireCooldown > 0) --pilot.underFireCooldown;

        const bool hurt = hull && hull->maxHp > 0.f && personality.fleeHealthFraction > 0.0
                && hull->hp / hull->maxHp < static_cast<float>(personality.fleeHealthFraction);

        // Deterministic per-(tick, entity) seed for this pilot's jitter/
        // danger-ignore rolls below -- same value every replay of this tick.
        std::uint64_t rng = SplitMix64Seed(step, ent.id());

        const Source* well = nullptr;
        for (const Source& src : sources) {
            if (src.entity == ent) continue;
            if (!well || src.mass > well->mass) well = &src;
        }

        const double evadeRadius = well
                ? std::max(personality.evadeRadius, well->radius * EVADE_SURFACE_CLEARANCE)
                : personality.evadeRadius;

        // A live Attack order names the target outright -- the strategy layer
        // has already weighed a structure or freighter against the nearest
        // enemy fighter, and the proximity rule below would only undo that.
        const bool ordered = pilot.order.kind != AIOrderKind::None && pilot.order.subject.is_alive();
        if (ordered && pilot.order.kind == AIOrderKind::Attack) {
            pilot.target = pilot.order.subject;
        }
        // Re-picked on the decision cadence, not only when the current target
        // dies: a pilot locked on a distant enemy flies off at cruise speed
        // past whoever is actually shooting at it, which reads as fleeing.
        // Switching needs the newcomer to be clearly closer (RETARGET_RATIO)
        // so two enemies at similar range don't make it flip-flop.
        else if (!pilot.target.is_alive() || pilot.decisionCooldown == 0) {
            flecs::entity nearest;
            double nearestDistSq = std::numeric_limits<double>::max();
            for (const Candidate& c : candidates) {
                if (c.entity == ent || c.team == myTeam.id) continue;
                const double distSq = (c.pos - transf.pos).dot();
                if (distSq < nearestDistSq) {
                    nearestDistSq = distSq;
                    nearest = c.entity;
                }
            }

            if (!pilot.target.is_alive()) {
                pilot.target = nearest;
            }
            else if (nearest.is_alive() && nearest != pilot.target) {
                const Transform* current = pilot.target.try_get<Transform>();
                const double currentDistSq = current ? (current->pos - transf.pos).dot() : 0.0;
                if (!current || nearestDistSq < currentDistSq * RETARGET_RATIO) {
                    pilot.target = nearest;
                }
            }
        }
        const Transform* targetTransf =
                pilot.target.is_alive() ? pilot.target.try_get<Transform>() : nullptr;

        const AIBehavior previous = pilot.behavior;

        if (pilot.decisionCooldown > 0) {
            --pilot.decisionCooldown;
        }
        else {
            if (personality.reactionJitter > 0.0) {
                const double jitter = (SplitMix64NextUnit(rng) - 0.5) * 2.0
                        * personality.reactionJitter * personality.decisionInterval;
                pilot.decisionCooldown = static_cast<std::uint32_t>(
                        std::max(1.0, static_cast<double>(personality.decisionInterval) + jitter));
            }
            else {
                pilot.decisionCooldown = personality.decisionInterval;
            }

            pilot.guidance.accel = ShipControlsSystem::THRUST_FORCE
                    / cpBodyGetMass(m_physicsSystem.GetBody(ref).cp.body.get());

            // A Patrol order circles the body it names (the faction's own
            // planet); with no order, the dominant well.
            const bool patrolOrdered = ordered && pilot.order.kind == AIOrderKind::Patrol;
            const Source* patrolBody = patrolOrdered ? findSource(pilot.order.subject) : well;

            // Tactical pick among Land/Intercept/Orbit/Idle; the danger check
            // below (which runs every tick, not just on this slower cadence)
            // can still override this with Evade regardless of what's picked
            // here.
            if (ordered && pilot.order.kind == AIOrderKind::Land) {
                pilot.behavior = AIBehavior::Land;
            }
            else if (!hurt && targetTransf
                     && (targetTransf->pos - transf.pos).length() < personality.engageRange) {
                pilot.behavior = AIBehavior::Intercept;
            }
            else if (patrolBody) {
                pilot.behavior = AIBehavior::Orbit;
                if (previous != AIBehavior::Orbit || pilot.patrolBody != patrolBody->entity) {
                    const Vector2d r = transf.pos - patrolBody->pos;
                    pilot.patrolBody = patrolBody->entity;
                    pilot.patrolRadius = patrolOrdered
                            ? patrolBody->radius * PATROL_RADIUS_FACTOR
                            : std::max(r.length(), evadeRadius * 2.0);
                    const double cross = r.x() * transf.vel.y() - r.y() * transf.vel.x();
                    pilot.patrolDirection = (cross < 0.0) ? -1.0 : 1.0;
                }
            }
            else {
                pilot.behavior = AIBehavior::Idle;
            }
        }

        // Breaking off, checked every tick like the danger override below.
        // The hysteresis is on range rather than on hull: nothing repairs a
        // ship, so a hull fraction that has fallen through the threshold
        // never climbs back out, and opening the gap is the only way a hurt
        // pilot stops running. Above the threshold it never picked Intercept
        // in the first place, so it patrols instead of re-engaging.
        if (!hurt) {
            pilot.fleeing = false;
            pilot.fleeThreat = flecs::entity();
        }
        else {
            flecs::entity threat;
            double threatDistSq = std::numeric_limits<double>::max();
            for (const Candidate& c : candidates) {
                if (c.entity == ent || c.team == myTeam.id) continue;
                const double distSq = (c.pos - transf.pos).dot();
                if (distSq < threatDistSq) {
                    threatDistSq = distSq;
                    threat = c.entity;
                }
            }

            const double range =
                    personality.fleeRange * (pilot.fleeing ? personality.fleeMargin : 1.0);
            pilot.fleeing = threat.is_alive() && threatDistSq < range * range;
            pilot.fleeThreat = pilot.fleeing ? threat : flecs::entity();
            if (pilot.fleeing) pilot.behavior = AIBehavior::Flee;
        }

        // Where this pilot's business actually is: the order's subject, or
        // whoever it is dogfighting.
        const Transform* objective = nullptr;
        if (ordered) {
            objective = pilot.order.subject.try_get<Transform>();
        }
        else if (targetTransf) {
            objective = targetTransf;
        }

        // Departure, checked every tick like the danger override below: a
        // ship whose business is elsewhere climbs clear of the body it is
        // sitting on before turning onto its course, since turning where it
        // stands drags it across the deck, the surface, or the rest of the
        // complex. An objective at that same body -- a landing, an attack on
        // its structures, a patrol of it -- is not elsewhere.
        const Source* neighbourhood = nearestSource(transf.pos);
        if (neighbourhood) {
            const double clearRadius = DepartureRadius(neighbourhood->entity);
            const double distance = (transf.pos - neighbourhood->pos).length();
            const bool objectiveHere = objective
                    && (objective->pos - neighbourhood->pos).length() < clearRadius;
            const bool departing = pilot.departureSite == neighbourhood->entity;

            if (objective && !objectiveHere
                && distance < clearRadius * (departing ? DEPARTURE_MARGIN : 1.0)) {
                pilot.departureSite = neighbourhood->entity;
                pilot.behavior = AIBehavior::Depart;
            }
            else if (departing) {
                pilot.departureSite = flecs::entity();
            }
        }

        // Danger check: every tick, not gated behind decisionCooldown. A
        // pursuit/orbit path is actively thrust-driven and can curve toward a
        // well between decision points; TrajectoryPredictor only coasts
        // (gravity, no thrust -- see its class comment), so checking every
        // tick means the moment the ship's actual velocity starts curving
        // into danger, it's caught within a tick instead of up to
        // decisionInterval ticks late.
        bool predictedDanger = false;
        if (well) {
            const std::vector<Vector2d> path =
                    m_predictor.Predict(ent, personality.dangerLookaheadSteps, Game::PHYSICS_DELTA);
            for (const Vector2d& p : path) {
                if ((p - well->pos).length() < evadeRadius) {
                    predictedDanger = true;
                    break;
                }
            }
        }

        // Roll once per fresh danger episode (not every tick it persists) so
        // a Reckless ship that shrugs off a warning actually commits to the
        // risky path rather than re-rolling itself into evading a tick later.
        if (predictedDanger && !pilot.wasInDanger) {
            pilot.dangerSuppressed = personality.dangerIgnoreChance > 0.0
                    && SplitMix64NextUnit(rng) < personality.dangerIgnoreChance;
        }
        if (!predictedDanger) {
            pilot.dangerSuppressed = false;
        }
        pilot.wasInDanger = predictedDanger;

        const bool effectiveDanger = predictedDanger && !pilot.dangerSuppressed;

        if (effectiveDanger) {
            pilot.behavior = AIBehavior::Evade;
        }
        else if (pilot.behavior == AIBehavior::Evade) {
            // Hysteresis: don't hand control back the instant the prediction
            // clears -- wait until genuinely clear of the well, or this would
            // flap Evade/Intercept right at the trigger boundary.
            const bool clear = !well
                    || (transf.pos - well->pos).length() > evadeRadius * personality.evadeMargin;
            if (clear) {
                pilot.decisionCooldown = 0; // re-pick a tactical behavior next tick
            }
        }

        Vector2d desiredVel = transf.vel; // Idle: no correction
        switch (pilot.behavior) {
            case AIBehavior::Evade:
                if (well) {
                    desiredVel = EvadeBody(transf, well->pos, well->vel,
                                           evadeRadius * personality.evadeMargin, pilot.guidance);
                }
                break;
            case AIBehavior::Intercept:
                if (targetTransf) {
                    GuidanceParams standoff = pilot.guidance;
                    standoff.arriveRadius = personality.standoffDistance;
                    desiredVel = InterceptEntity(transf, *targetTransf, standoff);

                    // Under fire, weave across the line of sight: a straight
                    // closing run hands whoever is shooting a free lead
                    // solution. The reversal phase is offset per entity so a
                    // pair of wingmen don't weave in lockstep.
                    if (pilot.underFireCooldown > 0 && personality.jinkSpeed > 0.0
                        && personality.jinkPeriod > 0) {
                        const Vector2d los = targetTransf->pos - transf.pos;
                        const double dist = los.length();
                        if (dist > 1e-6) {
                            const Vector2d lateral{-los.y() / dist, los.x() / dist};
                            const bool right = ((step / personality.jinkPeriod + ent.id()) & 1u) != 0u;
                            desiredVel += lateral
                                    * (right ? personality.jinkSpeed : -personality.jinkSpeed);
                            const double speed = desiredVel.length();
                            if (speed > pilot.guidance.maxSpeed) {
                                desiredVel *= pilot.guidance.maxSpeed / speed;
                            }
                        }
                    }
                }
                break;
            case AIBehavior::Flee:
                if (pilot.fleeThreat.is_alive()) {
                    if (const Transform* threat = pilot.fleeThreat.try_get<Transform>()) {
                        desiredVel = FleeThreat(transf, threat->pos, threat->vel, pilot.guidance);
                    }
                }
                break;
            case AIBehavior::Orbit:
                if (const Source* body = findSource(pilot.patrolBody)) {
                    const Vector2d r = transf.pos - body->pos;
                    const double dist = r.length();
                    if (dist > pilot.patrolRadius * PATROL_APPROACH_FACTOR) {
                        // OrbitBody's radial term is clamped to a station
                        // -keeping trickle, so closing a long way to the ring
                        // is GotoPoint's job.
                        desiredVel = GotoPoint(transf, body->pos + r * (pilot.patrolRadius / dist),
                                               pilot.guidance) + body->vel;
                    }
                    else {
                        desiredVel = OrbitBody(transf, body->pos, body->mass, pilot.patrolRadius,
                                               pilot.patrolDirection, pilot.guidance) + body->vel;
                    }
                }
                break;
            case AIBehavior::Depart:
                if (pilot.departureSite.is_alive()) {
                    const Transform& site = pilot.departureSite.get<Transform>();
                    desiredVel = EvadeBody(transf, site.pos, site.vel,
                                           DepartureRadius(pilot.departureSite), pilot.guidance);
                }
                break;
            case AIBehavior::Land:
                if (const Source* site = ordered ? findSource(pilot.order.subject) : nullptr) {
                    desiredVel = LandOnBody(transf, site->pos, site->vel, site->mass * gravityMultiplier,
                                            site->radius + SHIP_LANDING_CLEARANCE, pilot.guidance);
                }
                break;
            case AIBehavior::Idle:
                break;
        }

        ControlFlags flags = FlyToVelocity(transf, desiredVel, pilot.flight, &pilot.throttle);

        if (pilot.fireCooldown > 0) {
            --pilot.fireCooldown;
        }
        else if (pilot.behavior == AIBehavior::Intercept && targetTransf) {
            const Vector2d relPos = targetTransf->pos - transf.pos;
            const Vector2d relVel = targetTransf->vel - transf.vel;
            if (relPos.length() < personality.fireRange) {
                if (std::optional<double> t = SolveInterceptTime(relPos, relVel, bulletSpeed)) {
                    const Vector2d aim = relPos + relVel * (*t);
                    const double aimHeading = std::atan2(aim.y(), aim.x());
                    const double heading = static_cast<double>(transf.rot) - PI / 2.0;

                    // Rolled once per firing opportunity (not every tick) and
                    // held steady while waiting for an aligned shot -- a
                    // sloppy shot is then a real, fixed aiming error rather
                    // than the fire threshold flickering randomly tick to
                    // tick (which would look like the gun spraying).
                    if (personality.aimJitter > 0.0 && !pilot.aimBiasRolled) {
                        pilot.aimBias = (SplitMix64NextUnit(rng) - 0.5) * 2.0 * personality.aimJitter;
                        pilot.aimBiasRolled = true;
                    }
                    const double tolerance = personality.fireTolerance
                            + (personality.aimJitter > 0.0 ? pilot.aimBias : 0.0);

                    // Weapon discipline: bullets are gravity-immune and fly
                    // straight, so a body across the firing solution eats the
                    // shot. Holding fire costs nothing (the cooldown is only
                    // spent on shots actually taken) and stops a pilot from
                    // emptying itself into a planet the target is hiding
                    // behind.
                    bool blocked = false;
                    for (const Source& src : sources) {
                        if (src.radius <= 0.0) continue;
                        if (SegmentHitsCircle(transf.pos, transf.pos + aim, src.pos,
                                              src.radius + SHOT_CLEARANCE)) {
                            blocked = true;
                            break;
                        }
                    }

                    if (!blocked && std::abs(WrapToPi(aimHeading - heading)) < tolerance) {
                        flags.firePrimary = true;
                        pilot.aimBiasRolled = false; // roll fresh for the next shot

                        if (personality.burstCount > 1) {
                            if (pilot.burstShotsRemaining == 0) {
                                pilot.burstShotsRemaining = personality.burstCount;
                            }
                            --pilot.burstShotsRemaining;
                            pilot.fireCooldown = pilot.burstShotsRemaining > 0
                                    ? personality.burstShotInterval
                                    : personality.fireInterval;
                        }
                        else {
                            pilot.fireCooldown = personality.fireInterval;
                        }
                    }
                }
            }
        }
        else {
            pilot.aimBiasRolled = false; // no live shot attempt -- clear for next time
        }

        queue.Push(InputCommand{step, flags});
    });
}

// Distance from a departure site's center a pilot must reach to be clear of
// it: its own surface, plus room for the turn onto course.
// Whether the segment from-to passes within `radius` of `center`.
static bool SegmentHitsCircle(const Vector2d& from, const Vector2d& to, const Vector2d& center,
                              double radius)
{
    const Vector2d along = to - from;
    const double lengthSq = along.dot();
    const double t = lengthSq > 1e-9
            ? std::clamp(Magnum::Math::dot(center - from, along) / lengthSq, 0.0, 1.0)
            : 0.0;
    return (from + along * t - center).length() < radius;
}

static double DepartureRadius(flecs::entity site)
{
    double radius = 0.0;
    if (const Planet* planet = site.try_get<Planet>()) {
        radius = planet->radius * site.get<Transform>().scale.x();
    }
    return radius + DEPARTURE_CLEARANCE;
}

static double WrapToPi(double angle)
{
    angle = std::fmod(angle + PI, 2.0 * PI);
    if (angle < 0.0) angle += 2.0 * PI;
    return angle - PI;
}

// Smallest positive time at which a projectile of `projectileSpeed` (relative
// to the shooter) meets a target at relPos moving at relVel.
static std::optional<double> SolveInterceptTime(const Vector2d& relPos, const Vector2d& relVel,
                                                double projectileSpeed)
{
    const double a = relVel.dot() - projectileSpeed * projectileSpeed;
    const double b = 2.0 * Magnum::Math::dot(relPos, relVel);
    const double c = relPos.dot();

    if (std::abs(a) < 1e-9) {
        if (std::abs(b) < 1e-9) return std::nullopt;
        const double t = -c / b;
        return t > 0.0 ? std::optional<double>(t) : std::nullopt;
    }

    const double disc = b * b - 4.0 * a * c;
    if (disc < 0.0) return std::nullopt;

    const double sq = std::sqrt(disc);
    const double t1 = (-b - sq) / (2.0 * a);
    const double t2 = (-b + sq) / (2.0 * a);

    double t = std::numeric_limits<double>::max();
    if (t1 > 0.0) t = std::min(t, t1);
    if (t2 > 0.0) t = std::min(t, t2);
    return t != std::numeric_limits<double>::max() ? std::optional<double>(t) : std::nullopt;
}

} // namespace Gravitaris
