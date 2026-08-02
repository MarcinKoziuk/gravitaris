#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

#include <gravitaris/game/component/ai-pilot.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/faction-state.hpp>
#include <gravitaris/game/component/landing-state.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/planet-attachment.hpp>
#include <gravitaris/game/component/ship-loadout.hpp>
#include <gravitaris/game/component/structure.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/upgrade-draft.hpp>
#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/system/gwell/research-system.hpp>

namespace Gravitaris {

static constexpr std::size_t NUM_TEAMS = 7; // TeamId::Blue..None

// How close, and how nearly matched in velocity, a ship has to hold to its
// own High Port to collect from it without landing on the deck. Generous on
// distance (a station is large and the approach is by eye) but tight on
// relative speed, so a fly-past never collects.
static constexpr double DOCK_RADIUS = 90.0;
static constexpr double DOCK_RELATIVE_SPEED = 25.0;

static std::uint32_t DraftSeed(std::uint64_t tick, std::uint32_t netId);

ResearchSystem::ResearchSystem(flecs::world& registry, EntitySpawner& entitySpawner,
                               GameEventQueue& eventQueue, const UpgradeCatalog& catalog,
                               const EconomyConfig& config)
        : m_registry(registry)
        , m_entitySpawner(entitySpawner)
        , m_eventQueue(eventQueue)
        , m_catalog(catalog)
        , m_config(config)
{}

void ResearchSystem::Update(std::uint64_t step)
{
    // One lab's share of the bar per tick; N labs advance N times as fast.
    const float progressPerLabPerTick =
            static_cast<float>(Game::PHYSICS_DELTA / m_config.research.secondsPerUpgrade);

    struct TeamResearch {
        std::uint32_t labs = 0;
        std::vector<std::uint32_t> labPlanets; // planets hosting this team's labs
        flecs::entity anyLab;                  // whichever one announces a finished upgrade
        Magnum::Vector2 anyLabPos;
        float progress = 0.f;                  // mirrored onto those labs at the end of the tick
        std::uint8_t ready = 0;
        std::uint8_t stockLevel = 0;
    };
    std::array<TeamResearch, NUM_TEAMS> byTeam{};

    m_registry.each([&](flecs::entity lab, const Structure& s, const Team& team, const Transform& transf,
                        const PlanetSurfaceAttachment& attach) {
        if (s.type != StructureType::Lab || team.id == TeamId::None) return;
        TeamResearch& tr = byTeam[static_cast<std::size_t>(team.id)];
        ++tr.labs;
        tr.labPlanets.push_back(attach.planetNetId);
        if (!tr.anyLab.is_alive()) {
            tr.anyLab = lab;
            tr.anyLabPos = Magnum::Vector2{static_cast<float>(transf.pos.x()),
                                           static_cast<float>(transf.pos.y())};
        }
    });

    // A faction's own High Ports over a lab planet, which a ship can collect
    // from by holding station alongside rather than setting down on -- see
    // the docking pass below.
    struct Dock {
        TeamId team = TeamId::None;
        Magnum::Vector2d pos;
        Magnum::Vector2d vel;
    };
    std::vector<Dock> docks;
    m_registry.each([&](const Structure& s, const Team& team, const Transform& transf,
                        const PlanetOrbitAttachment& orbit) {
        if (s.type != StructureType::HighPort || team.id == TeamId::None) return;
        const std::vector<std::uint32_t>& labPlanets = byTeam[static_cast<std::size_t>(team.id)].labPlanets;
        if (std::find(labPlanets.begin(), labPlanets.end(), orbit.planetNetId) == labPlanets.end()) return;
        docks.push_back(Dock{team.id, transf.pos, transf.vel});
    });

    // Who, if anyone, picks this tick's finished upgrade up: a landed
    // same-team ship at a lab's own planet, or one docked at that planet's
    // High Port. One per faction -- there is one upgrade to hand out.
    std::array<flecs::entity, NUM_TEAMS> collector{};
    m_registry.each([&](flecs::entity ship, const LandingState& landing, const Team& team) {
        if (!landing.landed || landing.landedOnNetId == 0 || team.id == TeamId::None) return;
        const std::size_t teamIndex = static_cast<std::size_t>(team.id);
        if (collector[teamIndex].is_alive()) return;

        std::uint32_t sitePlanetNetId = landing.landedOnNetId;
        const flecs::entity site = m_entitySpawner.EntityForNetId(landing.landedOnNetId);
        if (site.is_alive()) {
            const Structure* structure = site.try_get<Structure>();
            const PlanetOrbitAttachment* orbit = site.try_get<PlanetOrbitAttachment>();
            if (structure && orbit && structure->type == StructureType::HighPort) {
                const Team* siteTeam = site.try_get<Team>();
                if (!siteTeam || siteTeam->id != team.id) return;
                sitePlanetNetId = orbit->planetNetId;
            }
        }

        const std::vector<std::uint32_t>& labPlanets = byTeam[teamIndex].labPlanets;
        if (std::find(labPlanets.begin(), labPlanets.end(), sitePlanetNetId) == labPlanets.end()) return;
        collector[teamIndex] = ship;
    });

    // Docking: setting a fighter down on a station deck that is itself sweeping
    // along its orbit is far fiddlier than landing on a planet, and failing it
    // reads as the upgrade simply not being collectable at a High Port. Holding
    // station alongside one -- close, and near its velocity -- counts instead.
    m_registry.each([&](flecs::entity ship, const Transform& transf, const Team& team, const ShipLoadout&) {
        if (team.id == TeamId::None) return;
        const std::size_t teamIndex = static_cast<std::size_t>(team.id);
        if (collector[teamIndex].is_alive()) return;

        for (const Dock& dock : docks) {
            if (dock.team != team.id) continue;
            if ((dock.pos - transf.pos).length() > DOCK_RADIUS) continue;
            if ((transf.vel - dock.vel).length() > DOCK_RELATIVE_SPEED) continue;
            collector[teamIndex] = ship;
            return;
        }
    });

    // Ships holding an open draft this tick; every other ship's panel is
    // blanked at the end (see the last pass).
    std::vector<flecs::entity> offered;

    // Factions whose bar filled this tick, so their pilots can be given a
    // fresh reason to fly home (see the pass after this one).
    std::vector<TeamId> restocked;

    m_registry.each([&](FactionState& fs) {
        TeamResearch& tr = byTeam[static_cast<std::size_t>(fs.team)];

        if (fs.upgradesReady > 0) {
            if (const flecs::entity ship = collector[static_cast<std::size_t>(fs.team)]; ship.is_alive()) {
                ShipLoadout* loadout = ship.try_get_mut<ShipLoadout>();
                UpgradeDraft* draft = ship.try_get_mut<UpgradeDraft>();
                if (loadout && draft) {
                    // The pool holds cards of both scopes, so what a draft is
                    // rolled and scored against is the hull's levels overlaid
                    // with its faction's.
                    const UpgradeLevels levels = UpgradeCatalog::Combined(loadout->levels, fs.levels);

                    // Rolled from the empty state, not from `available`: a
                    // landing that bounces, or a dock held a hair outside its
                    // tolerance for a tick, drops eligibility and would
                    // otherwise reshuffle the panel the moment it comes back.
                    if (draft->offers[0] == 0) {
                        const NetId* netId = ship.try_get<NetId>();
                        draft->offers = m_catalog.RollOffers(
                                levels, DraftSeed(step, netId ? netId->value : 0u));
                    }
                    // Re-raised every qualifying tick, not only the one that
                    // rolled: the last pass below clears it for anyone who is
                    // not a collector *this* tick, so a bounce off the pad or
                    // a tick outside dock tolerance would otherwise blank the
                    // panel for good while the offers themselves survive.
                    draft->available = true;

                    // An AI has no HUD to pick from, so it scores the three
                    // against what it is already carrying (PreferredOffer) --
                    // taking whatever landed in slot one left wings that
                    // never fitted a shield because the card came up second.
                    const Controls* controls = ship.try_get<Controls>();
                    const std::uint8_t slot = ship.has<AIPilot>()
                            ? m_catalog.PreferredOffer(draft->offers, *loadout, levels)
                            : (controls ? controls->upgradePick : 0);

                    if (slot >= 1 && slot <= UpgradeCatalog::OFFER_COUNT) {
                        const id_t chosen = draft->offers[slot - 1];
                        const UpgradeDef* def = m_catalog.Find(chosen);
                        // A faction-scope card lands on the faction and is
                        // kept when this hull dies; everything else is carried
                        // by the ship that collected it.
                        const bool granted = def && (def->scope == UpgradeScope::Faction
                                                             ? m_catalog.ApplyFaction(*def, fs.levels)
                                                             : m_catalog.Apply(*def, *loadout));
                        // A no-op pick (unknown id, or a tier that maxed out
                        // between the roll and the press) leaves the stock
                        // untouched so the player can pick again rather than
                        // losing it.
                        if (granted) {
                            // Spent here rather than inferred from a loadout
                            // diff: the thing that hands an upgrade over is
                            // the thing that knows one was collected.
                            if (AIPilot* aiPilot = ship.try_get_mut<AIPilot>();
                                aiPilot && aiPilot->upgradesWanted > 0) {
                                --aiPilot->upgradesWanted;
                            }
                            // Spends one off the queue; the bar itself is not
                            // reset -- work already done toward the next one
                            // is not the collector's to lose.
                            --fs.upgradesReady;
                            draft->available = false;
                            draft->offers = {};

                            const Transform& shipTransf = ship.get<Transform>();
                            m_eventQueue.Emit(GameEventType::UpgradeCollected, ship,
                                              Magnum::Vector2{static_cast<float>(shipTransf.pos.x()),
                                                              static_cast<float>(shipTransf.pos.y())},
                                              static_cast<std::uint32_t>(fs.team));
                            LOG(info) << "research: team " << static_cast<int>(fs.team) << " collected "
                                      << def->key;
                        }
                    }

                    if (draft->available) offered.push_back(ship);
                }
            }
        }
        // Lose every lab and the bar simply stalls where it stood. Filling the
        // queue stalls it too -- but only until somebody collects, which is
        // the point of a bounded queue rather than an unbounded bank.
        const int capacity = m_catalog.ResearchStockCapacity(fs.levels, m_config.research.stockCapacity);
        if (tr.labs > 0 && fs.upgradesReady < capacity) {
            fs.researchProgress += progressPerLabPerTick * static_cast<float>(tr.labs);
            if (fs.researchProgress >= 1.f) {
                fs.researchProgress = 0.f;
                ++fs.upgradesReady;
                // Every pilot of this faction now has a reason to come home
                // again. Without this a ship spent the intent it spawned with
                // and never went shopping for the rest of the match, however
                // many upgrades its labs went on to finish.
                restocked.push_back(fs.team);
                // Announced from a lab rather than from nowhere, so the sound
                // has a position -- it is that building that finished the work.
                // Distinct from UpgradeCollected, which fires later, when a
                // ship actually turns up to spend it.
                m_eventQueue.Emit(GameEventType::ResearchComplete, tr.anyLab,
                                  tr.anyLabPos, static_cast<std::uint32_t>(fs.team));
            }
        }

        tr.progress = fs.researchProgress;
        tr.ready = fs.upgradesReady;
        tr.stockLevel = fs.levels.researchStock;
    });

    if (!restocked.empty()) {
        m_registry.each([&](AIPilot& pilot, const Team& team) {
            if (std::find(restocked.begin(), restocked.end(), team.id) == restocked.end()) return;
            // One more than it is already after, capped at its own greed, so a
            // pilot that never made it home does not accumulate an unbounded
            // shopping list while its labs keep working.
            pilot.upgradesWanted = std::max<std::uint32_t>(pilot.upgradesWanted, 1u);
            pilot.padWaitRemaining = pilot.personality.padWaitTicks;
        });
    }

    m_registry.each([&](flecs::entity ship, UpgradeDraft& draft, const Team& team) {
        if (std::find(offered.begin(), offered.end(), ship) != offered.end()) return;
        draft.available = false;
        // The rolled three survive as long as the faction's queue is not
        // empty, so re-qualifying shows the same offers.
        const bool waiting = team.id != TeamId::None && byTeam[static_cast<std::size_t>(team.id)].ready > 0;
        if (!waiting) draft.offers = {};
    });

    // FactionState is server-only and a Lab's Structure is replicated, so the
    // labs carry the copy the client's glow reads.
    m_registry.each([&](Structure& s, const Team& team) {
        if (s.type != StructureType::Lab || team.id == TeamId::None) return;
        const TeamResearch& tr = byTeam[static_cast<std::size_t>(team.id)];
        s.researchProgress = tr.progress;
        s.upgradesReady = tr.ready;
        s.researchStockLevel = tr.stockLevel;
    });
}

// Mixes the tick and the collecting ship's NetId into a draft seed. Derived
// from sim state alone, so a replay -- and eventually a second peer -- rolls
// the same three offers (ADR 0001).
static std::uint32_t DraftSeed(std::uint64_t tick, std::uint32_t netId)
{
    std::uint32_t h = 2166136261u;
    const auto mix = [&h](std::uint32_t v) {
        for (int byte = 0; byte < 4; ++byte) {
            h = (h ^ ((v >> (byte * 8)) & 0xffu)) * 16777619u;
        }
    };
    mix(static_cast<std::uint32_t>(tick));
    mix(static_cast<std::uint32_t>(tick >> 32));
    mix(netId);
    return h;
}

} // namespace Gravitaris
