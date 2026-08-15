#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/component/callsign.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/faction-state.hpp>
#include <gravitaris/game/component/pilot-account.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/ship-loadout.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/debug/debug-spawn.hpp>
#include <gravitaris/game/game.hpp>

#include <gravitaris/game/cheat/cheat-console.hpp>

namespace Gravitaris {

using Magnum::Vector2d;

// Bounds on anything a command can ask for in bulk, so a fat-fingered
// "/spawn 100000" is a refusal rather than a hung server.
static constexpr int MAX_BULK = 100;

// Points and supplies are counters, not work -- MAX_BULK would be the wrong
// ceiling for them, so they get one of their own and a default worth typing.
static constexpr int MAX_GRANT = 1000000;
static constexpr int DEFAULT_GRANT = 100;

// How far to one side /tp puts you when the destination is another ship --
// clear of its hull and its shield bubble, close enough to still be there.
static constexpr double TP_STANDOFF = 90.;

static std::vector<std::string> Tokenize(const std::string& text);
static FactionState* FindFaction(flecs::world& registry, TeamId team);
static PilotAccount* FindAccount(flecs::world& registry, flecs::entity ship);
static flecs::entity FindCallsign(flecs::world& registry, const std::string& lowercaseName);
static std::string Lowercase(std::string text);
static bool ParseInt(const std::string& token, int& out);
static bool ParseDouble(const std::string& token, double& out);
static std::optional<TeamId> ParseTeam(const std::string& token);
static const char* TeamName(TeamId team);
static std::uint32_t AddCapped(std::uint32_t total, int amount);
static std::string Format(const char* format, ...);

// Everything a command needs to reach: the sim, who is asking, and the reply
// being built. Passed by reference to each handler so they read as a list of
// rules rather than a list of parameter packs.
namespace {

struct Cheat {
    Game& game;
    flecs::entity subject;
    TeamId team;
    const std::vector<std::string>& args; // args[0] is the verb
    CheatResult& result;

    // The issuer's ship, or null with `reply` already explaining why not --
    // every command that touches a hull opens with this.
    ShipLoadout* Loadout();
    Damageable* Hull();

    void Say(std::string line) { result.reply.push_back(std::move(line)); }

    // args[index] if it is there, otherwise `fallback`.
    [[nodiscard]] int IntArg(std::size_t index, int fallback) const;
};

} // namespace

static void CheatHelp(Cheat& c);
static void CheatUnlockAll(Cheat& c);
static void CheatTech(Cheat& c);
static void CheatSupplies(Cheat& c);
static void CheatUpgrade(Cheat& c);
static void CheatHeal(Cheat& c);
static void CheatGod(Cheat& c);
static void CheatAmmo(Cheat& c);
static void CheatKill(Cheat& c);
static void CheatTeleport(Cheat& c);
static void CheatWhere(Cheat& c);
static void CheatSpawn(Cheat& c);
static void CheatAI(Cheat& c);
static void CheatFriendlyFire(Cheat& c);
static void CheatPlayers(Cheat& c);
static void CheatNotify(Cheat& c);

bool IsCheatCommand(const std::string& text)
{
    return text.size() > 1 && text[0] == '/';
}

CheatResult RunCheatCommand(Game& game, flecs::entity subject, TeamId team, const std::string& text)
{
    CheatResult result;

    std::vector<std::string> args = Tokenize(text.substr(1));
    if (args.empty()) {
        result.reply.push_back("cheats: type /help");
        return result;
    }

    // "@name" anywhere in the line runs the command on that player's ship
    // instead of your own -- and on their faction, so /research @bob stocks
    // bob's labs rather than yours. Stripped here so no handler has to know
    // it was there.
    for (auto arg = args.begin() + 1; arg != args.end(); ++arg) {
        if (arg->size() < 2 || (*arg)[0] != '@') continue;

        const std::string name = arg->substr(1);
        const flecs::entity target = FindCallsign(game.GetRegistry(), name);
        if (!target.is_alive()) {
            result.reply.push_back("no player flying as '" + name + "' (/players lists them)");
            return result;
        }
        subject = target;
        if (const Team* targetTeam = target.try_get<Team>()) team = targetTeam->id;
        args.erase(arg);
        break;
    }

    Cheat cheat{game, subject, team, args, result};

    const std::string& verb = args[0];
    if (verb == "help" || verb == "cheats") CheatHelp(cheat);
    else if (verb == "unlock" || verb == "unlockall") CheatUnlockAll(cheat);
    else if (verb == "tech") CheatTech(cheat);
    else if (verb == "supply") CheatSupplies(cheat);
    else if (verb == "upgrade") CheatUpgrade(cheat);
    else if (verb == "heal") CheatHeal(cheat);
    else if (verb == "god") CheatGod(cheat);
    else if (verb == "ammo") CheatAmmo(cheat);
    else if (verb == "kill") CheatKill(cheat);
    else if (verb == "tp" || verb == "teleport") CheatTeleport(cheat);
    else if (verb == "where") CheatWhere(cheat);
    else if (verb == "spawn") CheatSpawn(cheat);
    else if (verb == "ai") CheatAI(cheat);
    else if (verb == "ff" || verb == "friendlyfire") CheatFriendlyFire(cheat);
    else if (verb == "players" || verb == "who") CheatPlayers(cheat);
    else if (verb == "notify") CheatNotify(cheat);
    else result.reply.push_back("no such cheat: /" + verb + " -- try /help");

    LOG(info) << "cheat: " << TeamName(team) << " ran " << text;
    return result;
}

static void CheatHelp(Cheat& c)
{
    c.Say("cheats (everyone, always):");
    c.Say("/unlock - learn every rank in the tree, and stock the account to buy them");
    c.Say("/tech [n] - n technology points into your faction's pool (100)");
    c.Say("/supply [n] - n supplies into your own account (100)");
    c.Say("/upgrade <key|all|list> [n] - fit one straight onto your ship");
    c.Say("/heal - hull and shields back to full");
    c.Say("/god - stop dying (lost on respawn)");
    c.Say("/ammo [n] - fill the missile rack and the cannon magazine");
    c.Say("/kill - blow your own ship up");
    c.Say("/tp <x> <y> | /tp home | /tp <player> - go there");
    c.Say("/where - where you are now");
    c.Say("/players - who is flying, and what they are called");
    c.Say("/spawn [n] [team] - n AI fighters at that team's home");
    c.Say("/ai [team] - field an AI leader for every side nobody is flying");
    c.Say("/ff [on|off] - friendly fire, for the whole round");
    c.Say("/notify [on|off] - webhook a line when somebody joins");
    c.Say("add @<player> to any of the above to run it on their ship instead");
}

// Everything the trees can offer, learned and paid for, in one line. /tech and
// /supply hand out the currencies and leave the climb; this skips the climb --
// which is what testing a weapon wants, since the thing under test is usually
// several ranks up a tree behind a prerequisite that has nothing to do with it.
//
// Still has to be FITTED at a lab or high port afterwards (/upgrade puts one
// straight onto the hull instead, no landing needed). Unlocking is the faction's
// and the supplies are the pilot's, so an "@name" runs both for them.
static void CheatUnlockAll(Cheat& c)
{
    const UpgradeCatalog& catalog = c.game.GetUpgradeCatalog();

    FactionState* faction = FindFaction(c.game.GetRegistry(), c.team);
    if (!faction) {
        c.Say("no faction to research for -- fly something first");
        return;
    }

    int learned = 0;
    for (std::size_t i = 0; i < catalog.Defs().size(); ++i) {
        const std::uint8_t top = UpgradeCatalog::RankCount(catalog.Defs()[i]);
        if (faction->unlocked.rank[i] >= top) continue;

        faction->unlocked.rank[i] = top;
        ++learned;
    }
    // The pool as well, so the PERMANENT board still reads as affordable rather
    // than as a wall of prices nothing can meet.
    faction->techPoints = AddCapped(faction->techPoints, MAX_GRANT);

    if (PilotAccount* account = FindAccount(c.game.GetRegistry(), c.subject)) {
        account->supplies = AddCapped(account->supplies, MAX_GRANT);
        c.Say(Format("unlocked %d lines for %s, and stocked the account: %u supplies",
                     learned, TeamName(c.team), account->supplies));
    }
    else {
        c.Say(Format("unlocked %d lines for %s (no pilot account to stock -- fly something first)",
                     learned, TeamName(c.team)));
    }
    c.Say("fit them at a lab or high port, or /upgrade <key> to skip the landing");
}

static void CheatTech(Cheat& c)
{
    FactionState* faction = FindFaction(c.game.GetRegistry(), c.team);
    if (!faction) {
        c.Say("no faction to research for -- fly something first");
        return;
    }

    const int count = std::clamp(c.IntArg(1, DEFAULT_GRANT), 1, MAX_GRANT);
    faction->techPoints = AddCapped(faction->techPoints, count);

    c.Say(Format("tech: %u to spend in the PERMANENT tree", faction->techPoints));
}

static void CheatSupplies(Cheat& c)
{
    PilotAccount* account = FindAccount(c.game.GetRegistry(), c.subject);
    if (!account) {
        c.Say("no pilot account -- fly something first");
        return;
    }

    const int count = std::clamp(c.IntArg(1, DEFAULT_GRANT), 1, MAX_GRANT);
    account->supplies = AddCapped(account->supplies, count);

    c.Say(Format("supplies: %u to spend in the SHIP tree (land at a lab or high port)",
                 account->supplies));
}

static void CheatUpgrade(Cheat& c)
{
    const UpgradeCatalog& catalog = c.game.GetUpgradeCatalog();

    const std::string key = c.args.size() > 1 ? c.args[1] : "list";
    if (key == "list") {
        std::string keys;
        for (const UpgradeDef& def : catalog.Defs()) {
            keys += (keys.empty() ? "" : ", ") + def.key;
        }
        c.Say("upgrades: " + (keys.empty() ? std::string("none loaded") : keys));
        return;
    }

    ShipLoadout* loadout = c.Loadout();
    if (!loadout) return;

    FactionState* faction = FindFaction(c.game.GetRegistry(), c.team);
    TechUnlocks spare; // stands in for a faction that doesn't exist yet
    TechUnlocks& unlocked = faction ? faction->unlocked : spare;

    // Unlocks the rank for the side and fits it in one go, and pays for
    // neither -- a cheat has no business queueing at the shop. `all` walks the
    // pool once per requested rank, so a repeatable entry stacks and a tiered
    // one climbs.
    const int count = std::clamp(c.IntArg(2, 1), 1, MAX_BULK);
    int granted = 0;
    for (int i = 0; i < count; ++i) {
        for (const UpgradeDef& def : catalog.Defs()) {
            if (key != "all" && def.key != key) continue;

            const auto rank = static_cast<std::uint8_t>(
                    def.maxLevel == 0 ? 1 : UpgradeCatalog::LevelOf(def, loadout->levels) + 1);
            if (rank > UpgradeCatalog::RankCount(def)) continue;

            const std::size_t index = catalog.IndexOf(def.id);
            if (index >= catalog.Defs().size()) continue;
            unlocked.rank[index] = std::max(unlocked.rank[index], rank);

            std::uint32_t free = UpgradeCatalog::SupplyCostOf(def, rank);
            if (catalog.FitRank(def, rank, *loadout, unlocked, free, /*atLab=*/true)) ++granted;
        }
    }

    if (granted == 0) {
        c.Say("upgrade: nothing to grant -- unknown key, or already maxed (/upgrade list)");
        return;
    }
    c.Say(Format("upgrade: %d fitted", granted));
}

static void CheatHeal(Cheat& c)
{
    Damageable* hull = c.Hull();
    if (!hull) return;

    hull->hp = hull->maxHp;

    if (ShipLoadout* loadout = c.subject.try_get_mut<ShipLoadout>()) {
        const ShipStats stats = c.game.GetUpgradeCatalog().ResolveStats(loadout->levels);

        loadout->shieldHp = stats.shieldCapacity;
        loadout->shieldRegenDelay = 0;
        const float perPlate = PerPlate(*loadout, stats.shieldCapacity);
        for (std::uint8_t i = 0; i < loadout->plateCount; ++i) {
            loadout->plates[i] = perPlate;
            loadout->plateRegenDelay[i] = 0;
        }
    }

    c.Say("heal: hull and shields full");
}

static void CheatGod(Cheat& c)
{
    Damageable* hull = c.Hull();
    if (!hull) return;

    hull->invulnerable = !hull->invulnerable;
    if (hull->invulnerable) hull->hp = hull->maxHp;

    c.Say(hull->invulnerable ? "god: on -- nothing kills this hull (until you respawn)"
                             : "god: off");
}

static void CheatAmmo(Cheat& c)
{
    ShipLoadout* loadout = c.Loadout();
    if (!loadout) return;

    // Both magazines, since nothing refills either one for free any more -- a
    // yard charges for rounds now (UpgradeCatalog::Resupply), and a dev testing
    // the cannon should not have to go shopping.
    const ShipStats stats = c.game.GetUpgradeCatalog().ResolveStats(loadout->levels);
    if (stats.missileCapacity <= 0 && stats.cannonCapacity <= 0) {
        c.Say("ammo: nothing fitted that runs out -- /upgrade missile_bay or heavy_cannons first");
        return;
    }

    const auto fill = [&](int capacity) {
        return static_cast<std::uint16_t>(std::clamp(c.IntArg(1, capacity), 0, capacity));
    };
    loadout->missileAmmo = fill(stats.missileCapacity);
    loadout->cannonAmmo = fill(stats.cannonCapacity);
    c.Say(Format("ammo: %d / %d on the rack, %d / %d in the magazine", loadout->missileAmmo,
                 stats.missileCapacity, loadout->cannonAmmo, stats.cannonCapacity));
}

static void CheatKill(Cheat& c)
{
    Damageable* hull = c.Hull();
    if (!hull) return;

    // Clears god first, or the hull that asked to die would be put straight
    // back together by DeathSystem.
    hull->invulnerable = false;
    hull->hp = 0.f;
    hull->lastDamageCause = DamageCause::Unknown;
    hull->lastDamageTeam = TeamId::None;
    c.Say("kill: goodbye");
}

static void CheatTeleport(Cheat& c)
{
    if (!c.subject.is_alive()) {
        c.Say("tp: you have no ship right now");
        return;
    }
    const PhysicsRef* ref = c.subject.try_get<PhysicsRef>();
    Transform* transform = c.subject.try_get_mut<Transform>();
    if (!ref || !transform) {
        c.Say("tp: your ship isn't in the world");
        return;
    }

    Vector2d pos;
    Vector2d vel;
    double x = 0.;
    double y = 0.;

    if (c.args.size() > 1 && c.args[1] == "home") {
        const std::optional<FactionSystem::SpawnPoint> site =
                c.game.GetFactionSystem().SpawnPosition(c.team);
        if (!site) {
            c.Say("tp: your faction holds nothing to go home to");
            return;
        }
        pos = site->pos;
        // The site's own velocity, not zero: home is a station riding an
        // orbiting planet, and arriving stationary beside it means arriving at
        // orbital speed relative to it.
        vel = site->vel;
    }
    else if (c.args.size() > 2 && ParseDouble(c.args[1], x) && ParseDouble(c.args[2], y)) {
        pos = Vector2d{x, y};
    }
    else if (c.args.size() > 1) {
        const flecs::entity target = FindCallsign(c.game.GetRegistry(), c.args[1]);
        const Transform* theirs = target.is_alive() ? target.try_get<Transform>() : nullptr;
        if (!theirs) {
            c.Say("tp: no player flying as '" + c.args[1] + "' (/players lists them)");
            return;
        }
        if (target == c.subject) {
            c.Say("tp: you are already there");
            return;
        }
        // Alongside rather than on top of them, and matching their velocity:
        // arriving at rest beside a ship doing 400 u/s is arriving at 400 u/s
        // relative to it, which is a ram rather than a rendezvous.
        pos = theirs->pos + Vector2d{TP_STANDOFF, 0.};
        vel = theirs->vel;
    }
    else {
        c.Say("tp: /tp <x> <y>, /tp home, or /tp <player> (/players lists them)");
        return;
    }

    c.game.GetPhysicsSystem().Teleport(*ref, pos, vel);
    transform->pos = pos;
    transform->vel = vel;

    c.Say(Format("tp: %.0f, %.0f", pos.x(), pos.y()));
}

static void CheatWhere(Cheat& c)
{
    if (!c.subject.is_alive()) {
        c.Say("where: nowhere -- you have no ship right now");
        return;
    }
    const Transform* transform = c.subject.try_get<Transform>();
    if (!transform) {
        c.Say("where: your ship isn't in the world");
        return;
    }

    c.Say(Format("where: %.0f, %.0f at %.0f u/s", transform->pos.x(), transform->pos.y(),
                 transform->vel.length()));
}

static void CheatSpawn(Cheat& c)
{
    const int count = std::clamp(c.IntArg(1, 1), 1, MAX_BULK);

    // Defaults to somebody else's side: a wave you have to fight is the one
    // worth conjuring, and your own is one word away.
    TeamId team = c.team;
    if (c.args.size() > 2) {
        const std::optional<TeamId> named = ParseTeam(c.args[2]);
        if (!named) {
            c.Say("spawn: no such side -- blue, red, violet, yellow, magenta, cyan");
            return;
        }
        team = *named;
    }
    else {
        for (const TeamId rostered : c.game.GetRoster()) {
            if (rostered != c.team) {
                team = rostered;
                break;
            }
        }
    }

    SpawnAIWave(c.game, count, c.game.ResolveAIPreset(0), team);

    c.result.announce = true;
    c.Say(Format("spawn: %d %s fighter%s", count, TeamName(team), count == 1 ? "" : "s"));
}

// The other half of a dedicated server fielding nobody by default: a side left
// empty is a slot waiting for a player, and this is what says the wait is over.
static void CheatAI(Cheat& c)
{
    if (c.args.size() > 1 && c.args[1] != "fill") {
        const std::optional<TeamId> named = ParseTeam(c.args[1]);
        if (!named) {
            c.Say("ai: no such side -- blue, red, violet, yellow, magenta, cyan");
            return;
        }
        if (c.game.HasAIFaction(*named)) {
            c.Say(Format("ai: %s already fields a leader", TeamName(*named)));
            return;
        }
        c.game.AddAIFaction(*named);
        c.result.announce = true;
        c.Say(Format("ai: %s now fields a leader", TeamName(*named)));
        return;
    }

    const std::vector<TeamId> filled = c.game.FillEmptyTeamsWithAI();
    if (filled.empty()) {
        c.Say("ai: every side is already flown or already has a leader");
        return;
    }

    std::string names;
    for (const TeamId team : filled) {
        names += (names.empty() ? "" : ", ") + std::string(TeamName(team));
    }
    c.result.announce = true;
    c.Say("ai: leaders fielded for " + names);
}

static void CheatPlayers(Cheat& c)
{
    int found = 0;
    c.game.GetRegistry().each([&](flecs::entity ship, const Callsign& callsign) {
        const Team* team = ship.try_get<Team>();
        const Transform* transform = ship.try_get<Transform>();
        ++found;
        c.Say(Format("%s (%s) at %.0f, %.0f", callsign.name.c_str(),
                     TeamName(team ? team->id : TeamId::None),
                     transform ? transform->pos.x() : 0., transform ? transform->pos.y() : 0.));
    });

    if (found == 0) c.Say("players: nobody is flying");
}

static void CheatNotify(Cheat& c)
{
    if (!c.game.HasNotifier()) {
        c.Say("notify: this server has no webhook configured (GRAVITARIS_NOTIFY_WEBHOOK)");
        return;
    }

    bool enabled = !c.game.AreNotificationsEnabled();
    if (c.args.size() > 1) {
        const std::string& arg = c.args[1];
        if (arg == "on" || arg == "1" || arg == "true") enabled = true;
        else if (arg == "off" || arg == "0" || arg == "false") enabled = false;
        else {
            c.Say("notify: /notify on, /notify off, or /notify to flip it");
            return;
        }
    }
    c.game.SetNotificationsEnabled(enabled);

    // Sent through the same path it just switched on, so "it says it's on" and
    // "something actually arrives" are one test rather than two.
    if (enabled) c.game.Notify("Notifications on.");

    c.Say(enabled ? "notify: on -- a line goes out when somebody joins"
                  : "notify: off");
}

static void CheatFriendlyFire(Cheat& c)
{
    DamageSystem& damage = c.game.GetDamageSystem();

    bool enabled = !damage.IsFriendlyFire();
    if (c.args.size() > 1) {
        const std::string& arg = c.args[1];
        if (arg == "on" || arg == "1" || arg == "true") enabled = true;
        else if (arg == "off" || arg == "0" || arg == "false") enabled = false;
        else {
            c.Say("ff: /ff on, /ff off, or /ff to flip it");
            return;
        }
    }
    damage.SetFriendlyFire(enabled);

    c.result.announce = true;
    c.Say(enabled ? "friendly fire: ON -- your rounds and your hull hurt your own side"
                  : "friendly fire: off");
}

ShipLoadout* Cheat::Loadout()
{
    if (!subject.is_alive()) {
        Say("you have no ship right now");
        return nullptr;
    }
    ShipLoadout* loadout = subject.try_get_mut<ShipLoadout>();
    if (!loadout) Say("your ship carries no loadout");
    return loadout;
}

Damageable* Cheat::Hull()
{
    if (!subject.is_alive()) {
        Say("you have no ship right now");
        return nullptr;
    }
    Damageable* hull = subject.try_get_mut<Damageable>();
    if (!hull) Say("your ship has no hull to work on");
    return hull;
}

int Cheat::IntArg(std::size_t index, int fallback) const
{
    int value = 0;
    if (index < args.size() && ParseInt(args[index], value)) return value;
    return fallback;
}

// Splits on whitespace and lowercases as it goes: a cheat console that cares
// whether you typed /GOD is a cheat console nobody uses.
static std::vector<std::string> Tokenize(const std::string& text)
{
    std::vector<std::string> out;
    std::string token;
    for (const char c : text) {
        if (c == ' ' || c == '\t') {
            if (!token.empty()) out.push_back(std::exchange(token, std::string{}));
            continue;
        }
        token.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (!token.empty()) out.push_back(std::move(token));
    return out;
}

static FactionState* FindFaction(flecs::world& registry, TeamId team)
{
    FactionState* found = nullptr;
    registry.each([&](FactionState& faction) {
        if (faction.team == team) found = &faction;
    });
    return found;
}

// The account of whoever is flying `ship`. Null before ResearchSystem has
// opened one, which is the first tick of the ship's life.
static PilotAccount* FindAccount(flecs::world& registry, flecs::entity ship)
{
    const PilotRef* ref = ship.is_alive() ? ship.try_get<PilotRef>() : nullptr;
    if (!ref || ref->pilotId == 0) return nullptr;

    PilotAccount* found = nullptr;
    registry.each([&](PilotAccount& account) {
        if (account.pilotId == ref->pilotId) found = &account;
    });
    return found;
}

// Case-insensitive, since the tokenizer already lowercased what was typed and
// nobody addressing a teammate should have to match their capitals.
static flecs::entity FindCallsign(flecs::world& registry, const std::string& lowercaseName)
{
    flecs::entity found;
    registry.each([&](flecs::entity ship, const Callsign& callsign) {
        if (Lowercase(callsign.name) == lowercaseName) found = ship;
    });
    return found;
}

static std::string Lowercase(std::string text)
{
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// strtol/strtod rather than stoi/stod: this parses whatever somebody typed
// into a chat box, and a fat-fingered argument is a reply, not an exception.
static bool ParseInt(const std::string& token, int& out)
{
    char* end = nullptr;
    const long value = std::strtol(token.c_str(), &end, 10);
    if (end == token.c_str()) return false;
    out = static_cast<int>(value);
    return true;
}

static bool ParseDouble(const std::string& token, double& out)
{
    char* end = nullptr;
    const double value = std::strtod(token.c_str(), &end);
    if (end == token.c_str() || !std::isfinite(value)) return false;
    out = value;
    return true;
}

static std::optional<TeamId> ParseTeam(const std::string& token)
{
    for (const TeamId team : FACTION_ROSTER) {
        if (token == TeamName(team)) return team;
    }
    return std::nullopt;
}

static const char* TeamName(TeamId team)
{
    switch (team) {
    case TeamId::Blue: return "blue";
    case TeamId::Red: return "red";
    case TeamId::Violet: return "violet";
    case TeamId::Yellow: return "yellow";
    case TeamId::Magenta: return "magenta";
    case TeamId::Cyan: return "cyan";
    case TeamId::None: break;
    }
    return "nobody";
}

static std::uint32_t AddCapped(std::uint32_t total, int amount)
{
    const std::uint64_t sum = std::uint64_t{total} + static_cast<std::uint64_t>(amount);
    return static_cast<std::uint32_t>(
            std::min<std::uint64_t>(sum, std::numeric_limits<std::uint32_t>::max()));
}

static std::string Format(const char* format, ...)
{
    char buffer[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return buffer;
}

} // namespace Gravitaris
