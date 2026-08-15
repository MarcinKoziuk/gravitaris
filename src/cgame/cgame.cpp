#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <gravitaris/game/logging.hpp>

#include <Magnum/Math/Matrix3.h>
#include <Magnum/Math/Functions.h>

#include <gravitaris/game/resource/common/resource-loader.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/gravity-source.hpp>
#include <gravitaris/game/component/planet.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/missile.hpp>
#include <gravitaris/game/component/ship-loadout.hpp>
#include <gravitaris/game/component/research-access.hpp>
#include <gravitaris/game/component/pilot-account.hpp>
#include <gravitaris/game/component/faction-state.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/structure.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/resource/body.hpp>
#include <gravitaris/game/system/combat/damage-system.hpp>
#include <gravitaris/game/system/ship/ship-controls-system.hpp>
#include <gravitaris/game/event/death-report.hpp>
#include <gravitaris/game/net/snapshot.hpp>
#include <gravitaris/game/net/protocol.hpp>
#include <gravitaris/game/cheat/cheat-console.hpp>

#include <gravitaris/cgame/fx/hit-flash-system.hpp>
#include <gravitaris/cgame/team-color.hpp>
#include <gravitaris/cgame/component/shield-flash.hpp>
#include <gravitaris/cgame/component/hit-outline.hpp>

#include <gravitaris/cgame/resource/shape.hpp>
#include <gravitaris/cgame/spawner/centity-spawner.hpp>
#include <gravitaris/cgame/cgame.hpp>

namespace Gravitaris {

static std::vector<std::string> SlotCategories(const std::string& label);
static SlotFamily SlotFamilyOf(const std::vector<std::string>& categories);
static std::uint8_t SlotIndexOf(const std::string& label);

namespace {

// One planet's worth of pull, in world units/s^2 -- the unit the G readout
// reports in, so standing on a planet reads 1 rather than a bare 184 that
// means nothing without knowing the gravity constant. Derived from the
// standard planet (data/models/planets/simple) at the default gravity
// multiplier: GRAVITY_CONSTANT * 3.5 * (mass 100000 * multiplier 0.4) /
// radius^2, radius = the svg's 299.5 at scale 0.4. Retuning that planet or the
// default multiplier means retuning this, or the readout stops meaning "one
// planet". The live multiplier deliberately isn't divided back out: cranking
// gravity to 3x should read 3g.
constexpr float SURFACE_GRAVITY = 195.f;

// How far behind the estimated server tick remote entities render (see
// CGame::m_interpDelaySeconds) -- smooths jitter at the cost of latency.
// Fractional throughout, and fed by the smoothed clock rather than the
// integer one: see SnapshotInterpolator::Compute's `renderTick` doc comment.
double ComputeRenderTick(double estimatedServerTick, float interpDelaySeconds, std::uint32_t tickRate)
{
    return std::max(estimatedServerTick - static_cast<double>(interpDelaySeconds) * tickRate, 0.0);
}

} // namespace

CGame::CGame(IFilesystem &filesystem, float contentScale)
    : Game(filesystem, CreateEntitySpawner())
    , m_simpleModelRenderer(m_registry, filesystem, m_resourceLoader)
    , m_modelRenderer2(m_registry, filesystem, m_resourceLoader)
    , m_mirrorRenderer2(m_mirrorWorld, filesystem, m_resourceLoader)
    , m_snapshotApplier(m_mirrorWorld, m_resourceLoader)
    , m_starfieldRenderer(filesystem)
    , m_minimapRenderer(filesystem, contentScale)
    , m_compassRenderer(filesystem, m_modelRenderer2, contentScale)
    , m_shipViewRenderer(m_modelRenderer2, contentScale)
    , m_audioSystem(m_registry, m_resourceLoader, m_eventQueue, m_upgradeCatalog)
    , m_hitFlashSystem(m_registry, m_eventQueue, *m_entitySpawner)
    , m_cameraDirector(Defaults::cameraZoom)
    , m_indicatorRenderer(m_resourceLoader)
    , m_laserRenderer(filesystem)
    , m_clientPrediction(m_registry, m_physicsSystem, *m_entitySpawner, m_eventQueue, m_resourceLoader,
                         m_upgradeCatalog)
    , m_cosmeticBulletDespawner(m_registry, m_mirrorWorld)
    , m_autopilot(m_registry, m_physicsSystem)
{
    m_modelRenderer2.SetReferenceZoom(Defaults::cameraZoom);
    m_mirrorRenderer2.SetReferenceZoom(Defaults::cameraZoom);

    SetShipWeightMultiplier(Defaults::shipWeight);

    // Loaded here (after both renderers' OnCreate<Model> observers exist, in
    // their own constructors above) so both m_modelRenderer2 and
    // m_mirrorRenderer2 bake it for SubmitPlanetOwnershipMarkers.
    m_teamMarkerModel = m_resourceLoader.Load<Model>("models/ui/team-marker"_id);
    m_shipSchematicModel = m_resourceLoader.Load<Model>("models/ui/fighter-1-schematic"_id);
    m_shipSchematicShape = m_resourceLoader.Load<Shape>("models/ui/fighter-1-schematic"_id);

    m_modelRenderer2.SetExtraPasses(ShieldPasses());
    m_mirrorRenderer2.SetExtraPasses(ShieldPasses());

    // Kill feed. As a net client this sim only holds the own predicted ship,
    // and the deaths that count are the server's -- which arrive as chat like
    // everyone else's lines, so writing one here too would double them up.
    OnDeath().connect([this](const DeathReport& report) {
        if (m_netClient) return;
        PushChatLine("", TeamId::None, FormatDeathMessage(report));
    });
}

SceneView CGame::CurrentSceneView()
{
    if (m_netClient) return SceneView{m_registry, &m_mirrorWorld, &m_mirrorRenderer2};
    return SceneView{m_registry, nullptr, &m_modelRenderer2};
}

std::optional<flecs::entity> CGame::CameraSubject()
{
    if (m_spectateTarget.is_alive()) return m_spectateTarget;
    return GetPlayer();
}

std::optional<float> CGame::GetHullFraction()
{
    const std::optional<flecs::entity> subject = CameraSubject();
    if (!subject) return std::nullopt;

    const Damageable* damageable = subject->try_get<Damageable>();
    if (!damageable || damageable->maxHp <= 0.f) return std::nullopt;

    return std::clamp(damageable->hp / damageable->maxHp, 0.f, 1.f);
}

std::optional<int> CGame::GetMissileAmmo()
{
    const std::optional<flecs::entity> subject = CameraSubject();
    if (!subject) return std::nullopt;

    const ShipLoadout* loadout = subject->try_get<ShipLoadout>();
    // A structure/planet being spectated has no rack, and neither has a hull
    // with no launcher in any bay -- the tier alone is what it has paid for,
    // not what it is carrying.
    if (!loadout || MissileBaysFitted(*loadout) == 0) return std::nullopt;

    return static_cast<int>(loadout->missileAmmo);
}

// The magazine of the camera subject's cannon, and which primary its pilot has
// the trigger on. Empty when the hull carries no cannon at all -- the row goes
// away rather than reading zero, which would say the opposite of the truth.
std::optional<CGame::CannonReadout> CGame::GetCannonReadout()
{
    const std::optional<flecs::entity> subject = CameraSubject();
    if (!subject) return std::nullopt;

    const ShipLoadout* loadout = subject->try_get<ShipLoadout>();
    // Mounted, not merely owned: a hull whose heavy mounts have all been
    // stripped has no magazine to show, whatever tier it once paid for.
    if (!loadout || MountsArmedWith(*loadout, MountArm::Heavy) == 0) return std::nullopt;

    CannonReadout readout;
    readout.ammo = static_cast<int>(loadout->cannonAmmo);
    readout.capacity = m_upgradeCatalog.ResolveStats(loadout->levels).cannonCapacity;
    // What the pilot asked for, not what fired: a dry cannon still reads as
    // selected, which is what makes the empty bar the explanation.
    if (const Controls* controls = subject->try_get<Controls>()) {
        readout.mode = static_cast<int>(controls->activeWeapon);
    }
    return readout;
}

std::optional<float> CGame::GetSpeed()
{
    const std::optional<flecs::entity> subject = CameraSubject();
    if (!subject) return std::nullopt;

    const Transform* transf = subject->try_get<Transform>();
    if (!transf) return std::nullopt;

    return static_cast<float>(transf->vel.length());
}

Vector2d CGame::GravityAt(const Vector2d& pos)
{
    const double strength = PhysicsSystem::GRAVITY_CONSTANT * GetGravityMultiplier();

    Vector2d accel;
    CurrentSceneView().Each([&](flecs::entity, const GravitySource& gs, const Transform& srcTransf) {
        const Vector2d d = srcTransf.pos - pos;
        const double dist2 = d.dot();
        if (dist2 < 1e-6) return; // the subject itself, or something sitting on it
        accel += d * (strength * gs.mass * gs.multiplier / (dist2 * std::sqrt(dist2)));
    });
    return accel;
}

Magnum::Vector2 CGame::SubjectGravity()
{
    const std::optional<flecs::entity> subject = CameraSubject();
    const Transform* transf = subject ? subject->try_get<Transform>() : nullptr;
    if (!transf) return {};

    const Vector2d accel = GravityAt(transf->pos);
    return Magnum::Vector2{static_cast<float>(accel.x()), static_cast<float>(accel.y())};
}

std::optional<float> CGame::GetGravityAccel()
{
    const std::optional<flecs::entity> subject = CameraSubject();
    if (!subject) return std::nullopt;

    const Transform* transf = subject->try_get<Transform>();
    if (!transf) return std::nullopt;

    return static_cast<float>(GravityAt(transf->pos).length()) / SURFACE_GRAVITY;
}

int CGame::GetMissileCapacity()
{
    const std::optional<flecs::entity> subject = CameraSubject();
    const ShipLoadout* loadout = subject ? subject->try_get<ShipLoadout>() : nullptr;
    if (!loadout) return 0;

    return m_upgradeCatalog.ResolveStats(loadout->levels).missileCapacity;
}

CGame::ShieldReadout CGame::GetShieldReadout()
{
    const std::optional<flecs::entity> subject = CameraSubject();
    if (!subject) return {};

    const ShipLoadout* loadout = subject->try_get<ShipLoadout>();
    if (!loadout) return {};

    const ShipStats stats = m_upgradeCatalog.ResolveStats(loadout->levels);

    std::vector<float> segments;
    if (IsPlated(*loadout)) {
        const float capacity = PerPlate(*loadout, stats.shieldCapacity);
        for (std::uint8_t i = 0; capacity > 0.f && i < loadout->plateCount; ++i) {
            segments.push_back(std::clamp(loadout->plates[i] / capacity, 0.f, 1.f));
        }
    }

    return ShieldReadout{loadout->shieldHp, stats.shieldCapacity, loadout->levels.shieldType,
                         std::move(segments)};
}

Magnum::Vector2 CGame::ViewportToWorld(const Magnum::Vector2& viewportPixel)
{
    const Camera& camera = m_cameraDirector.GetCamera();
    const float pixelsPerUnit = std::max(camera.GetZoom() * m_contentScale, 1e-6f);
    return camera.GetPosition() + (viewportPixel - m_viewportSize * 0.5f) / pixelsPerUnit;
}

std::optional<std::uint16_t> CGame::AimAtPoint(const Magnum::Vector2& worldPoint)
{
    const std::optional<flecs::entity> player = GetPlayer();
    if (!player || !player->is_alive()) return std::nullopt;

    const Transform* transf = player->try_get<Transform>();
    if (!transf) return std::nullopt;

    // The hull as the pilot last SAW it, not as the sim has since left it. The
    // cursor was turned into `worldPoint` through the camera of the frame on
    // screen, and this is that same frame's ship -- see m_aimOrigin.
    const Magnum::Vector2d from = m_aimOrigin.value_or(transf->pos);
    const Magnum::Vector2d offset{static_cast<double>(worldPoint.x()) - from.x(),
                                  static_cast<double>(worldPoint.y()) - from.y()};
    if (offset.dot() < 1e-9) return std::nullopt;

    return PackAim(std::atan2(offset.y(), offset.x()));
}

CGame::CapacitorReadout CGame::GetCapacitorReadout()
{
    const std::optional<flecs::entity> subject = CameraSubject();
    if (!subject) return {};

    const ShipLoadout* loadout = subject->try_get<ShipLoadout>();
    const Controls* controls = subject->try_get<Controls>();
    if (!loadout || !controls) return {};

    const ShipStats stats = m_upgradeCatalog.ResolveStats(loadout->levels);
    if (stats.capacitorCharge <= 0.f) return {};

    // One track for one bank: it drains while something is drawing and creeps
    // back when nothing is, and `charging` only says which of the two is
    // happening.
    const float left = std::clamp(1.f - controls->capacitorSpent / stats.capacitorCharge, 0.f, 1.f);

    // A spectated unit's bank is never simulated on this side -- only the
    // burning bit reaches us (SnapshotApplier) -- so its bar reports that it is
    // burning without pretending to know how much is left.
    if (controls->boosting) {
        return CapacitorReadout{true, controls->capacitorSpent > 0.f ? left : 1.f, false};
    }
    return CapacitorReadout{true, left, left < 1.f};
}

// What the port says when it turns a refit away. Worded as the yard talking
// rather than as a validation error, because a refusal is never the player
// getting it wrong: the panel only offers a rank it believes is buyable, so
// one of these means the world moved during the round trip.
static const char* DenialLine(TechNodeState reason)
{
    switch (reason) {
    case TechNodeState::NotUnlocked:  return "that pattern isn't on file here";
    case TechNodeState::Unaffordable: return "your account won't cover it";
    case TechNodeState::NeedsLanding: return "you've drifted out of the yard";
    case TechNodeState::Held:         return "you're already carrying it";
    case TechNodeState::Locked:       return "you're missing what it bolts onto";
    case TechNodeState::Available:    break;
    }
    return "order refused";
}

std::optional<std::string> CGame::TakeRefitDenial()
{
    const std::optional<flecs::entity> player = GetPlayer();
    if (!player) return std::nullopt;
    const NetId* ownNetId = player->try_get<NetId>();
    if (!ownNetId) return std::nullopt;

    std::optional<std::uint32_t> reason;
    GetEventQueue().ConsumeSince(m_lastDenialSeq, [&](const GameEvent& event) {
        m_lastDenialSeq = std::max(m_lastDenialSeq, event.seq);
        if (event.type != GameEventType::RefitDenied) return;
        if (event.sourceNetId != ownNetId->value) return;
        reason = event.param;
    });

    if (!reason) return std::nullopt;

    std::string line = DenialLine(static_cast<TechNodeState>(*reason));
    PushChatLine("PORT AUTHORITY", TeamId::None, line);
    return line;
}

CGame::TechReadout CGame::GetTechReadout()
{
    TechReadout readout;

    const std::optional<flecs::entity> player = GetPlayer();
    if (!player) return readout;

    const Team* playerTeam = player->try_get<Team>();
    if (!playerTeam || playerTeam->id == TeamId::None) return readout;

    if (m_netClient) {
        // Connected: both numbers arrive on the wire, since neither the
        // faction's pool nor the pilot's account exists in this registry.
        readout.supplies = m_ownSupplies;
        readout.atLab = m_ownAtLab;
        for (const FactionSnapshot& faction : m_factionSnapshots) {
            if (faction.team == static_cast<std::uint8_t>(playerTeam->id)) readout.tech = faction.techPoints;
        }
        return readout;
    }

    if (const ResearchAccess* access = player->try_get<ResearchAccess>()) readout.atLab = access->atLab;
    m_registry.each([&](const FactionState& faction) {
        if (faction.team == playerTeam->id) readout.tech = faction.techPoints;
    });
    if (const PilotRef* ref = player->try_get<PilotRef>()) {
        m_registry.each([&](const PilotAccount& account) {
            if (account.pilotId == ref->pilotId) readout.supplies = account.supplies;
        });
    }
    return readout;
}

CGame::ResupplyOffer CGame::GetResupplyOffer()
{
    const std::optional<flecs::entity> player = GetPlayer();
    const ShipLoadout* loadout = player ? player->try_get<ShipLoadout>() : nullptr;
    if (!loadout) return {};

    const TechReadout readout = GetTechReadout();
    const std::uint32_t cost = m_upgradeCatalog.ResupplyCost(*loadout);
    return ResupplyOffer{static_cast<int>(cost),
                         cost > 0 && readout.atLab && readout.supplies >= cost};
}

// The faction's unlock track, from whichever side of the wire this build is
// on. Both trees are drawn against it: the permanent one shows what it holds,
// the ship one what it permits.
TechUnlocks CGame::OwnUnlocks()
{
    TechUnlocks unlocked;

    const std::optional<flecs::entity> player = GetPlayer();
    if (!player) return unlocked;
    const Team* playerTeam = player->try_get<Team>();
    if (!playerTeam || playerTeam->id == TeamId::None) return unlocked;

    if (m_netClient) {
        for (const FactionSnapshot& faction : m_factionSnapshots) {
            if (faction.team == static_cast<std::uint8_t>(playerTeam->id)) unlocked = faction.unlocked;
        }
        return unlocked;
    }

    m_registry.each([&](const FactionState& faction) {
        if (faction.team == playerTeam->id) unlocked = faction.unlocked;
    });
    return unlocked;
}

// Which magazine a fitting draws its round count from, or None for one that
// reports none. The weapon that feeds from a pool and the locker that deepens
// it both answer with that pool -- see TechNode::ammo.
static AmmoPool PoolOf(const UpgradeDef& def)
{
    switch (def.kind) {
    case UpgradeKind::CannonTier:  return AmmoPool::Cannon;
    case UpgradeKind::MissileTier: return AmmoPool::Missile;
    case UpgradeKind::AmmoStore:   return def.ammo.pool;
    default:                       return AmmoPool::None;
    }
}

// Four role branches over the kinds. A map rather than a toml field: there is
// nothing to author yet, and the day a branch stops matching a kind is the day
// it earns one.
//
// The beams have a column of their own rather than sitting with the guns:
// they are aimed rather than pointed, they are paid for out of the bank rather
// than a magazine, and a pilot choosing between them and the cannons is
// choosing between two ways to fly, not between two guns.
static int BranchOf(UpgradeKind kind)
{
    switch (kind) {
    case UpgradeKind::Capacitor:
    case UpgradeKind::EngineTier: return 1; // MOBILITY
    case UpgradeKind::Shield:     return 2; // DEFENSE
    case UpgradeKind::LaserTier:  return 3; // BEAMS
    default:                      return 0; // WEAPONS -- rounds included
    }
}

std::vector<CGame::TechNode> CGame::GetTechTree()
{
    std::vector<TechNode> nodes;

    const std::optional<flecs::entity> player = GetPlayer();
    const ShipLoadout* loadout = player ? player->try_get<ShipLoadout>() : nullptr;
    if (!loadout) return nodes;

    const TechReadout readout = GetTechReadout();
    const TechUnlocks unlocked = OwnUnlocks();
    const UpgradeCatalog::ShipContext context{loadout, &unlocked, readout.supplies, readout.atLab};
    const ShipStats stats = m_upgradeCatalog.ResolveStats(loadout->levels);

    const std::vector<UpgradeDef>& defs = m_upgradeCatalog.Defs();
    nodes.reserve(defs.size() * 2);
    for (std::size_t i = 0; i < defs.size(); ++i) {
        const UpgradeDef& def = defs[i];
        const UpgradeCatalog::TreeSlot slot = m_upgradeCatalog.SlotOf(i);
        const std::uint8_t ranks = UpgradeCatalog::RankCount(def);

        for (const TechTab tab : {TechTab::Ship, TechTab::Permanent}) {
            // Stock hardware has no rank for a faction to learn, so it appears
            // on the hull's board and nowhere else.
            if (tab == TechTab::Permanent && !def.researched) continue;

            TechNode node;
            node.id = def.id;
            node.tab = tab;
            node.col = slot.col;
            node.row = slot.row;
            node.name = def.name;
            node.icon = def.icon;
            node.slots = def.slots;
            node.branch = BranchOf(def.kind);
            node.description = def.description;
            node.maxRank = ranks;
            node.cap = m_upgradeCatalog.UnlockedRank(def, unlocked);
            node.requiresId = def.requiresId;
            node.rank = tab == TechTab::Ship ? UpgradeCatalog::LevelOf(def, loadout->levels) : node.cap;
            if (tab == TechTab::Ship) {
                const AmmoPool pool = PoolOf(def);
                if (pool == AmmoPool::Cannon) {
                    node.ammo = static_cast<int>(loadout->cannonAmmo);
                    node.ammoCapacity = stats.cannonCapacity;
                }
                else if (pool == AmmoPool::Missile) {
                    node.ammo = static_cast<int>(loadout->missileAmmo);
                    node.ammoCapacity = stats.missileCapacity;
                }
            }

            node.ranks.reserve(ranks);
            for (std::uint8_t rank = 1; rank <= ranks; ++rank) {
                TechRank entry;
                if (tab == TechTab::Ship) {
                    entry.cost = UpgradeCatalog::SupplyCostOf(def, rank);
                    entry.state = m_upgradeCatalog.ShipState(def, rank, context);
                }
                else {
                    entry.cost = UpgradeCatalog::TechCostOf(def, rank);
                    entry.state = m_upgradeCatalog.PermanentState(def, rank, unlocked, readout.tech);
                }
                node.ranks.push_back(entry);
            }
            nodes.push_back(std::move(node));
        }
    }
    return nodes;
}

std::optional<CGame::ResearchReadout> CGame::GetResearchReadout()
{
    const std::optional<flecs::entity> player = GetPlayer();
    if (!player) return std::nullopt;
    const Team* playerTeam = player->try_get<Team>();
    if (!playerTeam || playerTeam->id == TeamId::None) return std::nullopt;

    ResearchReadout readout;
    CurrentSceneView().Each([&](const Structure& structure, const Team& team) {
        if (structure.type != StructureType::Lab || team.id != playerTeam->id) return;
        ++readout.labs;
        // Every lab of a faction carries the same pooled copy, so the last one
        // seen is as good as any.
        readout.progress = std::clamp(structure.researchProgress, 0.f, 1.f);
    });
    if (readout.labs == 0) return std::nullopt;

    // The bar advances once per lab per tick (ResearchSystem), so building a
    // second lab halves the wait -- which is the whole reason to show a time
    // rather than just the bar.
    readout.secondsRemaining = static_cast<float>((1. - readout.progress)
                                                  * GetEconomyConfig().research.secondsPerTech
                                                  / static_cast<double>(readout.labs));
    return readout;
}

void CGame::SubmitChat(const std::string& text)
{
    const std::size_t begin = text.find_first_not_of(" \t");
    if (begin == std::string::npos) return;
    const std::string trimmed = text.substr(begin, text.find_last_not_of(" \t") - begin + 1);

    if (m_netClient) {
        // Cheats included: the server owns the sim, so a command run against
        // this client's copy of the world would be overwritten by the next
        // snapshot. The line comes back from the server like everyone else's,
        // which is also what proves it was actually sent.
        m_netClient->SendChat(trimmed);
        return;
    }

    const std::optional<flecs::entity> player = GetPlayer();
    const Team* team = player ? player->try_get<Team>() : nullptr;
    const TeamId teamId = team ? team->id : TeamId::None;

    if (IsCheatCommand(trimmed)) {
        PushChatLine(m_playerName, teamId, trimmed);
        for (std::string& line :
             RunCheatCommand(*this, player.value_or(flecs::entity{}), teamId, trimmed).reply) {
            PushChatLine("", TeamId::None, std::move(line));
        }
        return;
    }

    PushChatLine(m_playerName, teamId, trimmed);
}

std::vector<CGame::ChatLine> CGame::GetChatLog() const
{
    return std::vector<ChatLine>(m_chatLog.begin(), m_chatLog.end());
}

void CGame::DrainChat()
{
    if (!m_netClient) return;

    for (ChatMessagePacket& message : m_netClient->TakeChatMessages()) {
        PushChatLine(std::move(message.sender), message.team, std::move(message.text));
    }
}

void CGame::PushChatLine(std::string sender, TeamId team, std::string text)
{
    m_chatLog.push_back(ChatLine{std::move(sender), team, std::move(text)});
    while (m_chatLog.size() > CHAT_HISTORY_LINES) m_chatLog.pop_front();
    ++m_chatRevision;

    m_audioSystem.PlayChatBlip();
}

void CGame::CycleSpectate(int direction)
{
    // NetId order, so the roster reads the same however flecs happens to
    // iterate and whichever world an entity lives in.
    std::vector<std::pair<std::uint32_t, flecs::entity>> units;
    CurrentSceneView().Each([&](flecs::entity ent, const Transform&, const Team&, const Damageable&,
                                const NetId& netId) {
        if (ent.has<Structure>() || ent.has<Planet>()) return;
        units.emplace_back(netId.value, ent);
    });
    if (units.empty()) return;
    std::sort(units.begin(), units.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    const std::optional<flecs::entity> subject = CameraSubject();
    std::size_t index = 0;
    for (std::size_t i = 0; i < units.size(); ++i) {
        if (subject && units[i].second == *subject) index = i;
    }

    const int count = static_cast<int>(units.size());
    const int next = ((static_cast<int>(index) + direction) % count + count) % count;

    const std::optional<flecs::entity> player = GetPlayer();
    m_spectateTarget = (player && units[next].second == *player) ? flecs::entity() : units[next].second;
}

void CGame::SubmitPlanetOwnershipMarkers(const SceneView& view)
{
    if (!view.overlays) return;

    static constexpr float MARKER_WORLD_SIZE = 22.f;
    view.Each([&](const Planet&, const Transform& t, const Team& team) {
        if (team.id == TeamId::None) return;
        const Magnum::Vector2 pos{static_cast<float>(t.pos.x()), static_cast<float>(t.pos.y())};
        const Matrix3 transform =
                Matrix3::translation(pos) * Matrix3::scaling({MARKER_WORLD_SIZE, MARKER_WORLD_SIZE});
        view.overlays->SubmitOverlay(m_teamMarkerModel.Id(), transform, Magnum::Vector3{TeamColor(team.id)});
    });
}

std::vector<ModelRenderer2::ExtraPass> CGame::ShieldPasses()
{
    // How far a shield's own color is lifted off the flat team color. A red
    // ship's field reads pink, a blue one's ice-blue -- still unmistakably
    // that team's, but plainly not hull.
    static constexpr float TEAM_LIFT = 0.45f;

    // A nearly spent shield is a faint outline, a full one is bright -- the
    // same reading the sidebar's bar gives, without looking away.
    static constexpr float FLOOR = 0.3f;

    // At rest a shield is a ghost over the hull rather than a second outline
    // competing with it; a strike drives it to fully opaque (see the shader).
    static constexpr float REST_OPACITY = 0.25f;

    // `charge` 0..1, `glow` 0..1; fills in the color, opacity and strike
    // bearing every shield pass shares, and refuses the entity outright when
    // there is nothing left to draw.
    const auto shade = [](const ShieldFlash* flash, ModelRenderer2::InstanceStyle& style,
                          float charge, float glow) {
        if (charge <= 0.f) return false;

        style.color = Magnum::Math::lerp(style.color, Magnum::Vector3{1.f, 1.f, 1.f}, TEAM_LIFT)
                      * (FLOOR + (1.f - FLOOR) * std::min(charge, 1.f));
        style.flash = 0.f; // the hull's flash is not the shield's

        const Magnum::Vector2 dir = flash ? flash->dir : Magnum::Vector2{0.f, -1.f};
        style.shieldFx = Magnum::Vector4{dir.x(), dir.y(), glow, REST_OPACITY};
        return true;
    };

    std::vector<ModelRenderer2::ExtraPass> passes;

    passes.push_back(ModelRenderer2::ExtraPass{
            SHIELD_TAG,
            [this, shade](flecs::entity entity, ModelRenderer2::InstanceStyle& style) {
                const ShipLoadout* loadout = entity.try_get<ShipLoadout>();
                if (!loadout || loadout->levels.shieldType != ShieldType::Bubble) return false;

                const ShipStats stats = m_upgradeCatalog.ResolveStats(loadout->levels);
                if (stats.shieldCapacity <= 0.f) return false;

                const ShieldFlash* flash = entity.try_get<ShieldFlash>();
                return shade(flash, style, loadout->shieldHp / stats.shieldCapacity,
                             flash ? flash->amount : 0.f);
            },
            ShieldGlow::Directional});

    for (std::size_t plate = 0; plate < MAX_SHIELD_PLATES; ++plate) {
        passes.push_back(ModelRenderer2::ExtraPass{
                PlatingTag(plate),
                [this, shade, plate](flecs::entity entity, ModelRenderer2::InstanceStyle& style) {
                    const ShipLoadout* loadout = entity.try_get<ShipLoadout>();
                    if (!loadout || !IsPlated(*loadout) || plate >= loadout->plateCount) return false;

                    const ShipStats stats = m_upgradeCatalog.ResolveStats(loadout->levels);
                    const float capacity = PerPlate(*loadout, stats.shieldCapacity);
                    if (capacity <= 0.f) return false;

                    // Whole-plate, not a bearing falloff: the sim resolved the
                    // hit against one specific plate, so that plate lights and
                    // its neighbours do not. A plate with no charge left is
                    // already refused below and stays dark.
                    const ShieldFlash* flash = entity.try_get<ShieldFlash>();
                    const bool struck = flash && flash->plate == static_cast<std::int8_t>(plate);

                    // Each plate reads its own charge, so a ship burned
                    // through on one side shows it.
                    return shade(flash, style, loadout->plates[plate] / capacity,
                                 struck ? flash->amount : 0.f);
                },
                ShieldGlow::Flat});
    }

    return passes;
}

void CGame::RenderMinimap()
{
    // No subject at all during round setup, and none between death and
    // respawn (or in MP before the first snapshot). The map is worth drawing
    // either way -- the sector is what the setup screen is there to show --
    // so only the "you are here" marker is conditional.
    const std::optional<flecs::entity> subject = CameraSubject();
    const Transform* transform = subject ? subject->try_get<Transform>() : nullptr;

    std::optional<Magnum::Vector2> subjectPos;
    if (transform) {
        subjectPos = Magnum::Vector2{static_cast<float>(transform->pos.x()),
                                     static_cast<float>(transform->pos.y())};
    }

    const Team* subjectTeam = subject ? subject->try_get<Team>() : nullptr;

    const Camera& camera = m_cameraDirector.GetCamera();
    const Magnum::Vector2 viewHalfExtent =
            GetDesignViewportSize() / (2.f * std::max(camera.GetZoom(), 1e-3f));

    // In MP, everything but the own ship lives in m_mirrorWorld (see
    // m_netClient's field comment) -- sweep it too so remote ships/planets
    // show up exactly like single-player's real registry entities do.
    const SceneView view = CurrentSceneView();
    m_minimapRenderer.Render(view, MinimapCenter(), subjectPos,
                             subjectTeam ? subjectTeam->id : TeamId::None,
                             camera.GetPosition(), viewHalfExtent);
}

void CGame::RenderCompass()
{
    CompassRenderer::Subject reading;
    reading.gravity = SubjectGravity();

    const std::optional<flecs::entity> subject = CameraSubject();
    const Transform* transform = subject ? subject->try_get<Transform>() : nullptr;
    const Renderable* renderable = subject ? subject->try_get<Renderable>() : nullptr;

    if (transform) {
        reading.rot = Magnum::Rad{static_cast<float>(double(transform->rot))};
        reading.velocity = Magnum::Vector2{static_cast<float>(transform->vel.x()),
                                           static_cast<float>(transform->vel.y())};
    }
    if (renderable && renderable->model) {
        reading.model = renderable->model.Get();
        reading.modelId = renderable->model.Id();
    }
    if (const Team* team = subject ? subject->try_get<Team>() : nullptr) {
        reading.teamColor = Magnum::Vector3{TeamColor(team->id)};
    }

    m_compassRenderer.Render(reading);
}

void CGame::RenderShipView()
{
    ShipViewRenderer::Subject subject;
    if (m_shipSchematicModel) {
        subject.model = m_shipSchematicModel.Get();
        subject.modelId = m_shipSchematicModel.Id();
    }

    m_shipViewRenderer.Render(subject);
}

std::vector<CGame::ShipSlot> CGame::GetShipSlots()
{
    std::vector<ShipSlot> slots;
    if (!m_shipSchematicShape || !m_shipSchematicModel) return slots;

    // What the subject actually has in each mount. Empty for a spectated
    // structure, which simply leaves every slot reading empty.
    const std::optional<flecs::entity> subject = CameraSubject();
    const ShipLoadout* loadout = subject ? subject->try_get<ShipLoadout>() : nullptr;

    for (const Shape::Marker& marker : m_shipSchematicShape->GetMarkers()) {
        ShipSlot slot;
        slot.name = marker.name;
        slot.categories = SlotCategories(marker.name);
        slot.family = SlotFamilyOf(slot.categories);
        slot.mount = slot.family == SlotFamily::None ? TechPick::NO_MOUNT
                                                     : SlotIndexOf(marker.name);
        if (slot.mount == TechPick::NO_MOUNT) slot.family = SlotFamily::None;

        // What each mounted line would cost in *this* hole, and whether it can
        // go there at all. Asked per hole because holding a line is: light guns
        // in the nose leave them for sale for a wing, and a launcher in the
        // port bay leaves the starboard one empty.
        if (loadout && slot.family != SlotFamily::None) {
            const TechReadout readout = GetTechReadout();
            const TechUnlocks unlocked = OwnUnlocks();

            for (const UpgradeDef& def : m_upgradeCatalog.Defs()) {
                if (UpgradeCatalog::FamilyOf(def) != slot.family) continue;

                std::vector<TechRank> ranks;
                const UpgradeCatalog::ShipContext context{loadout, &unlocked, readout.supplies,
                                                          readout.atLab, slot.mount};
                for (std::uint8_t rank = 1; rank <= def.maxLevel; ++rank) {
                    ranks.push_back(TechRank{UpgradeCatalog::SupplyCostOf(def, rank),
                                             m_upgradeCatalog.ShipState(def, rank, context)});
                }
                slot.rankStates.emplace_back(def.id, std::move(ranks));
            }
        }

        if (loadout && slot.family == SlotFamily::Weapon && slot.mount < loadout->mounts.size()) {
            const MountArm arm = loadout->mounts[slot.mount];
            if (arm != MountArm::None) {
                const UpgradeKind kind = arm == MountArm::Heavy  ? UpgradeKind::CannonTier
                                       : arm == MountArm::Laser ? UpgradeKind::LaserTier
                                                                : UpgradeKind::WeaponTier;
                if (const UpgradeDef* def = m_upgradeCatalog.FindKind(kind)) slot.fittedId = def->id;
            }
        }
        if (loadout && slot.family == SlotFamily::MissileBay
                && MissileBayFitted(*loadout, slot.mount)) {
            if (const UpgradeDef* def = m_upgradeCatalog.FindKind(UpgradeKind::MissileTier)) {
                slot.fittedId = def->id;
            }
        }
        if (loadout && slot.family == SlotFamily::AmmoBay && slot.mount < loadout->ammoBays.size()) {
            if (const UpgradeDef* def = m_upgradeCatalog.FindAmmoStore(loadout->ammoBays[slot.mount])) {
                slot.fittedId = def->id;
            }
        }
        slot.uv = m_shipViewRenderer.PanelUV(*m_shipSchematicModel, m_shipSchematicModel.Id(),
                                             Magnum::Vector2{static_cast<float>(marker.pos.x()),
                                                             static_cast<float>(marker.pos.y())});
        slots.push_back(std::move(slot));
    }
    return slots;
}

void CGame::LookAtMapPoint(const Magnum::Vector2& normalized)
{
    m_cameraDirector.LookAt(MinimapCenter() + normalized * m_minimapRenderer.FrameRadius());
}

void CGame::ConnectToServer(const std::string& wsUrl, TeamId requestedTeam)
{
    m_netTransport = std::make_unique<WebRtcTransport>(WebRtcTransport::Role::Offerer);
    m_simulatedTransport = std::make_unique<SimulatedNetTransport>(*m_netTransport);
    m_netClient = std::make_unique<NetClient>(*m_simulatedTransport, m_playerName);
    m_netClient->SetRequestedTeam(requestedTeam);
    m_ownShipSync.emplace(m_clientPrediction, *m_netClient, m_predictedTickClock);
    m_remoteEventApplier.emplace(*m_netClient, m_eventQueue, m_cosmeticBulletDespawner);
    m_netTransport->ConnectSignaling(wsUrl);
}

bool CGame::TickNetClient(const ControlFlags& flags, const TechPick& techPick)
{
    if (!m_netClient->IsWelcomed()) return false;

    if (m_ownShipSync->DropIfStale()) {
        m_player.reset();
    }
    if (const std::optional<flecs::entity> spawned = m_ownShipSync->SpawnIfConfirmed()) {
        m_player = *spawned;
    }
    if (!m_clientPrediction.HasOwnShip()) return false;

    // Smoothed clock, not the integer estimate: the target is compared
    // against a free-running counter a fraction of a tick at a time now (see
    // PredictedTickClock::Advance), so feeding it the raw staircase would
    // read its steps as drift.
    const double target =
            m_netClient->EstimateCurrentServerTickF() + static_cast<double>(m_netClient->GetInputLeadTicks());
    const PredictedTickClock::AdvanceResult advance = m_predictedTickClock.Advance(target);
    if (advance.skip) {
        // Running ahead of the target -- usually because the input lead was
        // just lowered under us. Give the tick back by simply not existing
        // this call: no step, no input stamped, one frame of the own ship
        // holding still while wall clock closes the gap.
        ++m_netDiagnostics.tickSkipCount;
        return false;
    }
    if (advance.resyncDrift) {
        // A lost tick is permanent backward drift vs. the server's
        // wall-clock-paced step (see PredictedTickClock's own doc comment),
        // and once it exceeds the input-lead window, every input this
        // client sends is stamped in the server's past -- InputSystem drops
        // it as stale, repeat-last-command latches the last consumed flags,
        // and the ship spins/freezes server-side forever while local
        // prediction (and silently-failing reconciliation) keep this client
        // feeling fine.
        LOG(info) << "net: predicted-tick drift of " << *advance.resyncDrift << " ticks (throttled tab?), resyncing "
                  << " -> " << target;
        ++m_netDiagnostics.resyncEventCount;
        m_netDiagnostics.lastResyncDriftTicks = *advance.resyncDrift;
        m_netDiagnostics.driftHistory.Record(static_cast<float>(*advance.resyncDrift));
    }

    const std::uint64_t tick = advance.tick;
    // How far behind this command's own tick the pilot's picture of everyone
    // else is: their ships are drawn at `m_lastRenderTick` (the interpolation
    // delay, plus however far this client is running behind the server), while
    // the command is stamped with the tick this client's OWN ship is predicted
    // at. That difference is the whole of what the server has to undo to
    // resolve a shot where it was aimed -- see LagCompensation.
    m_netClient->SendInput(tick, flags, techPick, ViewDelayTicks(tick));

    if (const std::optional<SnapshotData>& snapshot = m_netClient->GetLatestSnapshot()) {
        m_clientPrediction.Step(tick, flags, snapshot->entities, snapshot->tick, m_netClient->GetYourShipNetId());
        m_factionSnapshots = snapshot->factions;
        m_bulletLifetimeSystem.Update(PHYSICS_DELTA);

        // The own ship is predicted locally, so it never passes through
        // SnapshotApplier -- its hull, its loadout, its yard access and its
        // pilot's Supplies have to be copied off the wire here or the sidebar
        // would report the state it spawned with forever. Through the same
        // ApplyEntityShipState a mirrored remote ship goes through: neither a
        // refit nor damage is predicted, so what the board draws is whatever
        // the server last said, and one hull's copy of the mapping must not be
        // a subset of another's. Nothing here is predicted, so this arriving a
        // round trip late is the whole story of the readouts' latency.
        const std::uint32_t yourShipNetId = m_netClient->GetYourShipNetId();
        if (const std::optional<flecs::entity> player = GetPlayer(); player && yourShipNetId != 0) {
            for (const EntityState& state : snapshot->entities) {
                if (state.netId != yourShipNetId) continue;
                ApplyEntityShipState(*player, state);
                m_ownAtLab = state.atLab;
                m_ownSupplies = state.supplies;
                break;
            }
        }
    }

    return true;
}

void CGame::ReconcileOwnShipIfNeeded()
{
    const std::optional<OwnShipSync::ReconcileResult> result = m_ownShipSync->ReconcileIfNeeded();
    if (!result) return;

    // Diagnostic (2026-07-19): correlate correction magnitude/frequency
    // against NetServer's "peer N input timed out"/stale-input logs to
    // check whether corrections this large are caused by input arriving
    // past its stamped tick (INPUT_LEAD_TICKS too tight for real RTT/
    // jitter -- dropped server-side, repeat-last-command diverges from
    // what was predicted) rather than ordinary f32/quantization noise.
    LOG(trace) << "net: reconciled tick " << result->tick << ", correction magnitude "
              << result->correctionMagnitude << " world units";
    m_netDiagnostics.correctionHistory.Record(result->correctionMagnitude);
    // Recorded on the same "genuinely new snapshot" gate as the correction
    // above, so this lines up 1:1 with it in the Net debug tab's graphs --
    // a real network gap shows up here; a local main-thread stall shows up
    // as a drift/resync event above with this staying flat instead.
    m_netDiagnostics.snapshotIntervalHistory.Record(m_netClient->GetLastSnapshotIntervalMs());
}

void CGame::ApplyRemoteEvents()
{
    const std::uint32_t yourShipNetId = m_netClient->GetYourShipNetId();
    m_remoteEventApplier->Apply([&](std::uint32_t sourceNetId) -> flecs::entity {
        return sourceNetId == yourShipNetId ? GetPlayer().value_or(flecs::entity{})
                                            : m_snapshotApplier.EntityForNetId(sourceNetId);
    });
}

std::uint16_t CGame::ViewDelayTicks(std::uint64_t commandTick) const
{
    // Before the first frame has rendered there is no picture to be behind.
    if (m_lastRenderTick <= 0.0) return 0;

    const double behind = static_cast<double>(commandTick) - m_lastRenderTick;
    if (behind <= 0.0) return 0;

    return static_cast<std::uint16_t>(
            std::min(behind, static_cast<double>(LagCompensation::MAX_REWIND_TICKS)));
}

void CGame::RenderNetClient(float dtSeconds, double tickFraction)
{
    m_netClient->Update();
    ApplyRemoteEvents();

    const std::uint64_t estimatedServerTick = m_netClient->EstimateCurrentServerTick();
    const double renderTick = ComputeRenderTick(m_netClient->EstimateCurrentServerTickF(), m_interpDelaySeconds,
                                                m_netClient->GetTickRate());
    m_lastEstimatedServerTick = estimatedServerTick;
    m_lastRenderTick = renderTick;

    ReconcileOwnShipIfNeeded();

    // Planets must be rendered against the same tick ClientPrediction's
    // gravity/collision proxies last used (TickNetClient's `tick`, i.e.
    // `m_predictedTickClock.Current() - 1` -- the most recent tick actually
    // stepped), not `renderTick` (delayed behind the estimated server tick
    // by the interpolation-delay setting) -- see SnapshotInterpolator::
    // Compute's `planetTick` doc comment. Falls back to `renderTick`
    // (nullopt) before the own ship exists yet, when nothing has
    // stepped/synced a proxy to desync from in the first place.
    // Plus `tickFraction`, so bodies re-derived from it move continuously
    // instead of stepping once per simulated tick while the ship and camera
    // move on real time -- see the `planetTick` doc comment.
    const std::optional<double> planetTick =
            m_clientPrediction.HasOwnShip()
                    ? std::optional<double>(static_cast<double>(m_predictedTickClock.Current() - 1)
                                            + std::clamp(tickFraction, 0.0, 1.0))
                    : std::nullopt;

    // Remote entities only (the own ship is real m_registry state now,
    // Phase 5) via Phase 4 interpolation into the mirror world.
    if (const std::optional<SnapshotData> interpolated =
                SnapshotInterpolator::Compute(m_netClient->GetSnapshotHistory(), renderTick,
                                              m_netClient->GetYourShipNetId(),
                                              static_cast<float>(m_netClient->GetTickRate()), m_interpParams,
                                              planetTick)) {
        m_snapshotApplier.Apply(*interpolated, dtSeconds);
    }

    // Right after the mirror world's ship positions were just refreshed,
    // not from TickNetClient's fixed-step loop (moved here 2026-07-21):
    // that loop can run several ticks back-to-back after a stall (catch-up,
    // MAX_STEPS_PER_FRAME in gravitaris.cpp), but the mirror world only
    // updates once per render frame -- checking mid-catch-up compared the
    // bullet's *already-advanced* position against an up-to-several-ticks
    // -stale enemy position, silently under-triggering hits exactly when
    // real network jitter (this client's actual bug report) made the
    // catch-up happen in the first place. Both are now as fresh as they
    // ever get, at the same instant.
    m_cosmeticBulletDespawner.CheckLocalHits();

    // Blend out any reconciliation snap over ~100ms by decaying the offset
    // *before* camera framing runs, then feeding it in as an already
    // -smoothed position override -- not by nudging the camera's own output
    // position afterward (the previous approach). That ordering mattered:
    // dead-zone follow/enemy-framing/planet-framing all read "where is the
    // player" once, at the top of CameraDirector::Update, so a correction
    // applied only after the fact left every one of them reacting to the
    // raw, still-discontinuous snap this frame -- a fast, repeating jitter
    // in position framing that had nothing to do with network lag in synced
    // enemy/planet data (that's smooth by design; see SnapshotInterpolator).
    // The real simulated Transform itself is never touched here -- it must
    // stay exactly correct for the next predicted tick to build on.
    m_ownShipSync->DecayCorrection(dtSeconds);

    // Own ship: rendered `tickFraction` of the way from the previous
    // predicted tick to the current one, not at the raw current one. Its
    // simulated position only advances on whole fixed-step ticks, while
    // everything it's judged against on screen moves on real time -- the
    // camera eases with wall-clock dt, and remote entities now run on a
    // continuous clock (SnapshotInterpolator). Drawing whole ticks against
    // those makes the ship stall on a frame that ran no tick and lurch on
    // one that ran two, by up to a tick of travel each way, which is why it
    // scales with speed. Costs up to one tick (~17ms) of render latency on
    // the own ship, the standard fixed-step interpolation trade.
    Magnum::Vector2 smoothedPlayerPos;
    if (const std::optional<flecs::entity> player = GetPlayer()) {
        const Transform& t = player->get<Transform>();
        const Vector2d rendered = t.prevPos + (t.pos - t.prevPos) * std::clamp(tickFraction, 0.0, 1.0);
        smoothedPlayerPos = Magnum::Vector2{static_cast<float>(rendered.x()), static_cast<float>(rendered.y())}
                + m_ownShipSync->GetCorrectionOffset();
    }

    // Real single-player camera logic against m_registry -- works for the
    // own ship (dead-zone follow, dynamic zoom); m_mirrorWorld is swept
    // alongside it for enemy/planet framing, since every entity but the own
    // ship lives there in this mode (see m_netClient's field comment).
    const SceneView view = CurrentSceneView();
    // The smoothed-position override belongs to the own predicted ship alone;
    // a spectated unit's Transform is never reconciled, so it needs none.
    m_cameraDirector.Update(view, CameraSubject(), GetDesignViewportSize(), dtSeconds,
                            IsSpectating() ? std::nullopt : std::optional<Magnum::Vector2>(smoothedPlayerPos),
                            SubjectGravity());
    Camera& camera = m_cameraDirector.GetCamera();

    // The hull this frame is being drawn around, kept for the aim: the smoothed
    // position, since that is where the pilot will see it and where the camera
    // was just put.
    m_aimOrigin = Magnum::Vector2d{smoothedPlayerPos};

    // Decays HitFlash on both worlds; ApplyRemoteEvents above is what sets
    // it (own ship directly, everyone else via the mirror world), since
    // m_hitFlashSystem's own event consumption never finds a match here --
    // the events it sees via m_eventQueue were re-emitted with no source
    // entity (see ApplyRemoteEvents), same as ClientPrediction's own local
    // BulletFired ones.
    m_hitFlashSystem.Update(dtSeconds);
    HitFlashSystem::Decay(m_mirrorWorld, dtSeconds);

    // Own-ship one-shots (BulletLifetimeSystem-tracked cosmetic bullets emit
    // BulletFired via m_clientPrediction) and thruster loop, via the same
    // event-driven path single-player uses. m_registry only ever holds the
    // own ship in this mode, so the mirror world has to be swept alongside
    // it or no one else's thrusters are heard.
    m_audioSystem.Update(camera.GetPosition(), &m_mirrorWorld);

    m_starfieldRenderer.SetZoom(camera.GetZoom());
    m_starfieldRenderer.SetCameraPosition(camera.GetPosition());
    m_starfieldRenderer.Render();

    SubmitPlanetOwnershipMarkers(view);
    m_indicatorRenderer.Update(view, CameraSubject(), camera.GetPosition(), camera.GetZoom(),
                               GetDesignViewportSize());
    m_mirrorRenderer2.SetZoom(camera.GetZoom());
    m_mirrorRenderer2.SetCameraPosition(camera.GetPosition());
    m_mirrorRenderer2.SetLineWidth(m_lineWidthPixels);
    m_mirrorRenderer2.SetZoomWidthFactor(m_zoomWidthFactor);
    m_mirrorRenderer2.Render(0.0);

    // Own ship plus the locally predicted cosmetic bullets -- real local sim,
    // drawn through the same renderer/world single-player uses.
    // ModelRenderer2::Render draws Transform::pos directly with no
    // interpolation of its own (its `delta` parameter is unused -- unlike a
    // typical fixed-tick renderer, there's no prevPos/pos blend here at
    // all), so every one of them needs the same sub-tick treatment
    // `smoothedPlayerPos` gives the camera above: draw at the interpolated
    // position by saving the real Transform::pos, overwriting it just for
    // this one render call, then restoring it immediately after -- the
    // actual simulated state (what the next predicted tick builds on) is
    // never touched, only one frame's worth of what gets drawn. Only
    // Renderable entities are touched; the collision/gravity proxies
    // ClientPrediction keeps in this world are driven kinematically and
    // aren't drawn.
    m_modelRenderer2.SetZoom(camera.GetZoom());
    m_modelRenderer2.SetCameraPosition(camera.GetPosition());
    m_modelRenderer2.SetLineWidth(m_lineWidthPixels);
    m_modelRenderer2.SetZoomWidthFactor(m_zoomWidthFactor);

    const double frac = std::clamp(tickFraction, 0.0, 1.0);
    const std::optional<flecs::entity> player = GetPlayer();
    m_renderPosRestore.clear();
    m_registry.each([&](flecs::entity entity, Transform& t, const Renderable&) {
        m_renderPosRestore.emplace_back(entity, t.pos);
        // The reconciliation offset in `smoothedPlayerPos` belongs to the own
        // ship alone -- a bullet is never reconciled, it's spawned from
        // already-corrected state.
        t.pos = (player && entity == *player) ? Vector2d{smoothedPlayerPos}
                                              : t.prevPos + (t.pos - t.prevPos) * frac;
    });

    m_modelRenderer2.Render(0.0);

    // Both worlds: this peer's own beams are predicted locally and everyone
    // else's arrive as a trigger bit and an angle. Gathered before the
    // interpolation override above is undone, so a beam leaves the hull where
    // the hull was actually drawn rather than where the sim last left it.
    m_beams.clear();
    m_charges.clear();
    // Targets from BOTH worlds before either world's beams: the own ship is
    // alone in m_registry and every other hull is in the mirror, so a beam and
    // what it meets are always on opposite sides of that split here.
    m_beamTargets.clear();
    CollectBeamTargets(m_registry);
    CollectBeamTargets(m_mirrorWorld);
    GatherBeams(m_registry);
    GatherBeams(m_mirrorWorld);
    DrawBeams(camera);

    for (const auto& [entity, pos] : m_renderPosRestore) {
        if (entity.is_alive()) entity.get_mut<Transform>().pos = pos;
    }
}

void CGame::Render(double delta)
{
    // Real wall-clock dt for the camera director (Render's `delta` is a fixed-
    // step interpolation fraction, not seconds). Clamped so a stall doesn't
    // snap the camera. Computed up front (not just in the local-sim path
    // below) since the net-client path needs it too, for its own zoom easing.
    const auto now = std::chrono::steady_clock::now();
    float dtSeconds = 1.f / 60.f;
    if (m_cameraTimeValid) {
        dtSeconds = std::chrono::duration<float>(now - m_lastCameraTime).count();
        dtSeconds = std::clamp(dtSeconds, 0.f, 0.1f);
    }
    m_lastCameraTime = now;
    m_cameraTimeValid = true;

    m_renderTimeSeconds += dtSeconds;
    m_modelRenderer2.SetTime(m_renderTimeSeconds);
    m_mirrorRenderer2.SetTime(m_renderTimeSeconds);

    DrainChat();

    if (m_netClient) {
        RenderNetClient(dtSeconds, delta);
        return;
    }

    const SceneView view = CurrentSceneView();
    m_cameraDirector.Update(view, CameraSubject(), GetDesignViewportSize(), dtSeconds, std::nullopt,
                            SubjectGravity());
    // The hull the camera was just placed around, kept for the aim -- see
    // m_aimOrigin. Nothing interpolates a position here (ModelRenderer2 draws
    // straight off Transform), so this frame's ship is simply where it is right
    // now; what makes it matter is that the sim steps on before the cursor is
    // read again.
    if (const std::optional<flecs::entity> own = GetPlayer(); own && own->is_alive()) {
        if (const Transform* transf = own->try_get<Transform>()) m_aimOrigin = transf->pos;
    }

    m_hitFlashSystem.Update(dtSeconds);

    const Camera& camera = m_cameraDirector.GetCamera();
    m_simpleModelRenderer.SetZoom(camera.GetZoom());
    m_simpleModelRenderer.SetCameraPosition(camera.GetPosition());
    m_modelRenderer2.SetZoom(camera.GetZoom());
    m_modelRenderer2.SetCameraPosition(camera.GetPosition());
    m_modelRenderer2.SetLineWidth(m_lineWidthPixels);
    m_modelRenderer2.SetZoomWidthFactor(m_zoomWidthFactor);

    m_starfieldRenderer.SetZoom(camera.GetZoom());
    m_starfieldRenderer.SetCameraPosition(camera.GetPosition());

    // Overlays ride m_modelRenderer2's instanced draw (which clears them at
    // the end of its Render), so only submit them when that renderer actually
    // runs this frame -- otherwise the overlay scratch grows unboundedly.
    if (m_activeRenderer == RendererKind::Baked) {
        m_indicatorRenderer.Update(view, CameraSubject(), camera.GetPosition(), camera.GetZoom(),
                                   GetDesignViewportSize());
        SubmitPlanetOwnershipMarkers(view);
    }

    {
        ScopedPerfTimer timer(m_perfMonitor, "Starfield");
        m_starfieldRenderer.Render();
    }

    {
        ScopedPerfTimer timer(m_perfMonitor, "Rendering");
        // Renderers are mutually exclusive; the debug UI picks the active one.
        switch (m_activeRenderer) {
            case RendererKind::Simple:
                m_simpleModelRenderer.Render(delta);
                break;
            case RendererKind::Baked:
                m_modelRenderer2.Render(delta);
                break;
            case RendererKind::Mirror: {
                // Round-trip the live sim through the full snapshot path
                // (world -> bytes -> parse -> apply), then draw the mirror
                // world instead of the real one (networking-plan 2.5).
                {
                    ScopedPerfTimer mirrorTimer(m_perfMonitor, "Snapshot Mirror");
                    m_snapshotScratch.Clear();
                    WriteSnapshot(m_registry, m_eventQueue, GetStep(), m_mirrorEventCursor,
                                  m_snapshotScratch);
                    ByteReader reader(m_snapshotScratch.Data(), m_snapshotScratch.Size());
                    SnapshotData snapshot;
                    if (ReadSnapshot(reader, snapshot)) {
                        m_snapshotApplier.Apply(snapshot);
                        if (!snapshot.events.empty()) {
                            m_mirrorEventCursor = snapshot.events.back().seq;
                        }
                    }
                }
                m_mirrorRenderer2.SetZoom(camera.GetZoom());
                m_mirrorRenderer2.SetCameraPosition(camera.GetPosition());
                m_mirrorRenderer2.SetLineWidth(m_lineWidthPixels);
                m_mirrorRenderer2.SetZoomWidthFactor(m_zoomWidthFactor);
                m_mirrorRenderer2.Render(delta);
                break;
            }
        }
    }

    // After the hulls, so a beam reads as light laid over what it is crossing,
    // and still inside the scene target so the glow pass picks it up.
    m_beams.clear();
    m_charges.clear();
    {
        flecs::world& drawn = m_activeRenderer == RendererKind::Mirror ? m_mirrorWorld : m_registry;
        m_beamTargets.clear();
        CollectBeamTargets(drawn);
        GatherBeams(drawn);
    }
    DrawBeams(camera);

    {
        ScopedPerfTimer timer(m_perfMonitor, "Audio");
        m_audioSystem.Update(camera.GetPosition());
    }
}

// Every beam burning in `world` this frame, drawn where its own hull says it
// leaves from and cut off at whatever it first crosses. Nothing about a beam is
// replicated beyond the trigger and the angle (see EntityState::aim) -- the
// geometry is re-derived here off the same rules the sim resolves damage with,
// which is why a beam looks like it hits what it is actually hurting.
void CGame::CollectBeamTargets(flecs::world& world)
{
    // Gathered up front, never inside the walk over the beams: a flecs query
    // run inside another query's callback silently iterates nothing (see
    // CLAUDE.md), so a per-beam lookup would quietly stop truncating anything.
    //
    // Appends rather than replaces, and that is the whole point of it being
    // separate from GatherBeams: in a networked game this peer's own ship is
    // the only thing in m_registry and everyone else is in the mirror world, so
    // collecting targets from the same world as the shooter meant your beam
    // could never see an enemy, nor theirs see you. It drew straight through
    // every hull it was burning -- and with nothing to bounce off, never
    // deflected either.
    world.each([&](flecs::entity ent, const Transform& transf, const Damageable&) {
        const Body* hull = HullOf(ent);
        if (!hull) return;

        const double radius = hull->GetBoundingRadius() * transf.scale.x();
        if (radius > 0.) m_beamTargets.push_back(BeamTarget{ent, transf.pos, radius});
    });

    // Missiles carry no Damageable -- their airframe is Missile::hp -- so the
    // walk above cannot see the one round a beam is able to stop. Without this
    // a beam would be drawn straight through a missile it is burning down.
    //
    // Against the interception corridor rather than the round's own outline,
    // because that is what the sim burns it inside of.
    world.each([&](flecs::entity ent, const Transform& transf, const Missile&) {
        m_beamTargets.push_back(BeamTarget{ent, transf.pos, DamageSystem::BEAM_INTERCEPT_RADIUS});
    });
}

// Every beam burning in `world`, drawn against whatever CollectBeamTargets has
// been given -- which must be every world being drawn, not just this one.
void CGame::GatherBeams(flecs::world& world)
{
    world.each([&](flecs::entity ship, const Transform& transf, const Controls& controls,
                   const ShipLoadout& loadout) {
        // A charging emitter is drawn too, and deliberately: the windup is the
        // weapon's telegraph, and it has to be visible from the other end of it.
        const bool charging = controls.laserWindup > 0;
        if (!controls.laserFiring && !charging) return;

        const ShipStats stats = m_upgradeCatalog.ResolveStats(loadout.levels);
        if (!stats.laser || !stats.laser->IsBeam()) return;

        const Body* hull = HullOf(ship);
        const WeaponDef::Beam& beam = stats.laser->beam;
        // The side's own colour, run hot with the rank: a beam crossing the
        // sector should say who fired it before it says what it is.
        const Team* team = ship.try_get<Team>();
        const Magnum::Color3 color = Magnum::Math::lerp(
                TeamColor(team ? team->id : TeamId::None), Magnum::Color3{1.f}, beam.heat);

        // How far through the charge this mount is. The remaining ticks are what
        // travels (EntityState::laserWindup), so a remote ship's charge swells
        // over exactly the same second the shooter's own does.
        const std::uint16_t windupTicks = ShipControlsSystem::BeamWindupTicks(*stats.laser);
        const float charged = windupTicks > 0
                ? 1.f - static_cast<float>(controls.laserWindup) / static_cast<float>(windupTicks)
                : 1.f;

        for (std::size_t mount = 0; mount < MAX_WEAPON_MOUNTS; ++mount) {
            if (loadout.mounts[mount] != MountArm::Laser) continue;

            const ShipControlsSystem::BeamOrigin origin = ShipControlsSystem::ComputeBeamOrigin(
                    transf, hull, static_cast<unsigned>(mount), controls.actionFlags.aim);
            const Magnum::Vector2d heading{std::cos(origin.angle), std::sin(origin.angle)};

            // The emitter itself, lit whether or not the beam has arrived: it
            // swells out of nothing over the charge and then stays at the root
            // of the beam for as long as one is burning. Both the telegraph and
            // the muzzle, and one shape rather than two -- what a pilot sees at
            // a mount is the same light either way.
            const float lit = charging ? charged : 1.f;
            m_charges.push_back(LaserRenderer::Charge{
                    Magnum::Vector2{static_cast<float>(origin.pos.x()),
                                    static_cast<float>(origin.pos.y())},
                    // Additive, so dimming the colour is the fade in from
                    // nothing; it runs hotter as it fills, brightest at the
                    // moment the beam leaves.
                    Magnum::Math::lerp(color, Magnum::Color3{1.f}, lit * 0.5f) * lit,
                    static_cast<float>(BEAM_CHARGE_RADIUS * lit)});
            if (charging) continue;

            // One segment per leg, following the same rule the sim resolves the
            // burn with: a mirrored plate throws the beam onward, weaker, from
            // where it landed. Everything continuous along the beam -- the fade,
            // the widening, the distance the falloff is measured on -- carries
            // across the kink, because it is one beam and not several.
            const auto fadeLength = static_cast<float>(beam.range * BEAM_FADE_SHARE);
            Magnum::Vector2d from = origin.pos;
            Magnum::Vector2d along = heading;
            double travelled = 0.;
            // Distance for the FADE, which a bounce mostly forgives and
            // `travelled` does not. The two part company on purpose: the falloff
            // is measured along the whole path because that is the physics, but
            // the light dies within a fraction of the reach, so a bounce off
            // anything further out than that came out at a tenth of an alpha --
            // in the buffer, invisible on screen, which is exactly how it
            // looked. A plate throwing a beam back is re-emitting it, so most of
            // the distance it had already lost is given back; what still says
            // the beam is spent is `strength`, which carries across in full.
            double litFrom = 0.;
            float strength = 1.f;
            flecs::entity deflector;

            for (unsigned leg = 0; leg <= ShipControlsSystem::MAX_BEAM_BOUNCES; ++leg) {
                const double reach = beam.range - travelled;
                if (reach <= 0. || strength <= 0.01f) break;

                // The shooter is only exempt on the first leg. After a bounce the
                // beam is coming at its own hull from outside, and drawing it
                // through the ship it is burning would be a lie the sim does not
                // tell.
                const BeamStop stop = BeamReach(leg == 0 ? ship : flecs::entity{}, deflector,
                                                from, along, reach);
                const float absorb = stop.target.is_alive() ? BeamAbsorbShareOf(stop.target) : 1.f;
                const bool bounces = stop.target.is_alive() && absorb < 1.f;

                // A beam that got nowhere still has to read as a beam rather
                // than vanish -- but only where it ends for good, since padding
                // a leg that bounces would move the kink off the hull.
                const double length = bounces ? stop.distance
                                              : std::max(stop.distance,
                                                         beam.range * MIN_BEAM_SHARE - travelled);
                if (length <= 0.) break;

                const Magnum::Vector2d end = from + along * length;
                const auto nearShare = static_cast<float>(travelled / beam.range);
                const auto farShare = static_cast<float>((travelled + length) / beam.range);
                const auto width = [&](float share) {
                    return beam.widthNear + (beam.widthFar - beam.widthNear) * share;
                };
                m_beams.push_back(LaserRenderer::Beam{
                        Magnum::Vector2{static_cast<float>(from.x()), static_cast<float>(from.y())},
                        Magnum::Vector2{static_cast<float>(end.x()), static_cast<float>(end.y())},
                        // Additive, so a deflected leg carrying half the beam is
                        // drawn at half the colour.
                        color * strength, width(nearShare), width(farShare), fadeLength,
                        static_cast<float>(litFrom / (beam.range * BEAM_FADE_SHARE))});

                if (!bounces) break;

                strength *= 1.f - absorb;
                travelled += length;
                litFrom = travelled * BEAM_BOUNCE_RELIGHT;
                from = end;
                along = ShipControlsSystem::ReflectHeading(along, stop.normal);
                deflector = stop.target;
            }
        }
    });

}

void CGame::DrawBeams(const Camera& camera)
{
    if (m_beams.empty() && m_charges.empty()) return;

    m_laserRenderer.SetZoom(camera.GetZoom());
    m_laserRenderer.SetCameraPosition(camera.GetPosition());
    m_laserRenderer.Render(m_beams, m_charges);
}

// The hull a beam leaves from, wherever this world keeps it: a simulated ship
// reaches it through its physics body, a replicated one through the HitOutline
// derived from the same replicated modelId (ADR 0001 -- a mirror ship has no
// physics at all).
const Body* CGame::HullOf(flecs::entity ent)
{
    if (const HitOutline* outline = ent.try_get<HitOutline>()) return outline->body.Get();
    if (const PhysicsRef* ref = ent.try_get<PhysicsRef>()) {
        return GetPhysicsSystem().GetBody(*ref).body.Get();
    }
    return nullptr;
}

CGame::BeamStop CGame::BeamReach(flecs::entity ignoreA, flecs::entity ignoreB,
                                 const Magnum::Vector2d& from,
                                 const Magnum::Vector2d& heading, double range)
{
    BeamStop stop{range, flecs::entity{}, {}};

    for (const BeamTarget& target : m_beamTargets) {
        // Compared by id, and never asked whether they are alive: both of these
        // are empty on the leg that has not bounced yet, and asking flecs about
        // entity 0 breaks a precondition it only checks in a debug build -- the
        // same one that crashed a release build once already (see
        // DamageSystem::QueryFirstHit).
        if ((ignoreA.id() != 0 && target.entity.id() == ignoreA.id())
            || (ignoreB.id() != 0 && target.entity.id() == ignoreB.id())) {
            continue;
        }

        const Magnum::Vector2d toTarget = target.pos - from;
        const double radiusSq = target.radius * target.radius;
        // A hull the muzzle is already inside cannot be what stops the beam --
        // the mount is buried in its own ship, and a bounding circle is coarse
        // enough that a neighbour's can swallow it too. Truncating to zero
        // there left a beam with no length at all, which is a beam that does
        // not draw: the sim was burning a target, the sound was playing, and
        // the screen showed nothing.
        if (toTarget.dot() <= radiusSq) continue;

        // Closest approach of the ray to the hull's centre, and the chord it
        // cuts if it comes inside the circle at all.
        const double along = Magnum::Math::dot(toTarget, heading);
        if (along <= 0. || along > stop.distance) continue;

        const double offSq = toTarget.dot() - along * along;
        if (offSq > radiusSq) continue;

        stop.distance = std::max(0., along - std::sqrt(radiusSq - offSq));
        stop.target = target.entity;
        stop.normal = (from + heading * stop.distance) - target.pos;
    }

    return stop;
}

float CGame::BeamAbsorbShareOf(flecs::entity ent)
{
    const ShipLoadout* loadout = ent.try_get<ShipLoadout>();
    if (!loadout || loadout->shieldHp <= 0.f) return 1.f;

    return m_upgradeCatalog.ResolveStats(loadout->levels).laserAbsorb;
}

std::unique_ptr<EntitySpawner> CGame::CreateEntitySpawner()
{
    return std::make_unique<CEntitySpawner>(m_registry, m_resourceLoader);
}

// Which family of hull holes a slot addresses, from what it accepts. Only the
// slots that take a mounted line address one at all: a shield carries an index
// of its own, and reading the mount array with one of those had every `_0`
// slot on the hull reporting whatever was in the nose.
static SlotFamily SlotFamilyOf(const std::vector<std::string>& categories)
{
    for (const std::string& category : categories) {
        if (category == "gun" || category == "cannon") return SlotFamily::Weapon;
        if (category == "missile") return SlotFamily::MissileBay;
        if (category == "ammo") return SlotFamily::AmmoBay;
    }
    return SlotFamily::None;
}

// The hole a slot sits on, from the index its label ends in: `gun+cannon_1` is
// the hull's weapon_1, `missile_1` its second bay. Which array that indexes is
// the family's business, not this one's.
static std::uint8_t SlotIndexOf(const std::string& label)
{
    const std::size_t underscore = label.find_last_of('_');
    if (underscore == std::string::npos) return TechPick::NO_MOUNT;

    const std::string digits = label.substr(underscore + 1);
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) {
        return TechPick::NO_MOUNT;
    }
    const int index = std::atoi(digits.c_str());
    return index >= 0 && index < static_cast<int>(MAX_WEAPON_MOUNTS)
                 ? static_cast<std::uint8_t>(index) : TechPick::NO_MOUNT;
}

// A slot's authored label is `<category>[+<category>...]_<index>`, the same
// shape hardpoint names take (see Body::FindMount): the index makes the label
// unique on a hull that carries two of something, and everything before it is
// what the slot accepts.
static std::vector<std::string> SlotCategories(const std::string& label)
{
    std::string categories = label;
    const std::size_t underscore = categories.find_last_of('_');
    if (underscore != std::string::npos
            && categories.find_first_not_of("0123456789", underscore + 1) == std::string::npos) {
        categories.resize(underscore);
    }

    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= categories.size()) {
        const std::size_t plus = categories.find('+', start);
        const std::size_t end = plus == std::string::npos ? categories.size() : plus;
        if (end > start) out.push_back(categories.substr(start, end - start));
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    return out;
}

} // namespace Gravitaris
