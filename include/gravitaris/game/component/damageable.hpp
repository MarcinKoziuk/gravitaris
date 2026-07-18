#pragma once

namespace Gravitaris {

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
};

} // namespace Gravitaris
