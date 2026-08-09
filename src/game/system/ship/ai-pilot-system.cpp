#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

#include <ankerl/unordered_dense.h>

#include <gravitaris/gravitaris.hpp>
#include <gravitaris/game/math-utils.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/gravity-source.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/faction-state.hpp>
#include <gravitaris/game/component/freighter.hpp>
#include <gravitaris/game/component/input-queue.hpp>
#include <gravitaris/game/component/landing-state.hpp>
#include <gravitaris/game/component/planet.hpp>
#include <gravitaris/game/component/planet-attachment.hpp>
#include <gravitaris/game/component/structure.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/ai-pilot.hpp>
#include <gravitaris/game/component/ai-strategy.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/ship-loadout.hpp>
#include <gravitaris/game/component/research-access.hpp>
#include <gravitaris/game/upgrade/upgrade-catalog.hpp>
#include <gravitaris/game/gnc/nav/trajectory-predictor.hpp>
#include <gravitaris/game/gnc/guidance/behaviors.hpp>
#include <gravitaris/game/gnc/control/flight-controller.hpp>
#include <gravitaris/game/util/splitmix.hpp>
#include <gravitaris/game/system/core/physics-system.hpp>
#include <gravitaris/game/system/gwell/home-site.hpp>
#include <gravitaris/game/system/ship/ship-controls-system.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/system/ship/ai-pilot-system.hpp>

#include "game/system/ship/ai-refit.hpp"

namespace Gravitaris {

using Magnum::Vector2d;

static constexpr std::size_t NUM_TEAMS = 7; // TeamId::Blue..None

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

// How long clear of a yard before a pilot's shopping budget comes back. Long
// enough that lifting off the deck is not immediately another shopping trip,
// short enough that one fight is always followed by a fresh one.
static constexpr std::uint16_t REFIT_REARM_TICKS = 600; // 10s at the fixed tick

// How far past a departure planet's surface a pilot climbs before it may
// turn onto its course -- outside the High Port's own orbit
// (StructureLayout::ORBIT_RADIUS_FACTOR), so leaving means leaving the whole
// complex rather than sliding through the middle of it.
static constexpr double DEPARTURE_CLEARANCE = 260.0;

// Hysteresis on that radius, so a course that dips back inside doesn't flap
// the pilot between departing and flying it (same idea as evadeMargin). The
// climb runs for as long as this state does -- EvadeBody commands
// unconditionally, so the band is flown under power rather than coasted
// through, which is the only way a slow climb clears it at all.
static constexpr double DEPARTURE_MARGIN = 1.25;

// Padding on a body's radius when testing whether a shot would hit it, so a
// pilot doesn't graze the surface trying to shoot past a limb.
static constexpr double SHOT_CLEARANCE = 15.0;

// How much wider than the station itself the blocked arc of its ring is
// treated as being. Covers the ground both it and the descending ship make
// while the descent is flown, so a corridor that was clear when it was picked
// is still clear on arrival.
static constexpr double RING_GUARD_FACTOR = 4.0;

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

    // Everywhere anyone could set down, gathered before the walk over pilots
    // because a query run inside another query's callback yields nothing (see
    // CLAUDE.md) -- which is also why the rings and the site->planet mapping
    // below are built here rather than asked per pilot.
    const std::vector<HomeSite> homeSites = GatherHomeSites(m_registry);

    std::array<bool, NUM_TEAMS> teamHasLab{};
    for (const HomeSite& site : homeSites) {
        if (site.hasLab) teamHasLab[static_cast<std::size_t>(site.team)] = true;
    }

    // Held by entity, not by pointer: a component address is only good until
    // the world changes shape, and this is read per pilot below.
    std::array<flecs::entity, NUM_TEAMS> factionOf{};
    m_registry.each([&](flecs::entity ent, const FactionState& fs) {
        factionOf[static_cast<std::size_t>(fs.team)] = ent;
    });

    // Which planet a landing counts against, for every site there is: a
    // planet is its own, a station is the one it orbits.
    ankerl::unordered_dense::map<std::uint32_t, flecs::entity> planetByNetId;
    ankerl::unordered_dense::map<std::uint32_t, std::uint32_t> sitePlanetOf;
    m_registry.each([&](flecs::entity ent, const Planet&, const NetId& netId) {
        planetByNetId.emplace(netId.value, ent);
        sitePlanetOf.emplace(netId.value, netId.value);
    });

    // Every station ring in the world, whoever holds it. A High Port is solid
    // and sits squarely on the way down, so it blocks a descent onto the
    // planet underneath whether it is a friendly yard or an enemy's.
    struct Ring {
        flecs::entity entity;
        std::uint32_t planetNetId = 0;
        Vector2d pos;
        double orbitRadius = 0.0;
        double radius = 0.0;
        double direction = 1.0;
    };
    std::vector<Ring> rings;
    m_registry.each([&](flecs::entity ent, const Structure& s, const Transform& transf,
                        const NetId& netId, const PlanetOrbitAttachment& orbit) {
        sitePlanetOf.emplace(netId.value, orbit.planetNetId);
        if (s.type != StructureType::HighPort) return;
        rings.push_back(Ring{ent, orbit.planetNetId, transf.pos, orbit.radius, ObstacleRadius(ent),
                             orbit.direction});
    });

    // A descent is radial, and a High Port rides a ring squarely across it.
    // Where the way down is blocked the answer is to come in beside the
    // station rather than through it: a point on the ring just past the arc
    // it blocks, on the side it is travelling away from, so the gap is
    // opening rather than closing while the ship crosses to it. Deliberately
    // not the far side of the ring -- guidance arrives in a straight line,
    // and the straight line to the far side runs through the planet.
    //
    // Nothing to do when the station IS the destination, when the ship is
    // already inside the ring, or when the way down was never blocked.
    const auto ringEntryPoint = [&](const Vector2d& from, flecs::entity well,
                                    std::uint32_t planetNetId,
                                    flecs::entity subject) -> std::optional<Vector2d> {
        const Source* body = findSource(well);
        if (!body) return std::nullopt;

        for (const Ring& ring : rings) {
            if (ring.planetNetId != planetNetId || ring.entity == subject) continue;

            const Vector2d toShip = from - body->pos;
            const Vector2d toStation = ring.pos - body->pos;
            if (toShip.length() <= ring.orbitRadius || toStation.length() < 1e-6) continue;

            // How wide the station reads from the planet's centre, widened to
            // cover the ground both of them make during the descent.
            const double halfWidth = std::asin(std::clamp(
                    (ring.radius + SHIP_LANDING_CLEARANCE) / ring.orbitRadius, 0.0, 1.0));
            const double guard = std::min(halfWidth * RING_GUARD_FACTOR, PI / 2.0);
            const double bearing = std::atan2(toShip.y(), toShip.x());
            const double stationBearing = std::atan2(toStation.y(), toStation.x());
            if (std::abs(WrapToPi(bearing - stationBearing)) > guard) continue;

            const double entryBearing = stationBearing - ring.direction * guard;
            return body->pos
                    + Vector2d{std::cos(entryBearing), std::sin(entryBearing)} * ring.orbitRadius;
        }
        return std::nullopt;
    };

    // Wing orders. A pilot with no AIStrategy of its own -- everything that
    // isn't a faction leader -- flies its team leader's objective instead of
    // its own nearest-enemy pick, which is what turns one strategic ship into
    // a squadron. A Land order is the exception: a claim is one ship setting
    // down, so the wing covers the descent (Patrol) rather than piling onto
    // the same rock. Lowest NetId wins if a team somehow fields two leaders,
    // so the choice doesn't ride on flecs iteration order (ADR 0001).
    struct WingOrder {
        AIOrder order;
        std::uint32_t netId = std::numeric_limits<std::uint32_t>::max();
    };
    ankerl::unordered_dense::map<std::uint8_t, WingOrder> wingOrders;
    m_registry.each([&](flecs::entity ent, const AIStrategy& strategy, const AIPilot& leader, const Team& team,
                        const NetId& netId) {
        if (leader.order.kind == AIOrderKind::None || !leader.order.subject.is_alive()) return;
        // Two of a leader's goals are its own business: its dogfight pick is
        // one enemy out of many and the wing has its own nearest, and a trip
        // home is about that leader's hull, not the wing's.
        if (strategy.goal == AIGoal::Dogfight || strategy.goal == AIGoal::Rearm) return;

        WingOrder& slot = wingOrders[static_cast<std::uint8_t>(team.id)];
        if (netId.value >= slot.netId) return;

        slot.netId = netId.value;
        slot.order = leader.order;
        if (slot.order.kind == AIOrderKind::Land) slot.order.kind = AIOrderKind::Patrol;
    });

    m_registry.each([&](flecs::entity ent, Transform& transf, PhysicsRef& ref,
                        AIPilot& pilot, InputQueue& queue, const Team& myTeam) {
        const AIPersonality& personality = pilot.personality;

        // What this pilot is actually carrying: the gun it has to lead with,
        // and the rack it can reach past it with.
        const ShipLoadout* loadout = ent.try_get<ShipLoadout>();
        const ShipStats stats = m_catalog.ResolveStats(loadout ? loadout->levels : UpgradeLevels{});
        const double bulletSpeed = stats.gun ? stats.gun->speed : 0.0;

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

        // The body that rules where the pilot actually is. Raw mass picks the
        // sector's sun from anywhere in it, which is the wrong answer for
        // every question asked of it -- an idle pilot circled the sun rather
        // than the rock it was next to. Gravitational influence is the same
        // falloff the sim's own force law uses.
        const Source* dominant = nullptr;
        double bestInfluence = 0.0;
        for (const Source& src : sources) {
            if (src.entity == ent) continue;
            const double influence = src.mass / std::max((src.pos - transf.pos).dot(), 1.0);
            if (!dominant || influence > bestInfluence) {
                bestInfluence = influence;
                dominant = &src;
            }
        }

        // How close this pilot is willing to come to a given body. Authored
        // against the 120-unit planets, so anything bigger raises it to keep
        // the clearance a personality was written to expect.
        const auto evadeRadiusOf = [&personality](const Source& src) {
            return std::max(personality.evadeRadius, src.radius * EVADE_SURFACE_CLEARANCE);
        };

        // Shopping: something on offer that this pilot wants and can pay for,
        // for as long as it is still willing to wait around for it
        // (AIPersonality::upgradeGreed, bounded by padWaitRemaining so a wing
        // never parks forever). With no lab left there is nowhere to fit it.
        //
        // The wait is spent standing at a yard, and both budgets come back
        // once the pilot is clear of one. Counted down every tick from spawn
        // instead, the window closed 45 seconds into the match and a pilot
        // that had bought its one allowance never shopped again -- which is
        // the whole of why an AI wing flew stock hulls all round.
        const ResearchAccess* access = ent.try_get<ResearchAccess>();
        if (access && access->atLab) {
            if (pilot.padWaitRemaining > 0) --pilot.padWaitRemaining;
        }
        else if (access && access->ticksSinceLab >= REFIT_REARM_TICKS) {
            pilot.padWaitRemaining = personality.padWaitTicks;
            pilot.upgradesWanted = personality.upgradeGreed;
        }
        const std::size_t myTeamIndex = static_cast<std::size_t>(myTeam.id);
        const flecs::entity factionEntity = factionOf[myTeamIndex];
        const FactionState* faction =
                factionEntity.is_alive() ? factionEntity.try_get<FactionState>() : nullptr;
        const bool shopping = teamHasLab[myTeamIndex] && pilot.padWaitRemaining > 0
                && WantsRefit(m_catalog, ent, pilot, faction);

        // A pilot with no strategy of its own flies its leader's objective,
        // except when its own hull or its own shopping list says otherwise --
        // the two things no one else can decide for it. (A leader gets the
        // same trip from AIStrategySystem, as AIGoal::Rearm.)
        if (!ent.has<AIStrategy>()) {
            HomeSite home;
            if (hurt || shopping) {
                // A shopping trip wants the lab; anything else just wants
                // friendly ground, and takes the lab site only if it is also
                // the nearest.
                if (shopping) home = SelectHomeSite(homeSites, myTeam.id, transf.pos, /*needsLab=*/true);
                if (!home.IsValid()) {
                    home = SelectHomeSite(homeSites, myTeam.id, transf.pos, /*needsLab=*/false);
                }
            }
            if (home.IsValid()) {
                pilot.order = AIOrder{AIOrderKind::Land, home.entity};
            }
            else {
                const auto wing = wingOrders.find(static_cast<std::uint8_t>(myTeam.id));
                pilot.order = wing != wingOrders.end() ? wing->second.order : AIOrder{};
            }
        }

        // Nothing sets down on a star and comes back (DamageSystem::
        // ResolveStarContact). Caught here rather than at each of the places
        // a Land order is honored, so the descent, the pad-holding rule and
        // the danger-evasion exemption all see the same demoted order.
        if (pilot.order.kind == AIOrderKind::Land && pilot.order.subject.is_alive()) {
            const Planet* body = pilot.order.subject.try_get<Planet>();
            if (body && body->star) pilot.order.kind = AIOrderKind::Patrol;
        }

        // What a Land order names, as something to put legs down on. A planet
        // states its own radius and pulls with its own mass; a station has
        // only the extent of its model, and what holds a ship on the deck is
        // the planet underneath at the station's orbital radius -- which is
        // also why a deck is landed on rather than merely parked alongside.
        struct LandingTarget {
            bool valid = false;
            Vector2d pos;
            Vector2d vel;
            double radius = 0.0;
            double gravity = 0.0;     // pull at the touchdown point, units/s^2
            flecs::entity well;       // the body a descent must not be talked out of
            std::uint32_t planetNetId = 0;
        };

        LandingTarget landingTarget;
        if (pilot.order.kind == AIOrderKind::Land && pilot.order.subject.is_alive()) {
            const flecs::entity subject = pilot.order.subject;
            const PlanetOrbitAttachment* orbit = subject.try_get<PlanetOrbitAttachment>();
            const flecs::entity wellEntity = orbit ? planetByNetId[orbit->planetNetId] : subject;

            if (const Source* well = findSource(wellEntity)) {
                const Transform& siteTransf = subject.get<Transform>();
                const double pullRadius = orbit ? orbit->radius : well->radius;
                landingTarget = LandingTarget{
                        /*valid=*/pullRadius > 0.0,
                        siteTransf.pos,
                        siteTransf.vel,
                        orbit ? LandingRadius(subject) : well->radius,
                        pullRadius > 0.0 ? PhysicsSystem::GRAVITY_CONSTANT * well->mass
                                        * gravityMultiplier / (pullRadius * pullRadius)
                                         : 0.0,
                        wellEntity,
                        SitePlanetNetId(subject)};
            }
        }

        // A live Attack order names the target outright -- the strategy layer
        // has already weighed a structure or freighter against the nearest
        // enemy fighter, and the proximity rule below would only undo that.
        const bool ordered = pilot.order.kind != AIOrderKind::None && pilot.order.subject.is_alive();
        const bool orderedAttack = ordered && pilot.order.kind == AIOrderKind::Attack;
        if (orderedAttack) {
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

            const PhysicsBody& phys = m_physicsSystem.GetBody(ref);
            pilot.guidance.accel = phys.body->GetThrust() / cpBodyGetMass(phys.cp.body.get());

            // A Patrol order circles the body it names (the faction's own
            // planet); with no order, the dominant well.
            const bool patrolOrdered = ordered && pilot.order.kind == AIOrderKind::Patrol;
            const Source* patrolBody = patrolOrdered ? findSource(pilot.order.subject) : dominant;

            // Tactical pick among Land/Intercept/Orbit/Idle; the danger check
            // below (which runs every tick, not just on this slower cadence)
            // can still override this with Evade regardless of what's picked
            // here.
            if (ordered && pilot.order.kind == AIOrderKind::Land) {
                pilot.behavior = AIBehavior::Land;
            }
            // Any live enemy is worth flying to. Range used to gate this, and
            // a pilot whose nearest enemy sat outside it went and circled a
            // rock instead -- an idle state wearing a goal's name. How far a
            // fight is worth travelling is a question for the strategy layer,
            // which weighs it against the other things a pilot could be doing
            // (AIPersonality::engageRange is that scale); down here, having
            // somewhere to be always beats patrolling.
            else if (!hurt && targetTransf) {
                pilot.behavior = AIBehavior::Intercept;
            }
            else if (patrolBody) {
                pilot.behavior = AIBehavior::Orbit;
                if (previous != AIBehavior::Orbit || pilot.patrolBody != patrolBody->entity) {
                    const Vector2d r = transf.pos - patrolBody->pos;
                    pilot.patrolBody = patrolBody->entity;
                    pilot.patrolRadius = patrolOrdered
                            ? patrolBody->radius * PATROL_RADIUS_FACTOR
                            : std::max(r.length(), evadeRadiusOf(*patrolBody) * 2.0);
                    const double cross = r.x() * transf.vel.y() - r.y() * transf.vel.x();
                    pilot.patrolDirection = (cross < 0.0) ? -1.0 : 1.0;
                }
            }
            else {
                pilot.behavior = AIBehavior::Idle;
            }
        }

        // On a pad the pilot either has business there or it leaves. Only
        // holding is decided here; a pilot with no reason to stay falls
        // through to the ladder, whose Depart rule already owns getting a
        // ship off a body it is standing on. Running the rest of that ladder
        // from the ground is what left a parked ship twitching at its own
        // landing site -- the reflexes are written for a ship in flight and
        // command a climb, a break or a pursuit that the legs then fight.
        // Where it was sent and where it is standing, both as the planet the
        // landing counts against: a High Port over the rock is that rock's
        // pad, so a ship on the deck has arrived at the site it was ordered
        // to. Comparing the site NetIds outright is what used to send a pilot
        // that had just landed on its own port back down toward the surface
        // it was already served by.
        const LandingState* landing = ent.try_get<LandingState>();
        const std::uint32_t orderedPlanetNet = landingTarget.valid ? landingTarget.planetNetId : 0;

        std::uint32_t standingOnPlanetNet = 0;
        if (landing && landing->landed) {
            const auto it = sitePlanetOf.find(landing->landedOnNetId);
            if (it != sitePlanetOf.end()) standingOnPlanetNet = it->second;
        }

        bool threatOverhead = false;
        for (const Candidate& c : candidates) {
            if (c.entity == ent || c.team == myTeam.id) continue;
            const double range = personality.groundedThreatRange;
            if ((c.pos - transf.pos).dot() < range * range) {
                threatOverhead = true;
                break;
            }
        }

        const bool onGround = landing && landing->landed;
        const bool waitingForKit = shopping && onGround
                && std::any_of(homeSites.begin(), homeSites.end(), [&](const HomeSite& site) {
                       return site.team == myTeam.id && site.hasLab
                               && site.planetNetId == standingOnPlanetNet;
                   });
        const bool holding = onGround && !threatOverhead
                && ((orderedPlanetNet != 0 && orderedPlanetNet == standingOnPlanetNet)
                    || waitingForKit);
        if (holding) {
            pilot.behavior = AIBehavior::Landed;
        }

        // The other half: leaving is a decision, not a side effect of having
        // somewhere else to be. The departure rule below starts a climb only
        // for a pilot whose objective is elsewhere, so one with no objective
        // at all -- a leader whose claim has just fired, fodder with no
        // orders -- sat on the rock for the rest of the round.
        const bool groundedWantsOut = onGround && !holding;

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
            if (pilot.fleeing && !holding) pilot.behavior = AIBehavior::Flee;
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

            // A climb already underway runs on its own hysteresis band: once
            // committed, what started it stops mattering, or a claim firing
            // mid-climb would strand the pilot halfway up.
            const bool wantsOut =
                    departing || groundedWantsOut || (objective && !objectiveHere);

            if (wantsOut && distance < clearRadius * (departing ? DEPARTURE_MARGIN : 1.0)) {
                pilot.departureSite = neighbourhood->entity;
                pilot.behavior = AIBehavior::Depart;
            }
            else if (departing) {
                // Clear of it. The behavior itself is only re-picked on the
                // decision cadence, and Depart with a dead site commands
                // nothing at all, so hand control back the same way Evade's
                // hysteresis does rather than coasting until the next one.
                pilot.departureSite = flecs::entity();
                pilot.decisionCooldown = 0;
            }
        }

        // Danger check: every tick, not gated behind decisionCooldown. A
        // pursuit/orbit path is actively thrust-driven and can curve toward a
        // well between decision points; TrajectoryPredictor only coasts
        // (gravity, no thrust -- see its class comment), so checking every
        // tick means the moment the ship's actual velocity starts curving
        // into danger, it's caught within a tick instead of up to
        // decisionInterval ticks late.
        //
        // A descent onto the site is the plan, not a hazard: LandOnBody owns
        // its own braked approach envelope, and a reflex that fires on
        // "predicted to reach the surface" fights it into a hover just short
        // of touchdown. Only the body being set down on is exempt -- anything
        // else on the way down is still a well to evade.
        const bool settingDown = pilot.behavior == AIBehavior::Land
                || pilot.behavior == AIBehavior::Landed;

        // Every body, not just the heaviest one: testing the path against a
        // single source meant that in a sector with a sun, no planet was ever
        // evaluated for danger at all, and the only thing keeping pilots off
        // planets was the departure rule.
        const Source* threat = nullptr;
        if (!sources.empty()) {
            const std::vector<Vector2d> path =
                    m_predictor.Predict(ent, personality.dangerLookaheadSteps, Game::PHYSICS_DELTA);
            for (const Vector2d& p : path) {
                for (const Source& src : sources) {
                    if (src.entity == ent) continue;
                    // The well being set down on -- or the one holding up the
                    // deck being set down on, which is the same descent.
                    if (settingDown && landingTarget.valid && src.entity == landingTarget.well) continue;

                    const double radius = evadeRadiusOf(src);
                    if ((p - src.pos).dot() < radius * radius) {
                        threat = &src;
                        break;
                    }
                }
                if (threat) break;
            }
        }

        const bool predictedDanger = threat != nullptr;

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

        if (effectiveDanger && !holding) {
            pilot.behavior = AIBehavior::Evade;
            pilot.evadeSite = threat->entity;
        }
        else if (pilot.behavior == AIBehavior::Evade) {
            // Hysteresis: don't hand control back the instant the prediction
            // clears -- wait until genuinely clear of the body being climbed
            // away from, or this would flap Evade/Intercept right at the
            // trigger boundary. Which body that is has to be held across the
            // climb (as departureSite is), since the prediction that named it
            // stops firing long before the ship is actually out.
            const Source* site = findSource(pilot.evadeSite);
            const bool clear = !site
                    || (transf.pos - site->pos).length()
                            > evadeRadiusOf(*site) * personality.evadeMargin;
            if (clear) {
                pilot.evadeSite = flecs::entity();
                pilot.decisionCooldown = 0; // re-pick a tactical behavior next tick
            }
        }

        Vector2d desiredVel = transf.vel; // Idle: no correction

        // Attitude to hold once there is no burn left to steer by; unset
        // means the desired velocity itself, which is the control layer's own
        // default.
        Vector2d coastFacing;
        bool hasCoastFacing = false;

        switch (pilot.behavior) {
            case AIBehavior::Evade:
                if (const Source* site = findSource(pilot.evadeSite)) {
                    desiredVel = EvadeBody(transf, site->pos, site->vel, pilot.guidance);
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
                    desiredVel = EvadeBody(transf, site.pos, site.vel, pilot.guidance);
                }
                break;
            case AIBehavior::Land:
                if (landingTarget.valid) {
                    const std::optional<Vector2d> entry =
                            ringEntryPoint(transf.pos, landingTarget.well, landingTarget.planetNetId,
                                           pilot.order.subject);
                    desiredVel = entry
                            ? GotoPoint(transf, *entry, pilot.guidance)
                            : LandOnBody(transf, landingTarget.pos, landingTarget.vel,
                                         landingTarget.gravity,
                                         landingTarget.radius + SHIP_LANDING_CLEARANCE,
                                         pilot.guidance);

                    // On the pad the attitude that matters is up -- legs to
                    // the ground, which is what LandingStateSystem's
                    // uprightness test asks for. Inside the flare LandOnBody
                    // asks only to match the site's own motion, so without
                    // this the control layer coasts onto that velocity
                    // instead: for an orbiting planet a ~80 unit/s vector
                    // along its orbit, i.e. a parked pilot spends the whole
                    // stay chasing an attitude taken from the pad's direction
                    // of travel rather than settling on its legs.
                    const Vector2d r = transf.pos - landingTarget.pos;
                    if (r.length() > 1e-6) {
                        coastFacing = r.normalized();
                        hasCoastFacing = true;
                    }
                }
                break;
            case AIBehavior::Landed:
                if (landingTarget.valid) {
                    desiredVel = landingTarget.vel;

                    const Vector2d r = transf.pos - landingTarget.pos;
                    if (r.length() > 1e-6) {
                        coastFacing = r.normalized();
                        hasCoastFacing = true;
                    }
                }
                break;
            case AIBehavior::Idle:
                break;
        }

        // The lead solution on this pilot's target, when it has one in range:
        // both what the gun shoots at and what the nose is flown to.
        std::optional<Vector2d> aimPoint;
        double targetRange = 0.0;
        if (pilot.behavior == AIBehavior::Intercept && targetTransf) {
            const Vector2d relPos = targetTransf->pos - transf.pos;
            targetRange = relPos.length();
            if (targetRange < personality.fireRange) {
                const Vector2d relVel = targetTransf->vel - transf.vel;
                if (std::optional<double> t = SolveInterceptTime(relPos, relVel, bulletSpeed)) {
                    aimPoint = relPos + relVel * (*t);
                }
            }
        }

        // Who owns the nose this tick. FlyToVelocity always resolves that in
        // favour of the burn, which in a dogfight means flying the whole
        // engagement broadside: at standoff the velocity correction is
        // station-keeping chatter pointing nowhere in particular, so the ship
        // pirouettes after it and the lead solution only lands inside
        // fireTolerance by luck. Inside firing range the gun wins instead --
        // up to a correction worth breaking the aim for, so an arrival still
        // flips and brakes, and a pilot pushed inside its standoff still
        // turns to open the range rather than boring in.
        const Vector2d velError = desiredVel - transf.vel;
        const bool gunHasTheNose = aimPoint.has_value()
                && targetRange > personality.standoffDistance
                && velError.length() < personality.aimPriorityError;

        ControlFlags flags = gunHasTheNose
                ? TrackBearing(transf, *aimPoint, velError, pilot.flight, &pilot.throttle)
                : FlyToVelocity(transf, desiredVel, pilot.flight, &pilot.throttle,
                                hasCoastFacing ? &coastFacing : nullptr);

        // The overburn, on the same condition whatever the pilot is doing:
        // the correction it is flying is bigger than its engine can deliver
        // in reasonable time. That is the planet it is falling toward, and it
        // is equally the hard break or hard close of a dogfight. Spending it
        // needs no cooldown bookkeeping here -- ShipControlsSystem grants a
        // burn only when there is one to grant.
        if (personality.boostVelError > 0.0 && velError.length() > personality.boostVelError) {
            flags.boost = true;
        }

        // Missiles, on their own cadence and their own (looser, longer)
        // envelope: the round homes, so it is pointed rather than aimed, and
        // it reaches targets the gun cannot. Fired at the target's actual
        // bearing, not the gun's lead solution -- leading a homing round only
        // launches it away from where it wants to go.
        if (pilot.missileCooldown > 0) --pilot.missileCooldown;
        if (pilot.missileCooldown == 0 && loadout && loadout->missileAmmo > 0 && stats.missile
            && targetTransf && personality.missileRange > 0.0) {
            const Vector2d toTarget = targetTransf->pos - transf.pos;
            const double range = toTarget.length();
            const double heading = static_cast<double>(transf.rot) - PI / 2.0;
            const double bearing = std::atan2(toTarget.y(), toTarget.x());

            if (range > 1e-6 && range < personality.missileRange
                && std::abs(WrapToPi(bearing - heading)) < personality.missileTolerance) {
                flags.fireMissile = true;
                pilot.missileCooldown = personality.missileInterval;
            }
        }

        if (pilot.fireCooldown > 0) {
            --pilot.fireCooldown;
        }
        else if (aimPoint) {
            const double aimHeading = std::atan2(aimPoint->y(), aimPoint->x());
            const double heading = static_cast<double>(transf.rot) - PI / 2.0;

            // Rolled once per firing opportunity (not every tick) and held
            // steady while waiting for an aligned shot -- a sloppy shot is
            // then a real, fixed aiming error rather than the fire threshold
            // flickering randomly tick to tick (which would look like the gun
            // spraying).
            if (personality.aimJitter > 0.0 && !pilot.aimBiasRolled) {
                pilot.aimBias = (SplitMix64NextUnit(rng) - 0.5) * 2.0 * personality.aimJitter;
                pilot.aimBiasRolled = true;
            }
            const double tolerance = personality.fireTolerance
                    + (personality.aimJitter > 0.0 ? pilot.aimBias : 0.0);

            // Weapon discipline: bullets are gravity-immune and fly straight,
            // so a body across the firing solution eats the shot. Holding fire
            // costs nothing (the cooldown is only spent on shots actually
            // taken) and stops a pilot from emptying itself into a planet the
            // target is hiding behind.
            bool blocked = false;
            for (const Source& src : sources) {
                if (src.radius <= 0.0) continue;
                if (SegmentHitsCircle(transf.pos, transf.pos + *aimPoint, src.pos,
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
