#pragma once

#include <cstdint>

namespace Gravitaris {

// One entity per pilot, created lazily as ships turn up (the same shape
// FactionState has, and for the same reason: a hull is destroyed on death and
// this has to outlive it). Human peers and AI pilots both get one, which keeps
// the currency inside the sim -- deterministic and replay-visible, ADR 0001 --
// rather than parked in the net layer.
//
// Replication class: replicated to its owner alone. No other client has any
// use for what a given pilot can afford.
struct PilotAccount {
    // What PilotRef points at. Held here rather than implied by the entity,
    // so a lookup is a scan over a handful of accounts with no side table.
    std::uint32_t pilotId = 0;

    // Spent in the SHIP tree to fit a rank onto whatever hull the pilot is
    // flying. Accrues continuously and on kills, and survives death untouched:
    // dying costs the loadout, never the savings, so a pilot is free to fly
    // recklessly and refit afterwards.
    //
    // There is no separate uncollected pool. Landing is already required to
    // *spend* Supplies (every ship node checks it), so gating the accrual on a
    // landing too would be a second lock on the same door.
    std::uint32_t supplies = 0;

    // Whether this account outlives the hull that opened it. True for a human
    // pilot, whose whole point is banking across lives; false for an AI, which
    // gets a fresh identity with every airframe -- so its account is closed
    // when the ship is gone, rather than left accruing for a pilot who no
    // longer exists. Without this a match of respawning AI grows an account
    // per hull ever flown, each one still being paid every tick.
    bool persistent = false;
};

// Who is flying this hull. An id rather than an entity handle, because it has
// to survive the hull: a respawn carries the same id onto the fresh ship and
// finds the same account waiting. Zero means nobody -- a drone or a wreck.
//
// Replication class: server-only.
struct PilotRef {
    std::uint32_t pilotId = 0;
};

} // namespace Gravitaris
