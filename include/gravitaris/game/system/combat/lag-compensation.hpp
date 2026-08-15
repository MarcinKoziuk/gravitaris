#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <flecs.h>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// What a shooter aimed at is never what the server is looking at. A client sees
// everyone else through an interpolation delay on top of its own latency, so by
// the time its trigger reaches here the target has moved on -- and a shot that
// was dead on when it was fired misses for reasons the pilot cannot see or
// answer. Every hitscan shooter since Quake III has fixed that the same way:
// keep a little of the past, and resolve a shot in the world the pilot saw.
//
// So each command carries how far behind the client's view was
// (Controls::viewDelay), every hull that can be shot leaves a trail of where it
// has been, and a Rewind puts the world back to that tick for exactly as long
// as it takes to resolve one shooter's shot.
//
// The shooter itself is never rewound: its own ship is predicted locally, so it
// is drawn at the tick it is shooting from, not at the tick it sees others at.
class LagCompensation {
private:
    // Where one hull has been, tick by tick, for the last half second or so.
    //
    // Deliberately NOT an ECS component, which is what this was first: giving
    // every damageable entity another component changes which table it lives
    // in, and that changes the order every unrelated query walks the world in.
    // Float accumulation is not associative, so the sim produced *different
    // physics* -- caught by a landing test that had nothing to do with any of
    // this. A side table costs a hash lookup per hull per tick and moves
    // nothing.
    struct Trail {
        // A power of two so the modulo is a mask, and comfortably more than
        // MAX_REWIND_TICKS -- the surplus is what stops a rewind at the limit
        // from reading the sample being overwritten this very tick.
        static constexpr std::size_t CAPACITY = 64;

        struct Sample {
            Magnum::Math::Vector2<double> pos;
            double rot = 0.;
        };

        std::array<Sample, CAPACITY> samples{};
        std::uint64_t newest = 0;
        // Ticks written, saturating at CAPACITY: a hull that spawned four ticks
        // ago cannot be rewound eight, and a ring cannot tell an unwritten slot
        // from a written one on its own.
        std::uint32_t filled = 0;
    };

    // One hull moved out of the present, and where to put it back.
    struct Displaced {
        flecs::entity entity;
        Magnum::Math::Vector2<double> pos;
        double rot = 0.;
    };

public:
    // The furthest back a client may drag the world, in ticks. Half a second at
    // 60Hz: past that, a pilot on a bad line would be shooting at ghosts far
    // enough behind that everyone else's dodging stops meaning anything, which
    // is the trade every game with this in it has to pick a number for.
    static constexpr std::uint16_t MAX_REWIND_TICKS = 30;

    LagCompensation(flecs::world& registry, PhysicsSystem& physicsSystem);

    // One sample per hull that can be shot, taken after the step has moved
    // everything -- so a trail holds what the snapshot for this tick will
    // carry, which is the whole point: the client saw exactly those positions.
    void Record(std::uint64_t step);

    // The world as `viewTick` left it, for as long as this object lives, with
    // `shooter` left where it really is. Restores every body it moved on the way
    // out, whatever happens in between.
    //
    // Moving the real bodies rather than resolving against stored numbers is
    // deliberate: a beam is swept against the physics space, through the same
    // polygons, shield elements and filters as any other shot, so nothing about
    // the hit rules has to be written twice and drift.
    class Rewind {
    public:
        Rewind(LagCompensation& owner, std::uint64_t viewTick, flecs::entity shooter);
        ~Rewind();

        Rewind(const Rewind&) = delete;
        Rewind& operator=(const Rewind&) = delete;

        // False when nothing was moved -- no delay asked for, or nothing has a
        // trail reaching that far back. The caller resolves the shot either
        // way; this is only worth testing to skip work.
        [[nodiscard]] bool Moved() const { return !m_moved.empty(); }

    private:
        LagCompensation& m_owner;
        std::vector<Displaced> m_moved;
    };

    // The tick a command composed `viewDelay` ticks behind `step` was looking
    // at, clamped to what may be asked for. Zero delay answers `step` itself,
    // which is every single-player shot and every AI one.
    [[nodiscard]] static std::uint64_t ViewTickOf(std::uint64_t step, std::uint16_t viewDelay);

private:
    flecs::world& m_registry;
    PhysicsSystem& m_physicsSystem;

    ankerl::unordered_dense::map<flecs::entity_t, Trail> m_trails;

    // Scratch for the walk a Rewind does, kept here so a shot resolved every
    // tick by every laser in the sector is not an allocation each.
    std::vector<Displaced> m_scratch;

    // Where `entity` was at `tick`, or null if its trail does not reach back
    // that far (a hull that has spawned since).
    [[nodiscard]] const Trail::Sample* SampleAt(flecs::entity_t entity, std::uint64_t tick) const;
};

} // namespace Gravitaris
