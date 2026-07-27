#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

#include <gravitaris/game/component/faction-state.hpp>
#include <gravitaris/game/component/landing-state.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/planet-attachment.hpp>
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

// One lab's share of the bar per tick; N labs advance N times as fast.
static constexpr float PROGRESS_PER_LAB_PER_TICK =
        static_cast<float>(Game::PHYSICS_DELTA / ResearchSystem::RESEARCH_SECONDS);

ResearchSystem::ResearchSystem(flecs::world& registry, EntitySpawner& entitySpawner,
                               GameEventQueue& eventQueue)
        : m_registry(registry)
        , m_entitySpawner(entitySpawner)
        , m_eventQueue(eventQueue)
{}

void ResearchSystem::Update()
{
    struct TeamResearch {
        std::uint32_t labs = 0;
        std::vector<std::uint32_t> labPlanets; // planets hosting this team's labs
        float progress = 0.f;                  // mirrored onto those labs at the end of the tick
        bool ready = false;
    };
    std::array<TeamResearch, NUM_TEAMS> byTeam{};

    m_registry.each([&](const Structure& s, const Team& team, const PlanetSurfaceAttachment& attach) {
        if (s.type != StructureType::Lab || team.id == TeamId::None) return;
        TeamResearch& tr = byTeam[static_cast<std::size_t>(team.id)];
        ++tr.labs;
        tr.labPlanets.push_back(attach.planetNetId);
    });

    // Who, if anyone, picks this tick's finished upgrade up: a landed
    // same-team ship at a lab's own planet, or at that planet's High Port.
    // One per faction -- there is one upgrade to hand out.
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

    m_registry.each([&](FactionState& fs) {
        TeamResearch& tr = byTeam[static_cast<std::size_t>(fs.team)];

        if (fs.upgradeReady) {
            if (const flecs::entity ship = collector[static_cast<std::size_t>(fs.team)]; ship.is_alive()) {
                fs.upgradeReady = false;
                fs.researchProgress = 0.f;

                // Missiles are the only upgrade so far; faster guns and
                // shields join it as further ShipLoadout fields.
                ShipLoadout& loadout = ship.get_mut<ShipLoadout>();
                loadout.missileAmmo = static_cast<std::uint8_t>(
                        std::min<int>(loadout.missileAmmo + MISSILES_PER_UPGRADE, MISSILE_CAPACITY));

                const Transform& shipTransf = ship.get<Transform>();
                m_eventQueue.Emit(GameEventType::UpgradeCollected, ship,
                                  Magnum::Vector2{static_cast<float>(shipTransf.pos.x()),
                                                  static_cast<float>(shipTransf.pos.y())},
                                  static_cast<std::uint32_t>(fs.team));
                LOG(info) << "research: team " << static_cast<int>(fs.team) << " collected missiles ("
                          << int(loadout.missileAmmo) << " on the rack)";
            }
        }
        // Lose every lab and the bar simply stalls where it stood.
        else if (tr.labs > 0) {
            fs.researchProgress += PROGRESS_PER_LAB_PER_TICK * static_cast<float>(tr.labs);
            if (fs.researchProgress >= 1.f) {
                fs.researchProgress = 1.f;
                fs.upgradeReady = true;
            }
        }

        tr.progress = fs.researchProgress;
        tr.ready = fs.upgradeReady;
    });

    // FactionState is server-only and a Lab's Structure is replicated, so the
    // labs carry the copy the client's glow reads.
    m_registry.each([&](Structure& s, const Team& team) {
        if (s.type != StructureType::Lab || team.id == TeamId::None) return;
        const TeamResearch& tr = byTeam[static_cast<std::size_t>(team.id)];
        s.researchProgress = tr.progress;
        s.upgradeReady = tr.ready;
    });
}

} // namespace Gravitaris
