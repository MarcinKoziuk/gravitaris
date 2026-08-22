#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/gravitaris.hpp>
#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/planet-attachment.hpp>
#include <gravitaris/game/component/rebuild-lockout.hpp>
#include <gravitaris/game/component/structure.hpp>
#include <gravitaris/game/event/death-report.hpp>
#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/system/combat/death-system.hpp>

namespace Gravitaris {

using Magnum::Vector2d;


static const char* StructureTypeName(StructureType type);
static const char* DamageCauseName(DamageCause cause);

// What a hull throws off when it comes apart. The lifetime matches
// ShipControlsSystem::BULLET_LIFETIME_SECONDS -- frags should linger exactly
// as long as a fired round, not vanish early.
static constexpr ShrapnelBurst HULL_SHRAPNEL{
        /*count=*/12,
        /*speedMin=*/120.0,
        /*speedMax=*/240.0,
        /*lifetimeSeconds=*/3.0,
        /*damage=*/8.f,
};

DeathSystem::DeathSystem(flecs::world& registry, EntitySpawner& entitySpawner, GameEventQueue& eventQueue,
                         const EconomyConfig& config)
        : m_registry(registry)
        , m_entitySpawner(entitySpawner)
        , m_eventQueue(eventQueue)
        , m_config(config)
{}

void DeathSystem::Update(std::uint64_t step)
{
    // Collect first, mutate after: exploding spawns entities and destruct
    // removes them, neither of which is safe mid-iteration.
    std::vector<flecs::entity> dead;
    m_registry.each([&](flecs::entity entity, Damageable& dmg) {
        if (dmg.hp > 0.f) return;
        // A cheated hull is not spared the damage, only its conclusion: it
        // comes back off the floor every tick something drives it through.
        if (dmg.invulnerable) {
            dmg.hp = dmg.maxHp;
            return;
        }
        dead.push_back(entity);
    });

    for (flecs::entity ship : dead) {
        LogStructureDeath(ship, step);
        StartRebuildLockout(ship);
        ReportDeath(ship);
        Explode(ship, step);
        ship.destruct();
    }
}

void DeathSystem::ReportDeath(flecs::entity ship)
{
    if (m_onDeath.slot_count() == 0) return;

    // Ships only: a Base or a Colony coming down is the conquest game's news,
    // not the kill feed's.
    const Team* team = ship.try_get<Team>();
    if (!team || ship.has<Structure>()) return;

    const Damageable& dmg = ship.get<Damageable>();

    DeathReport report;
    report.victimTeam = team->id;
    report.killerTeam = dmg.lastDamageTeam;
    report.killerPilotId = dmg.lastDamagePilotId;
    report.cause = dmg.lastDamageCause;
    m_onDeath(report);
}

// The wreck is about to be destructed, so what it was and where it stood has
// to be copied onto the planet now -- afterwards nothing can answer either.
void DeathSystem::StartRebuildLockout(flecs::entity structure)
{
    const Structure* what = structure.try_get<Structure>();
    if (!what || m_config.rebuild.lockoutTicks == 0) return;

    std::uint32_t planetNetId = 0;
    if (const PlanetSurfaceAttachment* surface = structure.try_get<PlanetSurfaceAttachment>()) {
        planetNetId = surface->planetNetId;
    }
    else if (const PlanetOrbitAttachment* orbit = structure.try_get<PlanetOrbitAttachment>()) {
        planetNetId = orbit->planetNetId;
    }
    if (planetNetId == 0) return;

    flecs::entity planet = m_entitySpawner.EntityForNetId(planetNetId);
    if (!planet.is_alive()) return;

    if (!planet.has<RebuildLockout>()) planet.emplace<RebuildLockout>();
    RebuildLockout& lockout = planet.get_mut<RebuildLockout>();
    std::uint32_t& ticks = lockout.ticks[static_cast<std::size_t>(what->type)];
    ticks = std::max(ticks, m_config.rebuild.lockoutTicks);
}

void DeathSystem::Explode(flecs::entity ship, std::uint64_t step)
{
    const Transform* transf = ship.try_get<Transform>();
    if (!transf) return;

    // Emitted before the caller destructs the ship, so the event can still
    // resolve its NetId.
    m_eventQueue.Emit(GameEventType::Explosion, ship,
                      Magnum::Vector2{static_cast<float>(transf->pos.x()),
                                      static_cast<float>(transf->pos.y())});

    m_entitySpawner.SpawnShrapnel(transf->pos, transf->vel, step, ship.id(), HULL_SHRAPNEL);
}

// A structure is absent from the kill feed by design (see ReportDeath), and
// carries no other trace of having gone: this is the only record that a
// complex came down, and which of the two silent causes -- the sector or
// somebody's guns -- took it.
void DeathSystem::LogStructureDeath(flecs::entity entity, std::uint64_t step)
{
    const Structure* structure = entity.try_get<Structure>();
    if (!structure) return;

    const Team* team = entity.try_get<Team>();
    const Damageable* dmg = entity.try_get<Damageable>();
    const Transform* transf = entity.try_get<Transform>();

    LOG(info) << "structure down: " << StructureTypeName(structure->type) << " ("
              << (team ? TeamDisplayName(team->id) : "unowned") << ") at ("
              << (transf ? transf->pos.x() : 0.) << ", " << (transf ? transf->pos.y() : 0.)
              << ") tick " << step << ", cause " << (dmg ? DamageCauseName(dmg->lastDamageCause) : "?")
              << ", last hit by " << (dmg ? TeamDisplayName(dmg->lastDamageTeam) : "") << " ("
              << (dmg ? dmg->lastDamagePilotId : 0u) << ")";
}

static const char* StructureTypeName(StructureType type)
{
    switch (type) {
    case StructureType::Base: return "Base";
    case StructureType::Colony: return "Colony";
    case StructureType::Lab: return "Lab";
    case StructureType::CommCenter: return "CommCenter";
    case StructureType::HighPort: return "HighPort";
    }
    return "?";
}

static const char* DamageCauseName(DamageCause cause)
{
    switch (cause) {
    case DamageCause::Unknown: return "unknown";
    case DamageCause::Gunfire: return "gunfire";
    case DamageCause::Missile: return "missile";
    case DamageCause::Ram: return "ram";
    case DamageCause::Crash: return "crash";
    case DamageCause::Debris: return "debris";
    case DamageCause::Star: return "star";
    }
    return "?";
}

} // namespace Gravitaris
