#pragma once

#include <array>
#include <cstdint>

#include <flecs.h>

#include <Magnum/Magnum.h>
#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/component/net-id.hpp>

namespace Gravitaris {

// One-shot gameplay occurrences (quake3's entityState events, adapted -- see
// docs/networking-plan.md "quake3 -> Gravitaris mapping"). Continuous states
// (thruster held, flash decay) are NOT events; they stay component state.
enum class GameEventType : std::uint8_t {
    BulletFired,    // source = shooter, pos = muzzle
    Impact,         // source = victim,  pos = hit point,  param = damage*10
    Explosion,      // source = the ship that died, pos = its position
    LandingCrash,   // source = the ship, pos = contact,   param = damage*10
    PlanetClaimed,  // source = the planet, pos = its position, param = new TeamId
    StructureBuilt, // source = the new structure, pos = its position, param = StructureType
    FactionDefeated, // source = none, pos = unused, param = the defeated TeamId
    RoundOver,       // source = none, pos = unused, param = the winning TeamId
};

struct GameEvent {
    std::uint32_t seq = 0; // globally monotonic, assigned by the queue; 0 = never
    std::uint64_t tick = 0;
    GameEventType type{};
    std::uint32_t sourceNetId = 0;
    Magnum::Vector2 pos{};
    std::uint32_t param = 0;
};

// The sim's single serializable stream of one-shots. Systems Emit() during
// their tick; consumers (audio, hit-flash, later: per-client snapshot
// serialization) each keep their own cursor and read via ConsumeSince, so
// nothing is "popped" -- an event exists until the ring overwrites it, which
// is also the loss-tolerance model for replication (re-send everything since
// the client's last acked seq; receivers drop seq <= cursor as duplicates).
class GameEventQueue {
public:
    static constexpr std::size_t CAPACITY = 256;

private:
    std::array<GameEvent, CAPACITY> m_events{};
    std::size_t m_head = 0; // index of the oldest entry
    std::size_t m_count = 0;
    std::uint32_t m_nextSeq = 1;
    std::uint64_t m_currentTick = 0;

public:
    // Game::Update stamps this each tick so emitters don't need the step
    // threaded through every call.
    void SetCurrentTick(std::uint64_t tick) { m_currentTick = tick; }

    // Stamps seq/tick and resolves the source entity's NetId (0 if it has
    // none). Overwrites the oldest event when full -- a consumer that far
    // behind has lost those events to the CAPACITY window, same contract as
    // InputQueue.
    void Emit(GameEventType type, flecs::entity source, const Magnum::Vector2& pos,
              std::uint32_t param = 0)
    {
        GameEvent event;
        event.seq = m_nextSeq++;
        event.tick = m_currentTick;
        event.type = type;
        if (source.is_alive()) {
            if (const NetId* netId = source.try_get<NetId>()) {
                event.sourceNetId = netId->value;
            }
        }
        event.pos = pos;
        event.param = param;

        if (m_count == CAPACITY) {
            m_head = (m_head + 1) % CAPACITY;
            --m_count;
        }
        m_events[(m_head + m_count) % CAPACITY] = event;
        ++m_count;
    }

    // Calls `f(const GameEvent&)` for every buffered event with seq >
    // sinceSeq, oldest first; returns the cursor to pass next time (the
    // highest seq seen, or sinceSeq unchanged if nothing new).
    template<typename F>
    std::uint32_t ConsumeSince(std::uint32_t sinceSeq, F&& f) const
    {
        std::uint32_t cursor = sinceSeq;
        for (std::size_t i = 0; i < m_count; ++i) {
            const GameEvent& event = m_events[(m_head + i) % CAPACITY];
            if (event.seq <= sinceSeq) continue;
            f(event);
            cursor = event.seq;
        }
        return cursor;
    }

    [[nodiscard]] std::uint32_t LatestSeq() const { return m_nextSeq - 1; }
};

} // namespace Gravitaris
