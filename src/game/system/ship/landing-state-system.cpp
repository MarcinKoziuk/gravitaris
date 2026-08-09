#include <cmath>
#include <utility>
#include <vector>

#include <chipmunk/chipmunk.h>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/planet.hpp>
#include <gravitaris/game/component/structure.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/landing-state.hpp>
#include <gravitaris/game/component/faction-state.hpp>
#include <gravitaris/game/system/gwell/faction-system.hpp>
#include <gravitaris/game/system/core/physics-system.hpp>
#include <gravitaris/game/system/ship/landing-state-system.hpp>

namespace Gravitaris {

// Same tolerance as PhysicsSystem's impact uprightness: legs (local +Y) must
// point within ~35 degrees of the planet's center for the ship to count as
// standing rather than tipped against the surface.
static constexpr double UPRIGHT_DOT_THRESHOLD = 0.82;

LandingStateSystem::LandingStateSystem(flecs::world& registry, PhysicsSystem& physicsSystem,
                                       FactionSystem& factionSystem)
        : m_registry(registry)
        , m_physicsSystem(physicsSystem)
        , m_factionSystem(factionSystem)
{}

void LandingStateSystem::Update()
{
    // Collected here, applied after the .each() below completes:
    // FactionSystem::GetOrCreate can create an entity, a structural change
    // flecs doesn't allow safely from inside an active iterator (observed
    // as an intermittent crash) -- so no FactionSystem call can happen
    // inside this loop itself.
    std::vector<std::pair<TeamId, std::uint32_t>> friendlyLandings;

    m_registry.each([&](flecs::entity ship, LandingState& state, Transform& transf, PhysicsRef& ref) {
        // A High Port is a landing site too, judged as a deck rather than as a
        // surface (see below). Planet wins if a ship somehow touches both.
        struct Ctx {
            flecs::entity planet;
            flecs::entity station;
        } ctx;

        m_physicsSystem.ForEachTouching(ref, [](flecs::entity touched, void* raw) {
            auto* c = static_cast<Ctx*>(raw);
            if (!c->planet.is_alive() && touched.has<Planet>()) c->planet = touched;
            if (!c->station.is_alive()) {
                const Structure* structure = touched.try_get<Structure>();
                if (structure && structure->type == StructureType::HighPort) c->station = touched;
            }
        }, &ctx);

        const bool onDeck = !ctx.planet.is_alive() && ctx.station.is_alive();
        const flecs::entity site = ctx.planet.is_alive() ? ctx.planet : ctx.station;

        bool landed = false;
        if (site.is_alive()) {
            const Transform& siteTransf = site.get<Transform>();

            const Magnum::Vector2d relVel = transf.vel - siteTransf.vel;
            landed = relVel.length() < SAFE_LANDING_SPEED;

            // Uprightness is a question about a surface: a slope will hold a
            // hull on its side, and that is not a landing. A deck will not --
            // it is a pad, and a ship in contact with one and matched to its
            // motion is parked whatever way it is pointing. Asking a station
            // for uprightness also asked the pilot to keep turning with a ring
            // that goes round once a minute, which is why standing on a High
            // Port would come and go as somewhere you could refit.
            if (!onDeck) {
                const Magnum::Vector2d toCenter = (siteTransf.pos - transf.pos).normalized();
                const Magnum::Vector2d legs{-std::sin(static_cast<double>(transf.rot)),
                                            std::cos(static_cast<double>(transf.rot))};
                landed = landed && Magnum::Math::dot(legs, toCenter) > UPRIGHT_DOT_THRESHOLD;
            }
        }

        if (landed) {
            state.landed = true;
            state.landedOnNetId = site.get<NetId>().value;
            ++state.landedTicks;

            const Team* shipTeam = ship.try_get<Team>();
            const Team* siteTeam = site.try_get<Team>();
            if (shipTeam && siteTeam && siteTeam->id == shipTeam->id) {
                state.lastFriendlySiteNetId = state.landedOnNetId;
                friendlyLandings.emplace_back(shipTeam->id, state.landedOnNetId);
            }
        }
        else {
            state.landed = false;
            state.landedOnNetId = 0;
            state.landedTicks = 0;
        }
    });

    for (const auto& [team, landedOnNetId] : friendlyLandings) {
        flecs::entity factionState = m_factionSystem.GetOrCreate(team);
        factionState.get_mut<FactionState>().lastLandingSiteNetId = landedOnNetId;
    }
}

} // namespace Gravitaris
