#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>

#include <gravitaris/game/component/ai-pilot.hpp>
#include <gravitaris/game/component/ai-strategy.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/freighter.hpp>
#include <gravitaris/game/component/gravity-source.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/orbit.hpp>
#include <gravitaris/game/component/planet.hpp>
#include <gravitaris/game/component/planet-attachment.hpp>
#include <gravitaris/game/component/structure.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/system/core/physics-system.hpp>
#include <gravitaris/game/system/ship/ai-strategy-system.hpp>

namespace Gravitaris {

using Magnum::Vector2d;

// Distance at which a proximity term is worth half its weight -- how far a
// leader is willing to travel for each kind of objective. Expansion targets
// (claim/attack) are worth a trip across a star system; a raid or a defense
// scramble is a local decision.
static constexpr double EXPANSION_SCALE = 6000.0;
static constexpr double RAID_SCALE = 3000.0;
static constexpr double THREAT_SCALE = 1500.0;

// Complex hp at which AttackComplex's weakness term is worth half its
// weight; roughly one full structure.
static constexpr double COMPLEX_REFERENCE_HP = 100.0;

// A rival goal must beat the incumbent by this much to take over, so a
// leader commits to something instead of oscillating between near-ties.
static constexpr double GOAL_SWITCH_MARGIN = 1.2;

// Crowding falloff: a subject already spoken for by n other leaders scores
// CROWDING_SHARE/(CROWDING_SHARE + n) of its worth, so the second leader to
// look at a planet halves its appeal rather than duplicating the trip.
static constexpr double CROWDING_SHARE = 1.0;

// Margin a hull's thrust acceleration must clear a body's surface gravity by
// before landing there is an option at all: below 1 the ship cannot lift off
// again, and right at 1 it cannot lift off in any useful time.
static constexpr double LIFTOFF_THRUST_MARGIN = 1.15;

// Upper bound on how long a claim is held without re-scoring (60s at the
// fixed tick) -- long enough for a descent from across a star system.
static constexpr std::uint32_t CLAIM_COMMIT_TICKS = 3600;

namespace {

struct PlanetInfo {
    flecs::entity entity;
    std::uint32_t netId = 0;
    Vector2d pos;
    TeamId team = TeamId::None;
    bool claimable = false; // suns have no Orbit and cannot be landed on
    double surfaceGravity = 0.0;
    int structureCount = 0;
    double totalHp = 0.0;
    flecs::entity weakestStructure;
    double weakestHp = std::numeric_limits<double>::max();
};

struct ShipInfo {
    flecs::entity entity;
    std::uint32_t netId = 0;
    Vector2d pos;
    TeamId team = TeamId::None;
    bool freighter = false;
};

// Falls from 1 at zero distance to 0.5 at `scale`. Keeps every goal's
// distance term on the same 0..1 footing as its quality term, so weights
// compare across goals.
double Proximity(double dist, double scale)
{
    return scale / (scale + dist);
}

AIOrder OrderFor(AIGoal goal, flecs::entity subject)
{
    switch (goal) {
        case AIGoal::ClaimPlanet:        return AIOrder{AIOrderKind::Land, subject};
        case AIGoal::AttackComplex:      return AIOrder{AIOrderKind::Attack, subject};
        case AIGoal::InterceptFreighter: return AIOrder{AIOrderKind::Attack, subject};
        case AIGoal::DefendComplex:      return AIOrder{AIOrderKind::Patrol, subject};
        case AIGoal::Dogfight:           break;
    }
    return AIOrder{};
}

} // namespace

AIStrategySystem::AIStrategySystem(flecs::world& registry, PhysicsSystem& physicsSystem)
        : m_registry(registry)
        , m_physicsSystem(physicsSystem)
{}

void AIStrategySystem::Update()
{
    // Both candidate lists are sorted by NetId before any scoring reads
    // them: flecs iteration order is not guaranteed stable, and ties between
    // equally-scored targets have to break the same way every run (ADR 0001).
    std::vector<PlanetInfo> planets;
    ankerl::unordered_dense::map<std::uint32_t, std::size_t> planetByNetId;

    const double gravityMultiplier = m_physicsSystem.GetGravityMultiplier();
    m_registry.each([&](flecs::entity ent, const Transform& transf, const Planet& planet,
                        const Team& team, const NetId& netId) {
        double surfaceGravity = 0.0;
        const double radius = planet.radius * transf.scale.x();
        if (const GravitySource* gs = ent.try_get<GravitySource>(); gs && radius > 0.0) {
            surfaceGravity = PhysicsSystem::GRAVITY_CONSTANT * gs->mass * gs->multiplier
                    * gravityMultiplier / (radius * radius);
        }
        planets.push_back(PlanetInfo{ent, netId.value, transf.pos, team.id, ent.has<Orbit>(),
                                     surfaceGravity});
    });
    std::sort(planets.begin(), planets.end(),
              [](const PlanetInfo& a, const PlanetInfo& b) { return a.netId < b.netId; });
    for (std::size_t i = 0; i < planets.size(); ++i) {
        planetByNetId.emplace(planets[i].netId, i);
    }

    const auto addStructure = [&](flecs::entity ent, const Damageable& hp, std::uint32_t planetNetId) {
        const auto it = planetByNetId.find(planetNetId);
        if (it == planetByNetId.end()) return;
        PlanetInfo& planet = planets[it->second];
        ++planet.structureCount;
        planet.totalHp += hp.hp;
        if (hp.hp < planet.weakestHp) {
            planet.weakestHp = hp.hp;
            planet.weakestStructure = ent;
        }
    };
    m_registry.each([&](flecs::entity ent, const Structure&, const Damageable& hp,
                        const PlanetSurfaceAttachment& attach) {
        addStructure(ent, hp, attach.planetNetId);
    });
    m_registry.each([&](flecs::entity ent, const Structure&, const Damageable& hp,
                        const PlanetOrbitAttachment& attach) {
        addStructure(ent, hp, attach.planetNetId);
    });

    std::vector<ShipInfo> ships;
    m_registry.each([&](flecs::entity ent, const Transform& transf, const Team& team, const Damageable&,
                        const NetId& netId) {
        if (ent.has<Structure>() || ent.has<Planet>()) return;
        ships.push_back(ShipInfo{ent, netId.value, transf.pos, team.id, ent.has<Freighter>()});
    });
    std::sort(ships.begin(), ships.end(),
              [](const ShipInfo& a, const ShipInfo& b) { return a.netId < b.netId; });

    // Who is already spoken for. Dogfights are exempt: several ships
    // converging on one enemy fighter is a fine outcome, unlike two of them
    // descending on the same rock.
    ankerl::unordered_dense::map<std::uint64_t, int> takers;
    m_registry.each([&](flecs::entity, const AIStrategy& strategy) {
        if (strategy.goal == AIGoal::Dogfight || !strategy.subject.is_alive()) return;
        ++takers[strategy.subject.id()];
    });

    // Leaders decide in NetId order, and each one's pick lands in `takers`
    // before the next reads it -- pilots all spawn with a zero decision
    // cooldown, so a whole wing's first decision happens on one tick, and a
    // snapshot taken before the pass would let every one of them claim the
    // same planet. Ordering the pass is what keeps that crowding-aware
    // sequence the same on every run (ADR 0001).
    std::vector<std::pair<std::uint32_t, flecs::entity>> leaders;
    m_registry.each([&](flecs::entity ent, const AIStrategy&, const NetId& netId) {
        leaders.emplace_back(netId.value, ent);
    });
    std::sort(leaders.begin(), leaders.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    for (const auto& entry : leaders) {
        const flecs::entity self = entry.second;
        const Transform* selfTransf = self.try_get<Transform>();
        const Team* selfTeam = self.try_get<Team>();
        AIPilot* selfPilot = self.try_get_mut<AIPilot>();
        if (!selfTransf || !selfTeam || !selfPilot) continue;

        const Transform& transf = *selfTransf;
        const Team& myTeam = *selfTeam;
        AIPilot& pilot = *selfPilot;
        AIStrategy& strategy = self.get_mut<AIStrategy>();
        if (strategy.commitCooldown > 0) --strategy.commitCooldown;

        // Hold a claim through its descent; anyone taking the planet in the
        // meantime (us included) releases it early.
        if (strategy.goal == AIGoal::ClaimPlanet && strategy.commitCooldown > 0
            && strategy.subject.is_alive()) {
            const Team* subjectTeam = strategy.subject.try_get<Team>();
            if (subjectTeam && subjectTeam->id == TeamId::None) continue;
        }

        const bool subjectLost = strategy.goal != AIGoal::Dogfight && !strategy.subject.is_alive();
        if (strategy.decisionCooldown > 0 && !subjectLost) {
            --strategy.decisionCooldown;
            continue;
        }
        strategy.decisionCooldown = strategy.decisionInterval;

        const AIStrategyWeights& weights = strategy.weights;

        struct Choice {
            AIGoal goal = AIGoal::Dogfight;
            flecs::entity subject;
            double score = 0.0;
        };
        Choice best;
        double incumbentScore = 0.0;
        const auto consider = [&](AIGoal goal, flecs::entity subject, double score) {
            if (goal != AIGoal::Dogfight) {
                const auto it = takers.find(subject.id());
                int others = it != takers.end() ? it->second : 0;
                // This leader's own claim is not competition for itself --
                // discounting it would make every incumbent goal look worse
                // than the alternatives on every re-score.
                if (subject == strategy.subject && strategy.goal != AIGoal::Dogfight) --others;
                score *= CROWDING_SHARE / (CROWDING_SHARE + others);
            }

            if (goal == strategy.goal && subject == strategy.subject) incumbentScore = score;
            if (score > best.score) best = Choice{goal, subject, score};
        };

        const auto nearestShip = [&](Vector2d from, bool wantFreighter) {
            const ShipInfo* found = nullptr;
            double bestDistSq = std::numeric_limits<double>::max();
            for (const ShipInfo& ship : ships) {
                if (ship.entity == self || ship.team == myTeam.id) continue;
                if (ship.freighter != wantFreighter) continue;
                const double distSq = (ship.pos - from).dot();
                if (distSq < bestDistSq) {
                    bestDistSq = distSq;
                    found = &ship;
                }
            }
            return found;
        };

        if (const ShipInfo* enemy = nearestShip(transf.pos, /*wantFreighter=*/false)) {
            consider(AIGoal::Dogfight, enemy->entity,
                     weights.dogfight * Proximity((enemy->pos - transf.pos).length(), RAID_SCALE));
        }
        if (const ShipInfo* freighter = nearestShip(transf.pos, /*wantFreighter=*/true)) {
            consider(AIGoal::InterceptFreighter, freighter->entity,
                     weights.interceptFreighter
                             * Proximity((freighter->pos - transf.pos).length(), RAID_SCALE));
        }

        for (const PlanetInfo& planet : planets) {
            if (!planet.claimable) continue;
            const double dist = (planet.pos - transf.pos).length();

            if (planet.team == TeamId::None && planet.structureCount == 0) {
                // A claim this hull could never lift off from again is not a
                // claim, it is a ship parked forever -- LandOnBody's own
                // braking floor would otherwise fly the descent regardless.
                if (planet.surfaceGravity * LIFTOFF_THRUST_MARGIN < pilot.guidance.accel) {
                    consider(AIGoal::ClaimPlanet, planet.entity,
                             weights.claim * Proximity(dist, EXPANSION_SCALE));
                }
            }
            else if (planet.team != myTeam.id && planet.structureCount > 0) {
                // Attacking the weakest complex is the point of the goal:
                // ConquestSystem will not flip a planet while an enemy
                // structure still stands on it.
                const double weakness = COMPLEX_REFERENCE_HP / (COMPLEX_REFERENCE_HP + planet.totalHp);
                consider(AIGoal::AttackComplex, planet.weakestStructure,
                         weights.attackComplex * Proximity(dist, EXPANSION_SCALE) * weakness);
            }
            else if (planet.team == myTeam.id && planet.structureCount > 0) {
                const ShipInfo* threat = nearestShip(planet.pos, /*wantFreighter=*/false);
                if (!threat) continue;
                consider(AIGoal::DefendComplex, planet.entity,
                         weights.defend * Proximity((threat->pos - planet.pos).length(), THREAT_SCALE)
                                 * Proximity(dist, EXPANSION_SCALE));
            }
        }

        const bool switching = best.goal != strategy.goal || best.subject != strategy.subject;
        if (switching && best.score < incumbentScore * GOAL_SWITCH_MARGIN) continue;

        if (switching) {
            if (strategy.goal != AIGoal::Dogfight && strategy.subject.is_alive()) {
                const auto it = takers.find(strategy.subject.id());
                if (it != takers.end() && --it->second <= 0) takers.erase(it);
            }
            if (best.goal != AIGoal::Dogfight && best.subject.is_alive()) {
                ++takers[best.subject.id()];
            }
        }

        strategy.goal = best.goal;
        strategy.subject = best.subject;
        strategy.commitCooldown = best.goal == AIGoal::ClaimPlanet ? CLAIM_COMMIT_TICKS : 0;
        pilot.order = OrderFor(best.goal, best.subject);
    }
}

} // namespace Gravitaris
