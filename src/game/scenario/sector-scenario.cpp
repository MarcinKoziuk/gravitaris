#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/component/gravity-source.hpp>
#include <gravitaris/game/component/planet.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/util/splitmix.hpp>
#include <gravitaris/game/scenario/sector-scenario.hpp>

namespace Gravitaris {

static constexpr double PI = 3.14159265358979323846;

// Scale reference throughout is the classic arena (classic-scenario.cpp):
// two suns 22400 apart, planets on orbits of 4000..9600.

// Two stars this far apart still leave a comfortable gap between their
// outermost orbits, so no planet is ever ambiguous about which sun it
// belongs to.
static constexpr double MIN_STAR_SEPARATION = 22400.;

// The sector disc grows with the square root of the star count, so adding
// stars adds space rather than crowding: 4 stars land near the classic
// arena's own half-width.
static constexpr double SECTOR_RADIUS_PER_SQRT_STAR = 11200.;

static constexpr double MIN_ORBIT_RADIUS = 4000.;
static constexpr double ORBIT_SPACING = 2800.;
static constexpr double ORBIT_JITTER = 600.;

// How far a faction looks for room to grow, when the fairness pass asks
// whether it has any. A bit over one star's outermost orbit, so a home's
// neighbourhood is its own system plus whatever sits just beyond it.
static constexpr double EXPANSION_RADIUS = 14000.;

// Bounded so generation always terminates in the same number of steps for a
// given seed -- an unbounded "keep trying until it fits" loop would make the
// sector depend on how lucky the stream got.
static constexpr int STAR_PLACEMENT_ATTEMPTS = 64;
static constexpr int FAIRNESS_ATTEMPTS = 8;

// Each exhausted placement budget relaxes the separation rule by this much,
// so a tight sector (6 stars) degrades into a slightly closer-packed layout
// instead of failing to place a star at all.
static constexpr double SEPARATION_RELAXATION = 0.9;

namespace {

struct PlannedPlanet {
    std::size_t star = 0;
    double orbitRadius = 0.;
    double direction = 1.;
    double phase = 0.;
    Vector2d pos;
};

struct SectorPlan {
    std::vector<Vector2d> stars;
    std::vector<PlannedPlanet> planets;
    std::vector<std::size_t> homes; // indices into `planets`, in roster order
    double fairness = 0.;           // lower is fairer; see ScorePlan
};

double Distance(Vector2d a, Vector2d b)
{
    return (a - b).length();
}

// A point drawn uniformly over the sector disc (sqrt on the radius, or the
// draws bunch up in the middle).
Vector2d RandomPointInDisc(std::uint64_t& stream, double radius)
{
    const double angle = SplitMix64NextUnit(stream) * 2. * PI;
    const double r = radius * std::sqrt(SplitMix64NextUnit(stream));
    return Vector2d{std::cos(angle), std::sin(angle)} * r;
}

void PlaceStars(SectorPlan& plan, std::uint64_t& stream, int starCount)
{
    const double sectorRadius = SECTOR_RADIUS_PER_SQRT_STAR * std::sqrt(static_cast<double>(starCount));

    for (int i = 0; i < starCount; ++i) {
        double separation = MIN_STAR_SEPARATION;
        Vector2d best;
        double bestClearance = -1.;

        for (int attempt = 0; attempt < STAR_PLACEMENT_ATTEMPTS; ++attempt) {
            const Vector2d candidate = RandomPointInDisc(stream, sectorRadius);

            double clearance = sectorRadius * 2.;
            for (const Vector2d& placed : plan.stars) {
                clearance = std::min(clearance, Distance(candidate, placed));
            }
            if (clearance > bestClearance) {
                bestClearance = clearance;
                best = candidate;
            }
            if (clearance >= separation) break;

            // Relax on a schedule rather than only at the very end, so the
            // late stars of a crowded sector don't all pile onto the single
            // roomiest spot the budget happened to find.
            if ((attempt % 16) == 15) separation *= SEPARATION_RELAXATION;
        }

        plan.stars.push_back(best);
    }
}

void PlacePlanets(SectorPlan& plan, std::uint64_t& stream, const SectorParams& params)
{
    const int span = params.maxPlanetsPerStar - params.minPlanetsPerStar + 1;

    for (std::size_t star = 0; star < plan.stars.size(); ++star) {
        const int count = params.minPlanetsPerStar
                          + static_cast<int>(SplitMix64Next(stream) % static_cast<std::uint64_t>(span));

        for (int slot = 0; slot < count; ++slot) {
            PlannedPlanet planet;
            planet.star = star;
            planet.orbitRadius = MIN_ORBIT_RADIUS + ORBIT_SPACING * slot
                                 + (SplitMix64NextUnit(stream) * 2. - 1.) * ORBIT_JITTER;
            planet.direction = (SplitMix64Next(stream) & 1ull) ? 1. : -1.;
            planet.phase = SplitMix64NextUnit(stream) * 2. * PI;
            planet.pos = plan.stars[star]
                         + Vector2d{std::cos(planet.phase), std::sin(planet.phase)} * planet.orbitRadius;

            plan.planets.push_back(planet);
        }
    }
}

// Greedy max-min distance: the first home is drawn from the stream, each
// later one is whichever remaining planet sits furthest from every home
// chosen so far. Homes never share a star -- two factions on one sun would
// be fighting before either had a freighter in the air.
void PickHomes(SectorPlan& plan, std::uint64_t& stream, int factionCount)
{
    std::vector<bool> starTaken(plan.stars.size(), false);

    const std::size_t first = static_cast<std::size_t>(SplitMix64Next(stream) % plan.planets.size());
    plan.homes.push_back(first);
    starTaken[plan.planets[first].star] = true;

    while (static_cast<int>(plan.homes.size()) < factionCount) {
        std::size_t best = plan.planets.size();
        double bestDistance = -1.;

        for (std::size_t i = 0; i < plan.planets.size(); ++i) {
            if (starTaken[plan.planets[i].star]) continue;

            double nearest = -1.;
            for (std::size_t home : plan.homes) {
                const double d = Distance(plan.planets[i].pos, plan.planets[home].pos);
                if (nearest < 0. || d < nearest) nearest = d;
            }
            if (nearest > bestDistance) {
                bestDistance = nearest;
                best = i;
            }
        }

        // Sanitize() guarantees at least one star per faction, so an
        // untaken star always remains.
        plan.homes.push_back(best);
        starTaken[plan.planets[best].star] = true;
    }
}

// Fairness as a single number, lower being fairer. Two things make a start
// unfair: a faction whose nearest rival is much closer than everyone else's
// (it gets rushed), and a faction with nowhere nearby to expand into (it
// can't grow). Both are measured as a spread across factions rather than an
// absolute, since what matters is the difference between sides.
double ScorePlan(const SectorPlan& plan)
{
    double nearestMin = -1., nearestMax = 0., nearestSum = 0.;
    int roomMin = -1, roomMax = 0;

    for (std::size_t home : plan.homes) {
        const Vector2d homePos = plan.planets[home].pos;

        double nearestRival = -1.;
        for (std::size_t other : plan.homes) {
            if (other == home) continue;
            const double d = Distance(homePos, plan.planets[other].pos);
            if (nearestRival < 0. || d < nearestRival) nearestRival = d;
        }
        if (nearestMin < 0. || nearestRival < nearestMin) nearestMin = nearestRival;
        nearestMax = std::max(nearestMax, nearestRival);
        nearestSum += nearestRival;

        int room = 0;
        for (std::size_t i = 0; i < plan.planets.size(); ++i) {
            if (i == home) continue;
            if (Distance(plan.planets[i].pos, homePos) <= EXPANSION_RADIUS) ++room;
        }
        if (roomMin < 0 || room < roomMin) roomMin = room;
        roomMax = std::max(roomMax, room);
    }

    const double meanNearest = nearestSum / static_cast<double>(plan.homes.size());
    const double crowding = meanNearest > 0. ? (nearestMax - nearestMin) / meanNearest : 0.;
    const double roomGap = static_cast<double>(roomMax - roomMin);

    // Room is counted in whole planets, so one planet of difference already
    // matters; the halving keeps it from swamping the crowding term.
    return crowding + 0.5 * roomGap;
}

SectorPlan PlanSector(std::uint64_t& stream, const SectorParams& params)
{
    SectorPlan best;
    bool haveBest = false;

    for (int attempt = 0; attempt < FAIRNESS_ATTEMPTS; ++attempt) {
        SectorPlan plan;
        PlaceStars(plan, stream, params.stars);
        PlacePlanets(plan, stream, params);
        PickHomes(plan, stream, params.factionCount);
        plan.fairness = ScorePlan(plan);

        if (!haveBest || plan.fairness < best.fairness) {
            best = std::move(plan);
            haveBest = true;
        }
    }

    return best;
}

} // namespace

void SectorParams::Sanitize()
{
    factionCount = std::clamp(factionCount, MIN_FACTIONS, MAX_FACTIONS);
    stars = std::clamp(stars, MIN_STARS, MAX_STARS);
    // One star per faction at minimum: PickHomes never puts two homes on the
    // same sun.
    stars = std::max(stars, factionCount);

    minPlanetsPerStar = std::max(1, minPlanetsPerStar);
    maxPlanetsPerStar = std::max(minPlanetsPerStar, maxPlanetsPerStar);
}

SectorLayout BuildSectorScenario(EntitySpawner& entitySpawner, const SectorParams& rawParams)
{
    SectorParams params = rawParams;
    params.Sanitize();

    std::uint64_t stream = SplitMix64Seed(params.seed, 0x5EC70Bull);
    const SectorPlan plan = PlanSector(stream, params);

    const id_t sunModel = "models/stars/sun"_id;
    const id_t planetModel = "models/planets/simple"_id;

    // Same idiom as classic-scenario.cpp: OrbitSystem derives angular speed
    // from the star's effective attracting mass, which is its own gravity
    // multiplier applied to its mass.
    const auto effectiveMass = [](flecs::entity star) {
        return star.get<GravitySource>().mass * star.get<GravitySource>().multiplier;
    };

    std::vector<double> starMass;
    starMass.reserve(plan.stars.size());
    for (const Vector2d& pos : plan.stars) {
        starMass.push_back(effectiveMass(entitySpawner.SpawnStar(sunModel, pos)));
    }

    SectorLayout layout;
    std::vector<flecs::entity> spawned;
    spawned.reserve(plan.planets.size());

    for (const PlannedPlanet& planet : plan.planets) {
        flecs::entity entity = entitySpawner.SpawnOrbitingPlanet(
                planetModel, plan.stars[planet.star], starMass[planet.star], planet.orbitRadius,
                planet.direction, planet.phase);
        spawned.push_back(entity);

        // The furthest this body ever gets from the origin is its star's
        // distance plus the full orbit, so the extent holds for every tick
        // rather than just this one.
        const double reach = plan.stars[planet.star].length() + planet.orbitRadius
                             + entity.get<Planet>().radius;
        layout.extent = std::max(layout.extent, reach);
    }

    layout.homes.reserve(plan.homes.size());
    for (std::size_t home : plan.homes) {
        layout.homes.push_back(spawned[home]);
    }

    return layout;
}

} // namespace Gravitaris
