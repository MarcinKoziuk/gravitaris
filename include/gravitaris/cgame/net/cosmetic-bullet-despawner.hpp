#pragma once

#include <flecs.h>

#include <gravitaris/game/component/transform.hpp>

namespace Gravitaris {

// Stops a cosmetic bullet as soon as it has actually hit something. Every
// unguided round on a client is one of these now: its own, spawned the
// instant its trigger went down (ClientPrediction::Step), and everybody
// else's, spawned from the Shot the server sent once instead of replicating
// a travelling entity (RemoteShotApplier). None of them decides anything --
// zero damage, and DamageSystem never runs against this world -- so taking
// them off the screen at the right moment is the whole job, and it is done
// by two independent mechanisms, because either alone was observed to fail
// in practice:
//
// - CheckLocalHits: a local hit test against the mirror world, independent of
//   any network message. GameEventQueue's own doc comment describes a
//   "loss-tolerance model" of re-sending everything since the client's last
//   acked seq, but NetServer::BroadcastSnapshot deliberately does NOT use
//   client acks for this (see PeerState::lastSentEventSeq's own comment: a peer
//   that never acks correctly must not wedge its own event stream), so in
//   practice a single dropped packet permanently loses that Impact event for
//   this peer -- no resend ever happens. Checking locally means the bullet
//   stops even if that event never arrives.
// - MatchImpact: when a server Impact event *does* arrive, matches by
//   position rather than owner id -- see its own doc comment (in the .cpp)
//   for why an owner id can never work here.
//
// `registry` is the client's own local prediction world -- its own ship, its
// planet proxies, and every cosmetic round in the sector, whoever fired it.
// That is where rounds have to live to move at all: the mirror world is
// presentation-only (ADR 0001) and has no Chipmunk space behind it, which is
// also why hits there are resolved against the authored Body geometry
// directly (QueryBodySegment, via the HitOutline SnapshotApplier attaches).
class CosmeticBulletDespawner {
public:
    // Forgiveness margin around a bullet's swept segment, the same one
    // DamageSystem's own cpSpaceSegmentQuery is given so the two agree on a
    // graze.
    static constexpr double BULLET_QUERY_RADIUS = 2.0;

    // How near an arriving Impact event a bullet must be to be the one that
    // caused it. The real bullet keeps travelling while the event propagates
    // through the snapshot pipeline, so this covers that overshoot -- but no
    // more: a wider window took out the rest of a burst still in flight behind
    // the round that connected.
    static constexpr double BULLET_IMPACT_MATCH_RADIUS = 30.0;

    CosmeticBulletDespawner(flecs::world& registry, flecs::world& mirrorWorld);

    // Resolves every own bullet's swept segment (prevPos -> pos, so a fast
    // round can't tunnel past a check done only at its instantaneous position)
    // against the real hull and shield shapes of every hostile ship and
    // structure drawn this frame, by the same nearest-hit, shields-first,
    // planets-pass-through rules DamageSystem applies server-side. A round that
    // connects is destroyed and lights the hull flash or the struck shield
    // plate immediately, rather than waiting out a round trip for the server's
    // own event to say the same thing.
    void CheckLocalHits();

    // Destroys any of this client's own still-alive bullets within
    // BULLET_IMPACT_MATCH_RADIUS of `impactPos`.
    void MatchImpact(const Vector2d& impactPos);

private:
    flecs::world& m_registry;
    flecs::world& m_mirrorWorld;
};

} // namespace Gravitaris
