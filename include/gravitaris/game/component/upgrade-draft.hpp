#pragma once

#include <gravitaris/game/upgrade/upgrade-catalog.hpp>

namespace Gravitaris {

// The choice a landed ship is being offered by its faction's Lab: three
// upgrades drawn from the pool, one of which it takes (ResearchSystem). Every
// ship carries one from spawn and `available` toggles in place, rather than
// the component being added and removed as ships come and go from a lab pad --
// see CLAUDE.md's ECS design note.
//
// The offers are rolled once, when a ship first becomes eligible, and held
// until it picks or leaves: rerolling per tick would make the panel flicker
// through the pool, and a player deciding between three things needs those
// three to stay put.
//
// Replication class: replicated (server -> clients) -- the client draws the
// panel and turns a keypress into the pick index, but never decides the
// contents.
struct UpgradeDraft {
    UpgradeCatalog::Offers offers{};
    // Set while this ship is standing at a lab site of its own faction with a
    // finished upgrade waiting. Clearing it blanks the panel.
    bool available = false;
};

} // namespace Gravitaris
