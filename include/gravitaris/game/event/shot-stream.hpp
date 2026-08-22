#pragma once

#include <array>
#include <cstdint>

#include <Magnum/Magnum.h>
#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/component/team.hpp>

namespace Gravitaris {

// A round leaving a barrel, as an instruction rather than a notification:
// everything a client needs to fly its own copy of it and nothing else.
//
// This is what replaces replicating a bullet as an entity. A round is a
// fixed-size EntityState in every snapshot for the whole three seconds it
// lives -- 145 bytes, sixty times a second, per peer, for something whose
// entire future is a position, a velocity and an expiry. Sent once as this,
// it is 37 bytes total, and the client integrates the rest against the same
// physics the server does.
//
// What that costs is authority over where a round IS: a client's copy is its
// own extrapolation, and one the server despawned on a hit keeps flying until
// the client's own hit test or the lifetime catches it. That was already the
// arrangement for a peer's own shots (see ClientPrediction::Step and
// CosmeticBulletDespawner), and damage was never client-side to begin with.
// Guided rounds are the exception and still travel as entities: nothing on a
// client can extrapolate a seeker.
struct Shot {
    std::uint32_t seq = 0; // globally monotonic, assigned by the stream; 0 = never
    // The ship that fired, so a peer can skip its own -- it already drew that
    // round the moment its trigger went down. 0 for a round nobody owns: a
    // turret's, or the shrapnel of something coming apart.
    std::uint32_t ownerNetId = 0;
    std::uint32_t modelId = 0;
    Magnum::Vector2 pos;
    Magnum::Vector2 vel;
    float rot = 0.f;
    float lifetimeSeconds = 0.f;
    TeamId team = TeamId::None;
};

// The sequenced ring the shots go through, deliberately the same shape as
// GameEventQueue: consumers keep their own cursor and read via ConsumeSince,
// nothing is popped, and a consumer that falls further behind than the ring
// is deep has lost those shots -- which for a round already in flight is the
// right failure, since a shot arriving late would be drawn from a barrel it
// left long ago.
class ShotStream {
public:
    static constexpr std::size_t CAPACITY = 256;

private:
    std::array<Shot, CAPACITY> m_shots{};
    std::size_t m_head = 0; // index of the oldest entry
    std::size_t m_count = 0;
    std::uint32_t m_nextSeq = 1;

public:
    void Emit(Shot shot)
    {
        shot.seq = m_nextSeq++;

        if (m_count == CAPACITY) {
            m_head = (m_head + 1) % CAPACITY;
            --m_count;
        }
        m_shots[(m_head + m_count) % CAPACITY] = shot;
        ++m_count;
    }

    // Calls `f(const Shot&)` for every buffered shot with seq > sinceSeq,
    // oldest first; returns the cursor to pass next time.
    template<typename F>
    std::uint32_t ConsumeSince(std::uint32_t sinceSeq, F&& f) const
    {
        std::uint32_t cursor = sinceSeq;
        for (std::size_t i = 0; i < m_count; ++i) {
            const Shot& shot = m_shots[(m_head + i) % CAPACITY];
            if (shot.seq <= sinceSeq) continue;
            f(shot);
            cursor = shot.seq;
        }
        return cursor;
    }

    [[nodiscard]] std::uint32_t LatestSeq() const { return m_nextSeq - 1; }
};

} // namespace Gravitaris
