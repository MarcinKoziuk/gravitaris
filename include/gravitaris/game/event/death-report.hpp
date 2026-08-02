#pragma once

#include <string>

#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/team.hpp>

namespace Gravitaris {

// One ship's death, read off it while it is still alive enough to read: whose
// it was, who gets the credit, and how it went. DeathSystem publishes these
// through its OnDeath signal; who listens (single-player's own chat log, the
// server's broadcast) is not the sim's business.
struct DeathReport {
    TeamId victimTeam = TeamId::None;
    // The side credited with the kill, None when nothing is (a crash, or the
    // ownerless shrapnel of an earlier death).
    TeamId killerTeam = TeamId::None;
    DamageCause cause = DamageCause::Unknown;
};

// The kill-feed line, worded in one place so single-player's local log and the
// server's broadcast can never phrase the same death differently.
std::string FormatDeathMessage(const DeathReport& report);

// What a kill-feed line calls a side. Also what the HUD looks for to colour
// those names back in: a line reaches a connected client as finished text (it
// rides the chat channel), so the names are recovered from the text rather
// than carried beside it -- which works because this is a closed set of words
// and no line is ever written by a player. Empty for TeamId::None.
const char* TeamDisplayName(TeamId team);

} // namespace Gravitaris
