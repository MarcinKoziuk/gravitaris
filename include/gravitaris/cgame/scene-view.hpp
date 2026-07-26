#pragma once

#include <flecs.h>

#include <gravitaris/cgame/fwd.hpp>

namespace Gravitaris {

// What one frame reads, and where its overlays go. Single-player keeps every
// entity in the sim registry and draws them with one renderer; multiplayer
// splits both -- the own predicted ship stays in the registry, everything
// else lives in the mirror world and is drawn by the mirror renderer (see
// CGame::m_netClient).
//
// A type, rather than an optional `flecs::world*` on each consumer that wants
// it: those made the multiplayer half opt-in per call site, so a consumer
// that never grew the parameter (or a call site that forgot to pass it) got a
// feature that silently did nothing in multiplayer. That is exactly how the
// off-screen enemy arrows shipped -- swept the registry, which in multiplayer
// holds nothing but your own ship, and submitted overlays into a renderer
// that wasn't drawing that frame.
struct SceneView {
    flecs::world& local;
    // Multiplayer's mirror world; null in single-player.
    flecs::world* remote = nullptr;
    // The renderer actually drawing this frame. Overlays must be submitted to
    // it and no other: whoever draws clears its own overlay scratch at the
    // end of its Render, so overlays handed to an idle renderer accumulate
    // forever.
    ModelRenderer2* overlays = nullptr;

    // Runs `fn` over every world in the view. flecs deduces the query from
    // the callable's parameters, exactly as world.each() would.
    template <typename F>
    void Each(const F& fn) const
    {
        local.each(fn);
        if (remote) remote->each(fn);
    }
};

} // namespace Gravitaris
