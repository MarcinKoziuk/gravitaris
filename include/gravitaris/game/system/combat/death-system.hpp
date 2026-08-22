#pragma once

#include <cstdint>

#include <flecs.h>

#include <sigslot/signal.hpp>

#include <gravitaris/game/config/economy-config.hpp>
#include <gravitaris/game/event/death-report.hpp>
#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Destroys entities whose Damageable hp has reached zero. Ships explode into
// a ring of ownerless frag shrapnel (TeamId::None) that damages anyone nearby,
// including the killer -- a first-pass frag-grenade death.
class DeathSystem {
private:
    flecs::world& m_registry;
    EntitySpawner& m_entitySpawner;
    GameEventQueue& m_eventQueue;
    const EconomyConfig& m_config;

    sigslot::signal<const DeathReport&> m_onDeath;

    void Explode(flecs::entity ship, std::uint64_t step);
    // Shuts the planet's build site of this structure's type for
    // EconomyConfig::Rebuild::lockoutTicks, so the complex has to be rebuilt
    // rather than merely re-dispatched (see RebuildLockout).
    void StartRebuildLockout(flecs::entity structure);
    void ReportDeath(flecs::entity ship);
    static void LogStructureDeath(flecs::entity entity, std::uint64_t step);

public:
    DeathSystem(flecs::world& registry, EntitySpawner& entitySpawner, GameEventQueue& eventQueue,
                const EconomyConfig& config);

    void Update(std::uint64_t step);

    // Fired once per ship destroyed, while it is still alive enough to read
    // its team and killer off. Not a GameEvent: those are the replicated
    // one-shot stream that every consumer polls, and a kill feed is a line of
    // text nobody but its own writer cares about -- so it goes out to whoever
    // asked instead. Structures are deliberately silent; the feed is about
    // ships.
    sigslot::signal<const DeathReport&>& OnDeath() { return m_onDeath; }
};

} // namespace Gravitaris
