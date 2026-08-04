#pragma once

#include <cstdint>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// The materials economy's rates and the round-level timers around it
// (docs/gravity-well-mode-plan.md Phase 3), parsed from data/economy.toml.
// Every default here matches the constant it replaced, so a missing file
// leaves the mode playing exactly as before rather than at zero.
struct EconomyConfig {
    // A Colony digs raw materials out of its planet and pushes them to that
    // planet's own Base and High Port -- two separate draws against one
    // store, one per structure, not a shared cap.
    struct Colony {
        float rawProductionPerTick = 0.5f;
        float rawCap = 200.f;
        float supplyRate = 0.4f;
    } colony;

    // A Base or High Port converts raw into finished, then spends finished.
    struct Production {
        float conversionRate = 0.3f;
        float finishedCap = 200.f;
        float freighterCost = 60.f;
        // What a Base spends developing its own planet -- a Lab, then a Comm
        // Center. Same new-unit rule as a freighter, but same-planet and
        // instant, with no freighter trip needed.
        float selfDevelopmentCost = 40.f;
    } production;

    struct Freighter {
        // Above planetary orbital speed at the sector's orbit radii (51-79),
        // or a freighter can never close on its target -- see economy.toml.
        double transitSpeed = 80.0;
        double transitAcceleration = 20.0;
        // Past the High Port's own orbit, so a parking freighter never sits
        // on top of one.
        double arrivalRadius = 440.0;
        std::uint32_t cargoUnloadIntervalTicks = 300;
        float cargoOneRawMaterials = 25.f;
    } freighter;

    struct Conquest {
        // Consecutive landed ticks before a claim fires: long enough that a
        // bounce or a graze never claims, short enough to feel immediate
        // after a real touchdown.
        std::uint32_t claimTicks = 60;
    } conquest;

    struct Research {
        // Seconds one Lab needs alone; N labs pay out N times as fast.
        double secondsPerTech = 30.0;
        // Technology points each fill of the bar pays into the faction's pool.
        int techPerFill = 2;
    } research;

    // A pilot's own currency, spent in the SHIP tree. Accrues wherever they
    // are; only spending it needs a landing.
    struct Supplies {
        float perSecond = 0.6f;
        int perKill = 8;
    } supplies;

    // Hull restored to a ship standing on one of its faction's developed
    // planets (RepairSystem) -- the same Base+Colony pairing a respawn needs,
    // since that is what makes a rock somewhere to come home to.
    struct Repair {
        float hullPerSecond = 10.f;
    } repair;

    // Reads `path` (default "economy.toml"). Returns false and keeps the
    // defaults above if it can't be read or parsed.
    bool Load(IFilesystem& filesystem, const char* path = "economy.toml");
};

} // namespace Gravitaris
