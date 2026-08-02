#include <string>

#include <gravitaris/game/event/death-report.hpp>

namespace Gravitaris {

std::string FormatDeathMessage(const DeathReport& report)
{
    const std::string victim = TeamDisplayName(report.victimTeam);
    const std::string killer = TeamDisplayName(report.killerTeam);

    // Nobody to blame: the sector got them, so the line names only the loss.
    if (killer.empty()) {
        switch (report.cause) {
        case DamageCause::Crash:
            return victim + " came down too hard";
        case DamageCause::Debris:
            return victim + " flew into the debris";
        case DamageCause::Star:
            return victim + " flew into the sun";
        default:
            return victim + " was destroyed";
        }
    }

    switch (report.cause) {
    case DamageCause::Missile:
        return victim + " took a missile from " + killer;
    case DamageCause::Ram:
        return victim + " was rammed by " + killer;
    case DamageCause::Crash:
        // Chased into the dirt: the pursuer still gets the line, since the
        // landing that finished it was not a landing anyone chose.
        return victim + " was driven into the ground by " + killer;
    case DamageCause::Debris:
        return victim + " was caught by " + killer + "'s debris";
    case DamageCause::Star:
        return victim + " was chased into the sun by " + killer;
    case DamageCause::Gunfire:
    case DamageCause::Unknown:
        break;
    }
    return victim + " was shot down by " + killer;
}

const char* TeamDisplayName(TeamId team)
{
    switch (team) {
    case TeamId::Blue: return "Blue";
    case TeamId::Red: return "Red";
    case TeamId::Violet: return "Violet";
    case TeamId::Yellow: return "Yellow";
    case TeamId::Magenta: return "Magenta";
    case TeamId::Cyan: return "Cyan";
    case TeamId::None: break;
    }
    return "";
}

} // namespace Gravitaris
