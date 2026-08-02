#pragma once

#include <cstdint>

#include <gravitaris/game/component/team.hpp>

namespace Gravitaris {

// What last took hp off a hull -- the "how" of a kill-feed line. Lives here
// rather than beside DeathReport because it is a property of the damage a
// Damageable has taken; DeathSystem only reads it back out.
enum class DamageCause : std::uint8_t {
    Unknown, // hp reached zero with nothing recorded (debug pokes, scripted)
    Gunfire,
    Missile,
    Ram,
    Crash,  // came down harder than the hull's landing threshold
    Debris, // caught the shrapnel of someone else's death
    Star,   // flew into a sun; nothing survives that, at any speed
};

// Anything that can take bullet damage.
//
// Replication class: replicated (server -> clients). Presentation state (the
// white hit-flash) deliberately does NOT live here -- it's the cgame-side
// HitFlash component, driven by Impact/LandingCrash GameEvents, so a
// replicated gameplay component never carries render state (ADR 0001
// constraint 2) and a skipped snapshot can't lose a one-tick flash edge.
struct Damageable {
    float hp = 100.f;
    float maxHp = 100.f;
    // The model's own [landing] fragility (Body::GetLandingFragility), copied
    // here at spawn so DamageSystem doesn't have to walk back to the resource
    // per impact. Deliberately NOT serialized: damage is resolved server-side
    // only, so a client never needs it.
    float landingFragility = 1.f;
    // Who gets the credit for the last hp taken off, and how. Deliberately
    // NOT serialized, for the same reason landingFragility isn't: damage is
    // resolved server-side, and the death it explains travels as its own
    // message rather than as replicated state. `lastDamageTeam` is None when
    // nothing has a side to blame (a crash, own shrapnel).
    DamageCause lastDamageCause = DamageCause::Unknown;
    TeamId lastDamageTeam = TeamId::None;
};

} // namespace Gravitaris
