#pragma once

#include <string>
#include <vector>

#include <flecs.h>

#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Chat-driven cheats, deliberately open to every player with no gate at all:
// this is a game in development and everyone flying it is testing it.
//
// Runs against whichever world owns the sim -- the local one in
// single-player, the server's in multiplayer -- so both paths execute the
// same rule and neither reimplements one. Everything here mutates sim state
// directly; nothing is predicted client-side.
struct CheatResult {
    // What to tell the issuer, one chat line each. Never empty: an unknown
    // command still answers.
    std::vector<std::string> reply;
    // Set for a command that changed the round for everybody (the friendly
    // fire rule, a spawned wave) rather than only the issuer's own ship, so a
    // server broadcasts the reply instead of whispering it back.
    bool announce = false;
};

// Whether `text` is addressed to the console at all. Anything else is chat.
[[nodiscard]] bool IsCheatCommand(const std::string& text);

// Runs `text` on behalf of `subject` -- the issuer's ship, which may be dead
// or never have existed -- flying for `team`.
[[nodiscard]] CheatResult RunCheatCommand(Game& game, flecs::entity subject, TeamId team,
                                          const std::string& text);

} // namespace Gravitaris
