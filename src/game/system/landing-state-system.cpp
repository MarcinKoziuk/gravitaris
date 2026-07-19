#include <cmath>

#include <chipmunk/chipmunk.h>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/planet.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/landing-state.hpp>
#include <gravitaris/game/system/physics-system.hpp>
#include <gravitaris/game/system/landing-state-system.hpp>

namespace Gravitaris {

// Same tolerance as PhysicsSystem's impact uprightness: legs (local +Y) must
// point within ~35 degrees of the planet's center for the ship to count as
// standing rather than tipped against the surface.
static constexpr double UPRIGHT_DOT_THRESHOLD = 0.82;

LandingStateSystem::LandingStateSystem(flecs::world& registry, PhysicsSystem& physicsSystem)
        : m_registry(registry)
        , m_physicsSystem(physicsSystem)
{}

void LandingStateSystem::Update()
{
    m_registry.each([&](flecs::entity ship, LandingState& state, Transform& transf, PhysicsRef& ref) {
        struct Ctx {
            flecs::entity planet;
        } ctx;

        m_physicsSystem.ForEachTouching(ref, [](flecs::entity touched, void* raw) {
            auto* c = static_cast<Ctx*>(raw);
            if (!c->planet.is_alive() && touched.has<Planet>()) c->planet = touched;
        }, &ctx);

        bool landed = false;
        if (ctx.planet.is_alive()) {
            const Transform& planetTransf = ctx.planet.get<Transform>();

            const Magnum::Vector2d relVel = transf.vel - planetTransf.vel;
            const Magnum::Vector2d toCenter = (planetTransf.pos - transf.pos).normalized();
            const Magnum::Vector2d legs{-std::sin(static_cast<double>(transf.rot)),
                                        std::cos(static_cast<double>(transf.rot))};

            landed = relVel.length() < SAFE_LANDING_SPEED
                    && Magnum::Math::dot(legs, toCenter) > UPRIGHT_DOT_THRESHOLD;
        }

        if (landed) {
            state.landed = true;
            state.landedOnNetId = ctx.planet.get<NetId>().value;
            ++state.landedTicks;

            const Team* shipTeam = ship.try_get<Team>();
            const Team* planetTeam = ctx.planet.try_get<Team>();
            if (shipTeam && planetTeam && planetTeam->id == shipTeam->id) {
                state.lastFriendlySiteNetId = state.landedOnNetId;
            }
        }
        else {
            state.landed = false;
            state.landedOnNetId = 0;
            state.landedTicks = 0;
        }
    });
}

} // namespace Gravitaris
