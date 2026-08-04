#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>
#include <vector>

#include <gravitaris/game/component/ai-pilot.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/faction-state.hpp>
#include <gravitaris/game/component/landing-state.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/pilot-account.hpp>
#include <gravitaris/game/component/planet-attachment.hpp>
#include <gravitaris/game/component/research-access.hpp>
#include <gravitaris/game/component/ship-loadout.hpp>
#include <gravitaris/game/component/structure.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/system/gwell/research-system.hpp>

namespace Gravitaris {

static constexpr std::size_t NUM_TEAMS = 7; // TeamId::Blue..None

// How close, and how nearly matched in velocity, a ship has to hold to its
// own High Port to fit parts without landing on the deck. Generous on
// distance (a station is large and the approach is by eye) but tight on
// relative speed, so a fly-past never counts as docked.
static constexpr double DOCK_RADIUS = 90.0;
static constexpr double DOCK_RELATIVE_SPEED = 25.0;

// How much of its Tech an AI faction will commit at once. Below 1 so a side
// keeps something back rather than emptying the pool on the first thing it can
// reach, which would starve the deeper ranks it is saving toward.
static constexpr float AI_TECH_BUDGET_SHARE = 0.75f;

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
    (void)step;

    // One lab's share of the bar per tick; N labs advance N times as fast.
    const float progressPerLabPerTick =
            static_cast<float>(Game::PHYSICS_DELTA / m_config.research.secondsPerTech);

    struct TeamResearch {
        std::uint32_t labs = 0;
        std::vector<std::uint32_t> labPlanets; // planets hosting this team's labs
        flecs::entity anyLab;                  // whichever one announces a payout
        Magnum::Vector2 anyLabPos;
        float progress = 0.f;                  // mirrored onto those labs at the end of the tick
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

    // A faction's own High Ports over a lab planet, which a ship can refit at
    // by holding station alongside rather than setting down on -- see the
    // docking pass below.
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

    // Every pilot's account earns simply for being out there. Done before the
    // purchases below so a landing and the tick that funds it are never one
    // apart, and before the accounts are read, so a fresh pilot is not a tick
    // behind.
    EnsureAccounts();
    const float suppliesPerTick =
            m_config.supplies.perSecond * static_cast<float>(Game::PHYSICS_DELTA);
    m_supplyRemainder += suppliesPerTick;
    const std::uint32_t supplyTick = static_cast<std::uint32_t>(m_supplyRemainder);
    if (supplyTick > 0) {
        m_supplyRemainder -= static_cast<float>(supplyTick);
        m_registry.each([&](PilotAccount& account) { account.supplies += supplyTick; });
    }

    // Who can fit parts this tick: a landed same-team ship at a lab's own
    // planet, or one docked at that planet's High Port. Unlike the old draft
    // this is not one per faction -- a yard serves everyone who reaches it.
    m_registry.each([&](flecs::entity ship, ResearchAccess& access, const Team& team) {
        access.atLab = false;
        if (team.id == TeamId::None) return;
        const std::vector<std::uint32_t>& labPlanets = byTeam[static_cast<std::size_t>(team.id)].labPlanets;
        if (labPlanets.empty()) return;

        if (const LandingState* landing = ship.try_get<LandingState>();
            landing && landing->landed && landing->landedOnNetId != 0) {
            std::uint32_t sitePlanetNetId = landing->landedOnNetId;
            const flecs::entity site = m_entitySpawner.EntityForNetId(landing->landedOnNetId);
            bool ownSite = true;
            if (site.is_alive()) {
                const Structure* structure = site.try_get<Structure>();
                const PlanetOrbitAttachment* orbit = site.try_get<PlanetOrbitAttachment>();
                if (structure && orbit && structure->type == StructureType::HighPort) {
                    const Team* siteTeam = site.try_get<Team>();
                    ownSite = siteTeam && siteTeam->id == team.id;
                    sitePlanetNetId = orbit->planetNetId;
                }
            }
            if (ownSite
                && std::find(labPlanets.begin(), labPlanets.end(), sitePlanetNetId) != labPlanets.end()) {
                access.atLab = true;
                return;
            }
        }

        // Docking: setting a fighter down on a station deck that is itself
        // sweeping along its orbit is far fiddlier than landing on a planet,
        // and failing it reads as the yard simply not being open. Holding
        // station alongside one -- close, and near its velocity -- counts.
        const Transform* transf = ship.try_get<Transform>();
        if (!transf) return;
        for (const Dock& dock : docks) {
            if (dock.team != team.id) continue;
            if ((dock.pos - transf->pos).length() > DOCK_RADIUS) continue;
            if ((transf->vel - dock.vel).length() > DOCK_RELATIVE_SPEED) continue;
            access.atLab = true;
            return;
        }
    });

    // Which sides have somebody reading the tree for themselves. Gathered
    // before the walk below rather than asked inside it: a query run inside
    // another query's callback yields nothing.
    std::array<bool, NUM_TEAMS> humanPilot{};
    m_registry.each([&](flecs::entity ship, const ShipLoadout&, const Team& team) {
        if (team.id == TeamId::None || ship.has<AIPilot>()) return;
        humanPilot[static_cast<std::size_t>(team.id)] = true;
    });

    m_registry.each([&](FactionState& fs) {
        TeamResearch& tr = byTeam[static_cast<std::size_t>(fs.team)];

        // Lose every lab and the bar simply stalls where it stood. Nothing
        // else stalls it: Tech banks, and a side that never spends it has only
        // itself to answer to.
        if (tr.labs > 0) {
            fs.researchProgress += progressPerLabPerTick * static_cast<float>(tr.labs);
            if (fs.researchProgress >= 1.f) {
                fs.researchProgress -= 1.f;
                fs.techPoints += static_cast<std::uint32_t>(m_config.research.techPerFill);
                // Announced from a lab rather than from nowhere, so the sound
                // has a position -- it is that building that did the work.
                m_eventQueue.Emit(GameEventType::ResearchComplete, tr.anyLab, tr.anyLabPos,
                                  static_cast<std::uint32_t>(fs.team));
            }
        }

        // An AI side has nobody reading the tree, so it commits its own Tech.
        // Without this it banks forever and its pilots fly stock hulls all
        // match, however hard its labs work.
        if (!humanPilot[static_cast<std::size_t>(fs.team)]) {
            const auto budget = static_cast<std::uint32_t>(
                    static_cast<float>(fs.techPoints) * AI_TECH_BUDGET_SHARE);
            const UpgradeCatalog::Choice choice = m_catalog.PreferredUnlock(fs.unlocked, budget);
            if (choice.def && m_catalog.UnlockRank(*choice.def, choice.rank, fs.unlocked, fs.techPoints)) {
                LOG(info) << "research: team " << static_cast<int>(fs.team) << " unlocked "
                          << choice.def->key << " " << static_cast<int>(choice.rank);
            }
        }

        tr.progress = fs.researchProgress;
    });

    ApplyPurchases();

    // FactionState is server-only and a Lab's Structure is replicated, so the
    // labs carry the copy the client's glow reads.
    m_registry.each([&](Structure& s, const Team& team) {
        if (s.type != StructureType::Lab || team.id == TeamId::None) return;
        s.researchProgress = byTeam[static_cast<std::size_t>(team.id)].progress;
    });
}

// One purchase per ship per tick, from Controls::techPick -- a human's click,
// or what an AI scored for itself. Both go through the same catalog rules, so
// there is exactly one place that decides whether something can be bought.
void ResearchSystem::ApplyPurchases()
{
    // Collected first and acted on afterwards, because acting means reaching
    // the faction and the pilot's account -- and a flecs query run inside
    // another query's callback yields nothing.
    struct Purchase {
        flecs::entity ship;
        TechPick pick;
    };
    std::vector<Purchase> purchases;
    m_registry.each([&](flecs::entity ship, Controls& controls, const ShipLoadout&,
                        const ResearchAccess&, const Team&) {
        // One-shot, whether or not it turns out to buy anything: a held click
        // must not buy twice.
        if (controls.techPick.IsSet() || ship.has<AIPilot>()) {
            purchases.push_back(Purchase{ship, controls.techPick});
            controls.techPick = {};
        }
    });

    for (const Purchase& purchase : purchases) {
        const flecs::entity ship = purchase.ship;
        if (!ship.is_alive()) continue;
        const Team& team = ship.get<Team>();
        const ResearchAccess& access = ship.get<ResearchAccess>();
        ShipLoadout& loadout = ship.get_mut<ShipLoadout>();

        FactionState* fs = FactionFor(team.id);
        if (!fs) continue;

        PilotAccount* account = AccountFor(ship);

        TechPick pick = purchase.pick;

        if (ship.has<AIPilot>() && account) {
            // An AI is offered the same tree and scores it against what it is
            // already carrying, so a wing ends up spread across shields, racks
            // and guns rather than every hull buying the same first thing.
            const UpgradeCatalog::ShipContext context{&loadout, &fs->unlocked, account->supplies,
                                                      access.atLab};
            const UpgradeCatalog::Choice choice = m_catalog.PreferredFit(loadout, context);
            if (choice.def) pick = TechPick{choice.def->id, TechTab::Ship, choice.rank};
        }

        if (!pick.IsSet()) continue;
        const UpgradeDef* def = m_catalog.Find(pick.node);
        if (!def) continue;

        if (pick.tab == TechTab::Permanent) {
            // Nothing about learning a part needs the hull to be anywhere in
            // particular, so this commits in flight.
            if (!m_catalog.UnlockRank(*def, pick.rank, fs->unlocked, fs->techPoints)) continue;
            LOG(info) << "research: team " << static_cast<int>(team.id) << " unlocked " << def->key
                      << " " << static_cast<int>(pick.rank);
            continue;
        }

        if (!account) continue;
        if (!m_catalog.FitRank(*def, pick.rank, loadout, fs->unlocked, account->supplies, access.atLab)) {
            continue;
        }

        if (AIPilot* aiPilot = ship.try_get_mut<AIPilot>(); aiPilot && aiPilot->upgradesWanted > 0) {
            --aiPilot->upgradesWanted;
        }

        const Transform& transf = ship.get<Transform>();
        m_eventQueue.Emit(GameEventType::UpgradeCollected, ship,
                          Magnum::Vector2{static_cast<float>(transf.pos.x()),
                                          static_cast<float>(transf.pos.y())},
                          static_cast<std::uint32_t>(team.id));
        LOG(info) << "research: team " << static_cast<int>(team.id) << " fitted " << def->key << " "
                  << static_cast<int>(pick.rank);
    }
}

void ResearchSystem::AwardKill(std::uint32_t pilotId)
{
    if (pilotId == 0) return;
    if (PilotAccount* account = FindAccount(pilotId)) {
        account->supplies += static_cast<std::uint32_t>(m_config.supplies.perKill);
    }
}

// Creates the account entity for any pilot that doesn't have one yet, and
// indexes every account by pilot id for the rest of the tick. Lazy in the same
// way FactionSystem creates FactionState: a pilot exists as soon as a hull
// names one, and the account outlives every hull it flies.
//
// Two flat passes rather than a lookup nested inside the walk over PilotRefs:
// a flecs query run inside another query's callback yields nothing, so a
// nested FindAccount reports every pilot as new and opens them a second
// account every tick.
void ResearchSystem::EnsureAccounts()
{
    m_accounts.clear();
    m_registry.each([&](flecs::entity entity, const PilotAccount& account) {
        m_accounts.push_back(Account{account.pilotId, entity});
    });

    // Gathered in the same pass the missing ones are, since pruning needs to
    // know exactly which ids still have a hull behind them.
    std::vector<std::uint32_t> referenced;
    std::vector<std::pair<std::uint32_t, bool>> missing; // id, outlives its hull
    m_registry.each([&](flecs::entity ship, const PilotRef& ref) {
        if (ref.pilotId == 0) return;
        referenced.push_back(ref.pilotId);
        if (FindAccountEntity(ref.pilotId).is_alive()) return;

        const auto known = [&](const std::pair<std::uint32_t, bool>& entry) {
            return entry.first == ref.pilotId;
        };
        if (std::find_if(missing.begin(), missing.end(), known) != missing.end()) return;
        missing.emplace_back(ref.pilotId, !ship.has<AIPilot>());
    });

    for (const auto& [pilotId, persistent] : missing) {
        PilotAccount account;
        account.pilotId = pilotId;
        account.persistent = persistent;
        m_accounts.push_back(Account{pilotId, m_registry.entity().set<PilotAccount>(account)});
    }

    // A human's account survives their death -- that is the whole of banking
    // across lives, and a peer's next hull carries the same PilotRef back to
    // it. An AI's does not: it flies one airframe under one identity, so once
    // that hull is gone nothing will ever ask for the account again.
    std::vector<flecs::entity> closed;
    for (const Account& account : m_accounts) {
        if (!account.entity.is_alive()) continue;
        const PilotAccount* held = account.entity.try_get<PilotAccount>();
        if (!held || held->persistent) continue;
        if (std::find(referenced.begin(), referenced.end(), account.pilotId) != referenced.end()) continue;
        closed.push_back(account.entity);
    }
    for (flecs::entity entity : closed) entity.destruct();
    if (!closed.empty()) {
        m_accounts.erase(std::remove_if(m_accounts.begin(), m_accounts.end(),
                                        [](const Account& a) { return !a.entity.is_alive(); }),
                         m_accounts.end());
    }
}

flecs::entity ResearchSystem::FindAccountEntity(std::uint32_t pilotId) const
{
    for (const Account& account : m_accounts) {
        if (account.pilotId == pilotId) return account.entity;
    }
    return flecs::entity::null();
}

PilotAccount* ResearchSystem::FindAccount(std::uint32_t pilotId)
{
    const flecs::entity entity = FindAccountEntity(pilotId);
    return entity.is_alive() ? entity.try_get_mut<PilotAccount>() : nullptr;
}

PilotAccount* ResearchSystem::AccountFor(flecs::entity ship)
{
    const PilotRef* ref = ship.try_get<PilotRef>();
    return ref && ref->pilotId != 0 ? FindAccount(ref->pilotId) : nullptr;
}

FactionState* ResearchSystem::FactionFor(TeamId team)
{
    if (team == TeamId::None) return nullptr;
    FactionState* found = nullptr;
    m_registry.each([&](FactionState& fs) {
        if (fs.team == team) found = &fs;
    });
    return found;
}

} // namespace Gravitaris
