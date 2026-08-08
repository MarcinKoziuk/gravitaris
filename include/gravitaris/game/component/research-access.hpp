#pragma once

namespace Gravitaris {

// Whether this ship can commit a SHIP-tree purchase right now: set while it is
// standing at a lab site of its own faction -- that planet's surface, or the
// deck of its own High Port over it, which is the same yard. Fitting a part
// needs a yard; learning one does not, which is why the PERMANENT tree has no
// equivalent of this.
//
// Every ship carries one from spawn and the flag toggles in place, rather than
// the component being added and removed as ships come and go from a lab pad --
// see CLAUDE.md's ECS design note.
//
// Replication class: replicated (server -> clients). The client greys the ship
// tree out with it, but never decides it.
struct ResearchAccess {
    bool atLab = false;

    // Ticks since the yard was last open to this ship. A purchase is honoured
    // for a short window after it closes, because the commonest way to be
    // refused is not being anywhere near a lab -- it is bouncing off the pad
    // for a moment during the round trip between the click and the server
    // hearing about it. The player did everything right and the latency took
    // it, which is not a thing to report; it is a thing to absorb.
    //
    // Server-only, unlike the flag above: a client greys its tree out on
    // where the ship *is*, not on how long ago it was somewhere.
    std::uint16_t ticksSinceLab = 0xFFFF;
};

} // namespace Gravitaris
