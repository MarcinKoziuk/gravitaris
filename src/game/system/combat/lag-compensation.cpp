#include <algorithm>

#include <chipmunk/chipmunk.h>

#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/system/core/physics-system.hpp>
#include <gravitaris/game/system/combat/lag-compensation.hpp>

namespace Gravitaris {

// How often the table is swept for hulls that have died. Anything not written
// for a whole ring's worth of ticks cannot be rewound to anyway, so nothing is
// lost by dropping it -- and doing this every tick would walk the whole map to
// find nothing on all but one of them.
static constexpr std::uint64_t PRUNE_INTERVAL_TICKS = 256;

LagCompensation::LagCompensation(flecs::world& registry, PhysicsSystem& physicsSystem)
        : m_registry(registry)
        , m_physicsSystem(physicsSystem)
{}

void LagCompensation::Record(std::uint64_t step)
{
    // Only what a shot can be resolved against. A planet keeps no trail: it is
    // on a rail both sides re-derive from the same tick anyway, and nothing
    // aims at one.
    m_registry.each([&](flecs::entity entity, const Transform& transf, const Damageable&) {
        Trail& trail = m_trails[entity.id()];
        trail.samples[step % Trail::CAPACITY] =
                Trail::Sample{transf.pos, static_cast<double>(transf.rot)};
        trail.newest = step;
        trail.filled = std::min<std::uint32_t>(trail.filled + 1, Trail::CAPACITY);
    });

    if (step % PRUNE_INTERVAL_TICKS != 0) return;

    for (auto it = m_trails.begin(); it != m_trails.end();) {
        it = step - it->second.newest > Trail::CAPACITY ? m_trails.erase(it) : std::next(it);
    }
}

std::uint64_t LagCompensation::ViewTickOf(std::uint64_t step, std::uint16_t viewDelay)
{
    const std::uint64_t delay = std::min<std::uint64_t>(viewDelay, MAX_REWIND_TICKS);
    return delay >= step ? 0 : step - delay;
}

const LagCompensation::Trail::Sample* LagCompensation::SampleAt(flecs::entity_t entity,
                                                                std::uint64_t tick) const
{
    const auto found = m_trails.find(entity);
    if (found == m_trails.end()) return nullptr;

    const Trail& trail = found->second;
    if (trail.filled == 0 || tick > trail.newest) return nullptr;
    if (trail.newest - tick >= trail.filled) return nullptr; // spawned since

    return &trail.samples[tick % Trail::CAPACITY];
}

LagCompensation::Rewind::Rewind(LagCompensation& owner, std::uint64_t viewTick, flecs::entity shooter)
        : m_owner(owner)
        , m_moved(std::move(owner.m_scratch))
{
    m_moved.clear();

    owner.m_registry.each([&](flecs::entity entity, Transform& transf, const Damageable&,
                              const PhysicsRef& ref) {
        // Never the shooter: its own ship is predicted on its own machine, so
        // it is already where the pilot thinks it is. Dragging it back would
        // move the muzzle the shot leaves from.
        if (entity == shooter) return;

        const Trail::Sample* was = owner.SampleAt(entity.id(), viewTick);
        if (!was) return;
        if (was->pos == transf.pos && static_cast<double>(transf.rot) == was->rot) return;

        m_moved.push_back(Displaced{entity, transf.pos, static_cast<double>(transf.rot)});

        PhysicsBody& slot = owner.m_physicsSystem.GetBody(ref);
        cpBody* body = slot.cp.body.get();
        cpBodySetPosition(body, cpv(was->pos.x(), was->pos.y()));
        cpBodySetAngle(body, cpFloat(was->rot));
        // Chipmunk only refreshes a shape's bounding box as part of a step, and
        // a query walks the index rather than the shapes -- without this the
        // hull is moved and every sweep still meets it at yesterday's address.
        cpSpaceReindexShapesForBody(slot.cp.space.get(), body);

        // The queries read the physics bodies, but everything else that resolves
        // a shot reads Transform (a beam's muzzle, the interception corridor),
        // so the two have to agree for as long as the world is held back.
        transf.pos = was->pos;
        transf.rot = Radd{was->rot};
    });
}

LagCompensation::Rewind::~Rewind()
{
    for (const Displaced& displaced : m_moved) {
        // A hull that died inside the rewind stays dead where it was hit; there
        // is nothing left to put back.
        if (!displaced.entity.is_alive()) continue;

        Transform& transf = displaced.entity.get_mut<Transform>();
        transf.pos = displaced.pos;
        transf.rot = Radd{displaced.rot};

        const PhysicsRef* ref = displaced.entity.try_get<PhysicsRef>();
        if (!ref) continue;

        PhysicsBody& slot = m_owner.m_physicsSystem.GetBody(*ref);
        cpBody* body = slot.cp.body.get();
        cpBodySetPosition(body, cpv(displaced.pos.x(), displaced.pos.y()));
        cpBodySetAngle(body, cpFloat(displaced.rot));
        cpSpaceReindexShapesForBody(slot.cp.space.get(), body);
    }

    // Hand the buffer back rather than freeing it: one rewind per burning
    // shooter per tick is a lot of identical allocations otherwise.
    m_moved.clear();
    m_owner.m_scratch = std::move(m_moved);
}

} // namespace Gravitaris
