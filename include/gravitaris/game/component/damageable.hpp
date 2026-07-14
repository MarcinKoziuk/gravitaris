#pragma once

namespace Gravitaris {

// Anything that can take bullet damage. flashAmount is a plain field
// decremented in place every tick (rather than a component added/removed
// per hit) since flecs discourages dynamically adding/removing components
// on hot entities; it drives the render-side hit-flash (see ModelRenderer2).
struct Damageable {
    float hp = 100.f;
    float maxHp = 100.f;
    float flashAmount = 0.f; // 1 right after a hit, decaying to 0
};

} // namespace Gravitaris
