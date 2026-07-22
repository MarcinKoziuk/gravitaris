#pragma once

#include <flecs.h>

#include <gravitaris/game/component/transform.hpp>

namespace Gravitaris {

// Stops a client's own locally-predicted cosmetic bullet (ClientPrediction::
// Step spawns a purely visual copy of every shot fired -- zero damage,
// DamageSystem never runs against it) as soon as it's actually hit
// something, via two independent mechanisms that both exist because either
// alone was observed to fail in practice:
//
// - CheckLocalHits: a local proximity check against the mirror world,
//   independent of any network message. GameEventQueue's own doc comment
//   describes a "loss-tolerance model" of re-sending everything since the
//   client's last acked seq, but NetServer::BroadcastSnapshot deliberately
//   does NOT use client acks for this (see PeerState::lastSentEventSeq's own
//   comment: a peer that never acks correctly must not wedge its own event
//   stream), so in practice a single dropped packet permanently loses that
//   Impact event for this peer -- no resend ever happens. Checking locally
//   means the bullet stops even if that event never arrives.
// - MatchImpact: when a server Impact event *does* arrive, matches by
//   position rather than owner id -- see its own doc comment (in the .cpp)
//   for why an owner id can never work here.
//
// `registry` is the client's own local prediction world (its own ship, own
// cosmetic bullets, and Phase 7's planet proxies -- nothing else); mirror
// world is presentation-only (ADR 0001), so hostile ships there have no real
// collision shape to query.
class CosmeticBulletReaper {
public:
    // Ships don't expose a simple bounding radius (their collision Body is
    // an authored polygon, not a circle, unlike planets); the largest ship
    // body (freighter-0) has a ~15.8 unit half-diagonal. Biased a bit above
    // that on purpose -- an occasional visible overshoot reads better than a
    // bullet stopping short of the ship it was aimed at.
    static constexpr double LOCAL_HIT_RADIUS = 22.0;

    // Generous on purpose: by the time an Impact event has propagated
    // through the snapshot/event pipeline, the real bullet already overshot
    // the impact point along the same straight line the cosmetic copy is
    // still flying, so an exact position match would almost never hit.
    static constexpr double BULLET_IMPACT_MATCH_RADIUS = 100.0;

    CosmeticBulletReaper(flecs::world& registry, flecs::world& mirrorWorld);

    // Approximate on purpose: checks distance from each hostile ship's
    // rendered position to the bullet's swept segment (prevPos -> pos, so a
    // fast bullet can't tunnel past a check done only at its instantaneous
    // position) against LOCAL_HIT_RADIUS, rather than exact geometry.
    void CheckLocalHits();

    // Destroys any of this client's own still-alive bullets within
    // BULLET_IMPACT_MATCH_RADIUS of `impactPos`.
    void MatchImpact(const Vector2d& impactPos);

private:
    flecs::world& m_registry;
    flecs::world& m_mirrorWorld;
};

} // namespace Gravitaris
