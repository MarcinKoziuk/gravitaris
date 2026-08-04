#pragma once

namespace Gravitaris {

// Whether this ship can commit a SHIP-tree purchase right now: set while it is
// standing at a lab site of its own faction, or holding station alongside that
// planet's High Port. Fitting a part needs a yard; learning one does not,
// which is why the PERMANENT tree has no equivalent of this.
//
// Every ship carries one from spawn and the flag toggles in place, rather than
// the component being added and removed as ships come and go from a lab pad --
// see CLAUDE.md's ECS design note.
//
// Replication class: replicated (server -> clients). The client greys the ship
// tree out with it, but never decides it.
struct ResearchAccess {
    bool atLab = false;
};

} // namespace Gravitaris
