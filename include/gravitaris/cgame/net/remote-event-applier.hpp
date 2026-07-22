#pragma once

#include <cstdint>
#include <functional>

#include <flecs.h>

#include <gravitaris/game/fwd.hpp>
#include <gravitaris/game/net/net-client.hpp>

#include <gravitaris/cgame/net/cosmetic-bullet-despawner.hpp>

namespace Gravitaris {

// Replicated GameEvents (docs/networking-plan.md's "events left to the
// caller" gap): walks `netClient`'s buffered snapshot history for events
// past the last-applied seq, re-emits each into `eventQueue` (audio only
// needs event.pos, already world-space -- no NetId resolution needed for
// that), routes the CosmeticBulletDespawner's Impact-position match, and for
// Impact/LandingCrash sets HitFlash on whichever entity `resolveHitTarget`
// resolves `event.sourceNetId` to.
//
// Skips BulletFired events sourced from the own ship: ClientPrediction
// already emitted the instant, locally-predicted version of that same shot
// into `eventQueue` directly, so replaying the server's (delayed,
// replicated) copy too would double the laser sound. Every other event type
// is never locally predicted for anyone, own ship included, so those always
// play.
//
// `resolveHitTarget` is a callback rather than a stored dependency because
// resolving "own ship" vs. "someone else, via the mirror world" needs
// CGame's own GetPlayer()/SnapshotApplier lookups, which don't belong here.
class RemoteEventApplier {
public:
    RemoteEventApplier(NetClient& netClient, GameEventQueue& eventQueue, CosmeticBulletDespawner& bulletDespawner);

    void Apply(const std::function<flecs::entity(std::uint32_t sourceNetId)>& resolveHitTarget);

private:
    NetClient& m_netClient;
    GameEventQueue& m_eventQueue;
    CosmeticBulletDespawner& m_bulletDespawner;
    std::uint32_t m_lastAppliedSeq = 0;
};

} // namespace Gravitaris
