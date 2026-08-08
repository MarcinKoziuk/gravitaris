#pragma once

#include <cstdint>

#include <flecs.h>

#include <gravitaris/game/component/ai-pilot.hpp>
#include <gravitaris/game/component/faction-state.hpp>
#include <gravitaris/game/component/pilot-account.hpp>
#include <gravitaris/game/component/ship-loadout.hpp>
#include <gravitaris/game/upgrade/upgrade-catalog.hpp>

namespace Gravitaris {

// Whether this pilot has business at a yard: something its side has learned,
// that its taste wants, that its purse covers past the reserve it keeps back.
//
// A question about this pilot's money, deliberately -- the earlier version
// asked whether the faction had unlocked anything at all, which every side
// has by the first tick (the ranks that cost no Tech are issued to everyone,
// see ResearchSystem::IssueFreeUnlocks). That answered yes for the whole
// match, so every pilot flew home forever and the padWaitTicks bound that was
// meant to stop a wing parking never applied.
//
// Asked with atLab = true: the question is what a trip would be worth, not
// what can be bought from where the ship is standing now.
inline bool WantsRefit(const UpgradeCatalog& catalog, flecs::entity ship, const AIPilot& pilot,
                       const FactionState* faction)
{
    if (!faction || pilot.upgradesWanted == 0) return false;

    const ShipLoadout* loadout = ship.try_get<ShipLoadout>();
    if (!loadout) return false;

    const PilotRef* ref = ship.try_get<PilotRef>();
    const PilotAccount* account =
            ref && ref->account.is_alive() ? ref->account.try_get<PilotAccount>() : nullptr;
    if (!account || account->supplies <= pilot.personality.supplyReserve) return false;

    const UpgradeCatalog::ShipContext context{loadout, &faction->unlocked,
                                              account->supplies - pilot.personality.supplyReserve,
                                              /*atLab=*/true};
    return catalog.PreferredFit(*loadout, context, pilot.personality.fit).def != nullptr;
}

} // namespace Gravitaris
