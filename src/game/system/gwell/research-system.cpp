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
#include <gravitaris/game/system/gwell/home-site.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/system/gwell/research-system.hpp>

namespace Gravitaris {

static constexpr std::size_t NUM_TEAMS = 7; // TeamId::Blue..None

// How long a purchase is still honoured after the yard closes. Generous
// against any plausible round trip, and far too short to fly anywhere on: a
// ship that has been away this long is away.
static constexpr std::uint16_t REFIT_GRACE_TICKS = 45;

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

    // Every pilot's account earns simply for being out there. Done before the
    // purchases below so a landing and the tick that funds it are never one
    // apart, and before the accounts are read, so a fresh pilot is not a tick
    // behind.
    EnsureAccounts();
    IssueFreeUnlocks();
    const float suppliesPerTick =
            m_config.supplies.perSecond * static_cast<float>(Game::PHYSICS_DELTA);
    m_supplyRemainder += suppliesPerTick;
    const std::uint32_t supplyTick = static_cast<std::uint32_t>(m_supplyRemainder);
    if (supplyTick > 0) {
        m_supplyRemainder -= static_cast<float>(supplyTick);
        m_registry.each([&](PilotAccount& account) { account.supplies += supplyTick; });
    }

    // Who can fit parts this tick: a same-team ship standing on a lab's own
    // planet, or on a High Port of its own over that planet -- a station deck
    // is that planet's pad. Feet down either way; holding station alongside a
    // port is not being at it. Unlike the old draft this is not one per
    // faction -- a yard serves everyone who reaches it.
    m_registry.each([&](flecs::entity ship, ResearchAccess& access, const Team& team) {
        if (access.ticksSinceLab < 0xFFFF) ++access.ticksSinceLab;
        access.atLab = false;
        if (team.id == TeamId::None) return;
        const std::vector<std::uint32_t>& labPlanets = byTeam[static_cast<std::size_t>(team.id)].labPlanets;
        if (labPlanets.empty()) return;

        const LandingState* landing = ship.try_get<LandingState>();
        if (!landing || !landing->landed || landing->landedOnNetId == 0) return;

        const flecs::entity site = m_entitySpawner.EntityForNetId(landing->landedOnNetId);
        const Team* siteTeam = site.is_alive() ? site.try_get<Team>() : nullptr;
        if (site.has<Structure>() && (!siteTeam || siteTeam->id != team.id)) return;

        const std::uint32_t sitePlanetNetId = SitePlanetNetId(site);
        if (std::find(labPlanets.begin(), labPlanets.end(), sitePlanetNetId) == labPlanets.end()) return;

        access.atLab = true;
        access.ticksSinceLab = 0;
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

        // Latched rather than read live: a player between lives has no hull,
        // and a side judged AI-run for those few seconds spent their Tech on
        // the catalog's own preferences -- which read, from the tree, exactly
        // like unlocks arriving from somebody else's faction.
        if (humanPilot[static_cast<std::size_t>(fs.team)]) fs.humanLed = true;

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
        if (!fs.humanLed) {
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

        if (const AIPilot* aiPilot = ship.try_get<AIPilot>(); aiPilot && account) {
            // An AI is offered the same tree and scores it against what it is
            // already carrying and what it has a taste for, so a wing ends up
            // spread across shields, racks and guns rather than every hull
            // buying the same first thing. What it keeps back is its own
            // (AIPersonality::supplyReserve) -- rounds come out of the same
            // purse, so a pilot that spends to zero flies home dry.
            const std::uint32_t reserve = aiPilot->personality.supplyReserve;
            const std::uint32_t spendable =
                    account->supplies > reserve ? account->supplies - reserve : 0;
            const UpgradeCatalog::ShipContext context{&loadout, &fs->unlocked, spendable,
                                                      access.atLab};
            const UpgradeCatalog::Choice choice =
                    m_catalog.PreferredFit(loadout, context, aiPilot->personality.fit);
            if (choice.def) pick = TechPick{choice.def->id, TechTab::Ship, choice.rank};
        }

        if (!pick.IsSet()) continue;

        const Transform& transf = ship.get<Transform>();
        const Magnum::Vector2 where{static_cast<float>(transf.pos.x()),
                                    static_cast<float>(transf.pos.y())};

        // What the port would say about it, if it says anything.
        const auto deny = [&](TechNodeState reason) {
            m_eventQueue.Emit(GameEventType::RefitDenied, ship, where,
                              static_cast<std::uint32_t>(reason));
        };

        // The yard counts as open for a moment after it closes -- see
        // REFIT_GRACE_TICKS. A pilot who was on the pad when they clicked gets
        // served, whatever the round trip did to them in between.
        const bool served = access.atLab || access.ticksSinceLab <= REFIT_GRACE_TICKS;

        // Rounds for whatever the hull already carries. Names no node, so it is
        // answered before the pool is consulted at all -- and priced by the
        // catalog, never by the click, so a client cannot ask for a discount.
        if (pick.resupply) {
            if (!account) continue;
            const std::uint32_t cost = m_catalog.ResupplyCost(loadout);
            if (m_catalog.Resupply(loadout, account->supplies, served)) {
                LOG(info) << "research: team " << static_cast<int>(team.id) << " resupplied for "
                          << cost;
            }
            // Nothing missing is not a refusal: the button is already idle, and
            // a stale click on it should say nothing at all.
            else if (cost > 0 && !ship.has<AIPilot>()) {
                deny(served ? TechNodeState::Unaffordable : TechNodeState::NeedsLanding);
            }
            continue;
        }

        const UpgradeDef* def = m_catalog.Find(pick.node);
        if (!def) continue;

        if (pick.tab == TechTab::Permanent) {
            // Nothing about learning a part needs the hull to be anywhere in
            // particular, so this commits in flight.
            if (!m_catalog.UnlockRank(*def, pick.rank, fs->unlocked, fs->techPoints)) {
                if (!ship.has<AIPilot>()) {
                    deny(m_catalog.PermanentState(*def, pick.rank, fs->unlocked, fs->techPoints));
                }
                continue;
            }
            LOG(info) << "research: team " << static_cast<int>(team.id) << " unlocked " << def->key
                      << " " << static_cast<int>(pick.rank);
            continue;
        }

        if (!account) continue;

        // Pulling a part is free and needs nothing but the yard, so it does
        // not go past the purse or the tree at all.
        if (pick.strip) {
            if (!m_catalog.StripRank(*def, loadout, served, pick.mount)) {
                if (!ship.has<AIPilot>()) deny(TechNodeState::NeedsLanding);
                continue;
            }
            LOG(info) << "research: team " << static_cast<int>(team.id) << " stripped " << def->key;
            continue;
        }

        if (!m_catalog.FitRank(*def, pick.rank, loadout, fs->unlocked, account->supplies, served,
                               pick.mount)) {
            if (!ship.has<AIPilot>()) {
                const UpgradeCatalog::ShipContext why{&loadout, &fs->unlocked, account->supplies, served};
                deny(m_catalog.ShipState(*def, pick.rank, why));
            }
            continue;
        }

        if (AIPilot* aiPilot = ship.try_get_mut<AIPilot>(); aiPilot && aiPilot->upgradesWanted > 0) {
            --aiPilot->upgradesWanted;
        }

        m_eventQueue.Emit(GameEventType::UpgradeCollected, ship, where,
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
// A rank that costs nothing to learn is one every side already knows: the light
// guns a hull flies with, and the drive that moves it. Granted here rather than
// at faction creation so a side that appears mid-match gets them too, and so
// nothing depends on the order the factions were made in.
void ResearchSystem::IssueFreeUnlocks()
{
    const std::vector<UpgradeDef>& defs = m_catalog.Defs();
    m_registry.each([&](FactionState& faction) {
        for (std::size_t i = 0; i < defs.size() && i < MAX_UPGRADE_DEFS; ++i) {
            const UpgradeDef& def = defs[i];
            if (!def.researched || UpgradeCatalog::TechCostOf(def, 1) != 0) continue;
            if (faction.unlocked.rank[i] == 0) faction.unlocked.rank[i] = 1;
        }
    });
}

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

    // The id resolved onto the hull, so anything holding a ship can read the
    // purse without a scan of its own -- what lets the AI weigh a trip home
    // against what it can actually afford. Last in the pass, so a hull whose
    // account was opened or closed above gets this tick's answer.
    m_registry.each([&](PilotRef& ref) {
        ref.account = ref.pilotId != 0 ? FindAccountEntity(ref.pilotId) : flecs::entity();
    });
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
