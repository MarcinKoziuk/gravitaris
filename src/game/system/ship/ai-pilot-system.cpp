#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

#include <ankerl/unordered_dense.h>

#include <gravitaris/gravitaris.hpp>
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

    // Somewhere to come home to, and whether anything is still being built
    // there worth coming home for. Both are per-team facts, so they're read
    // once here rather than per pilot; NetId order keeps the nearest-home tie
    // break the same on every run (ADR 0001).
    struct HomePlanet {
        flecs::entity entity;
        Vector2d pos;
        TeamId team;
        std::uint32_t netId;
        // Hosts one of this faction's labs. An upgrade can only be collected
        // at the planet that finished it (ResearchSystem's collector rule), so
        // a shopping trip that lands anywhere else is a wasted descent.
        bool hasLab = false;
    };
    std::vector<HomePlanet> homePlanets;

    ankerl::unordered_dense::set<std::uint32_t> labPlanets;
    std::array<bool, NUM_TEAMS> teamHasLab{};
    m_registry.each([&](const Structure& s, const Team& team, const PlanetSurfaceAttachment& attach) {
        if (s.type != StructureType::Lab || team.id == TeamId::None) return;
        teamHasLab[static_cast<std::size_t>(team.id)] = true;
        labPlanets.insert(attach.planetNetId);
    });

    // Whose upgrade is finished and waiting for somebody to come and get it.
    std::array<bool, NUM_TEAMS> teamUpgradeReady{};
    m_registry.each([&](const FactionState& fs) {
        teamUpgradeReady[static_cast<std::size_t>(fs.team)] = fs.upgradeReady;
    });

    m_registry.each([&](flecs::entity ent, const Planet&, const Transform& transf, const Team& team,
                        const NetId& netId) {
        if (IsHomePlanet(m_registry, ent, team.id)) {
            homePlanets.push_back({ent, transf.pos, team.id, netId.value,
                                   labPlanets.contains(netId.value)});
        }
    });
    std::sort(homePlanets.begin(), homePlanets.end(),
              [](const HomePlanet& a, const HomePlanet& b) { return a.netId < b.netId; });

    // `wantLab` picks the nearest home hosting a lab, falling back to the
    // nearest home of any kind -- a pilot flying back for an upgrade has to
    // reach the building that has it, but a hurt one just wants ground.
    const auto nearestHome = [&homePlanets](TeamId team, const Vector2d& from, bool wantLab) {
        flecs::entity best;
        double bestDistSq = std::numeric_limits<double>::max();
        for (const HomePlanet& home : homePlanets) {
            if (home.team != team) continue;
            if (wantLab && !home.hasLab) continue;
            const double distSq = (home.pos - from).dot();
            if (distSq < bestDistSq) {
                bestDistSq = distSq;
                best = home.entity;
            }
        }
        return best;
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

        // Shopping, for either of two reasons: something is finished and
        // waiting to be collected right now, or this pilot is holding its pad
        // for one its labs are still working on (AIPersonality::upgradeGreed,
        // bounded by padWaitRemaining so a wing never parks forever). With no
        // labs left nothing is coming either way.
        if (pilot.padWaitRemaining > 0) --pilot.padWaitRemaining;
        const std::size_t myTeamIndex = static_cast<std::size_t>(myTeam.id);
        const bool shopping = teamHasLab[myTeamIndex]
                && (teamUpgradeReady[myTeamIndex]
                    || (pilot.upgradesWanted > 0 && pilot.padWaitRemaining > 0));

        // A pilot with no strategy of its own flies its leader's objective,
        // except when its own hull or its own shopping list says otherwise --
        // the two things no one else can decide for it. (A leader gets the
        // same trip from AIStrategySystem, as AIGoal::Rearm.)
        if (!ent.has<AIStrategy>()) {
            flecs::entity home;
            if (hurt || shopping) {
                // A shopping trip wants the lab; anything else just wants
                // friendly ground, and takes the lab planet only if it is
                // also the nearest.
                if (shopping) home = nearestHome(myTeam.id, transf.pos, /*wantLab=*/true);
                if (!home.is_alive()) home = nearestHome(myTeam.id, transf.pos, /*wantLab=*/false);
            }
            if (home.is_alive()) {
                pilot.order = AIOrder{AIOrderKind::Land, home};
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
        const LandingState* landing = ent.try_get<LandingState>();
        const NetId* siteNet = ordered && pilot.order.kind == AIOrderKind::Land
                        && pilot.order.subject.is_alive()
                ? pilot.order.subject.try_get<NetId>()
                : nullptr;

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
                && std::any_of(homePlanets.begin(), homePlanets.end(), [&](const HomePlanet& home) {
                       return home.team == myTeam.id && home.netId == landing->landedOnNetId;
                   });
        const bool holding = onGround && !threatOverhead
                && ((siteNet && siteNet->value == landing->landedOnNetId) || waitingForKit);
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
                    if (settingDown && src.entity == pilot.order.subject) continue;

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
                if (const Source* site = ordered ? findSource(pilot.order.subject) : nullptr) {
                    desiredVel = LandOnBody(transf, site->pos, site->vel, site->mass * gravityMultiplier,
                                            site->radius + SHIP_LANDING_CLEARANCE, pilot.guidance);

                    // On the pad the attitude that matters is up -- legs to
                    // the ground, which is what LandingStateSystem's
                    // uprightness test asks for. Inside the flare LandOnBody
                    // asks only to match the site's own motion, so without
                    // this the control layer coasts onto that velocity
                    // instead: for an orbiting planet a ~80 unit/s vector
                    // along its orbit, i.e. a parked pilot spends the whole
                    // stay chasing an attitude taken from the pad's direction
                    // of travel rather than settling on its legs.
                    const Vector2d r = transf.pos - site->pos;
                    if (r.length() > 1e-6) {
                        coastFacing = r.normalized();
                        hasCoastFacing = true;
                    }
                }
                break;
            case AIBehavior::Landed:
                if (const Source* site = findSource(pilot.order.subject)) {
                    desiredVel = site->vel;

                    const Vector2d r = transf.pos - site->pos;
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
