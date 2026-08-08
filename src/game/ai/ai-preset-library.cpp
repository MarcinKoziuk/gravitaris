#include <string>

#include <toml++/toml.h>

#include <gravitaris/game/ai/ai-preset-library.hpp>
#include <gravitaris/game/fs/ifilesystem.hpp>
#include <gravitaris/game/logging.hpp>

namespace Gravitaris {

static void ReadPersonality(const toml::table& t, AIPersonality& p);
static void ReadGuidance(const toml::table& t, GuidanceParams& g);
static void ReadFlight(const toml::table& t, FlightControllerParams& f);
static void ReadStrategy(const toml::table& t, AIStrategyWeights& w);
static void ReadFit(const toml::table& t, FitWeights& w);
static std::uint32_t NextRandom(std::uint32_t& state);

AIPresetLibrary::AIPresetLibrary()
{
    // The fallback the file replaces: every struct default, which is exactly
    // what the old Balanced preset was.
    AIPreset balanced;
    balanced.key = "balanced";
    balanced.id = ID("balanced");
    balanced.name = "Balanced";
    m_presets.push_back(std::move(balanced));
}

bool AIPresetLibrary::Load(IFilesystem& filesystem, const char* path)
{
    std::string text;
    if (!filesystem.ReadString(std::string(path), &text)) {
        LOG(warning) << "ai-presets: " << path << " not found; using the built-in default";
        return false;
    }

    toml::table root;
    try {
        root = toml::parse(text, std::string(path));
    }
    catch (const toml::parse_error& error) {
        LOG(error) << "ai-presets: " << path << ": " << error.description();
        return false;
    }

    const toml::array* entries = root["preset"].as_array();
    if (!entries) {
        LOG(warning) << "ai-presets: " << path << " has no [[preset]] entries";
        return false;
    }

    std::vector<AIPreset> presets;
    for (const toml::node& node : *entries) {
        const toml::table* entry = node.as_table();
        if (!entry) continue;

        const auto key = (*entry)["key"].value<std::string>();
        if (!key) {
            LOG(error) << "ai-presets: an entry is missing `key`; skipped";
            continue;
        }

        // Starts at the struct defaults, so an entry only has to name what it
        // actually changes -- which is why `balanced` can be empty.
        AIPreset preset;
        preset.key = *key;
        preset.id = ID(key->c_str());
        preset.name = (*entry)["name"].value_or(*key);
        preset.description = (*entry)["description"].value_or("");
        preset.weight = (*entry)["weight"].value_or(1.f);

        if (const toml::table* t = (*entry)["personality"].as_table()) ReadPersonality(*t, preset.personality);
        if (const toml::table* t = (*entry)["guidance"].as_table()) ReadGuidance(*t, preset.guidance);
        if (const toml::table* t = (*entry)["flight"].as_table()) ReadFlight(*t, preset.flight);
        if (const toml::table* t = (*entry)["strategy"].as_table()) ReadStrategy(*t, preset.strategy);
        if (const toml::table* t = (*entry)["fit"].as_table()) ReadFit(*t, preset.personality.fit);

        presets.push_back(std::move(preset));
    }

    if (presets.empty()) {
        LOG(error) << "ai-presets: " << path << " parsed but held no usable presets";
        return false;
    }

    m_presets = std::move(presets);
    LOG(info) << "ai-presets: loaded " << m_presets.size() << " presets from " << path;
    return true;
}

const AIPreset* AIPresetLibrary::Find(id_t id) const
{
    for (const AIPreset& preset : m_presets) {
        if (preset.id == id) return &preset;
    }
    return nullptr;
}

const AIPreset* AIPresetLibrary::FindByKey(std::string_view key) const
{
    for (const AIPreset& preset : m_presets) {
        if (preset.key == key) return &preset;
    }
    return nullptr;
}

const AIPreset& AIPresetLibrary::PickRandom(std::uint32_t seed) const
{
    float total = 0.f;
    for (const AIPreset& preset : m_presets) total += std::max(preset.weight, 0.f);
    if (total <= 0.f) return Default();

    std::uint32_t state = seed | 1u;
    const float roll = total * static_cast<float>(NextRandom(state) & 0xffffffu) / 16777216.f;

    float running = 0.f;
    for (const AIPreset& preset : m_presets) {
        running += std::max(preset.weight, 0.f);
        if (roll < running) return preset;
    }
    return m_presets.back();
}

void AIPresetLibrary::Apply(const AIPreset& preset, AIPilot& pilot)
{
    pilot.presetId = preset.id;
    pilot.personality = preset.personality;
    pilot.guidance = preset.guidance;
    pilot.flight = preset.flight;
}

void AIPresetLibrary::ApplyStrategy(const AIPreset& preset, AIStrategy& strategy)
{
    strategy.weights = preset.strategy;
}

static void ReadPersonality(const toml::table& t, AIPersonality& p)
{
    if (const auto v = t["engage_range"].value<double>()) p.engageRange = *v;
    if (const auto v = t["standoff_distance"].value<double>()) p.standoffDistance = *v;
    if (const auto v = t["fire_range"].value<double>()) p.fireRange = *v;
    if (const auto v = t["fire_tolerance"].value<double>()) p.fireTolerance = *v;
    if (const auto v = t["aim_priority_error"].value<double>()) p.aimPriorityError = *v;
    if (const auto v = t["grounded_threat_range"].value<double>()) p.groundedThreatRange = *v;
    if (const auto v = t["upgrade_greed"].value<std::uint32_t>()) p.upgradeGreed = *v;
    if (const auto v = t["pad_wait_ticks"].value<std::uint32_t>()) p.padWaitTicks = *v;
    if (const auto v = t["evade_radius"].value<double>()) p.evadeRadius = *v;
    if (const auto v = t["evade_margin"].value<double>()) p.evadeMargin = *v;
    if (const auto v = t["danger_lookahead_steps"].value<int>()) p.dangerLookaheadSteps = *v;
    if (const auto v = t["flee_health_fraction"].value<double>()) p.fleeHealthFraction = *v;
    if (const auto v = t["flee_range"].value<double>()) p.fleeRange = *v;
    if (const auto v = t["flee_margin"].value<double>()) p.fleeMargin = *v;
    if (const auto v = t["jink_speed"].value<double>()) p.jinkSpeed = *v;
    if (const auto v = t["jink_period"].value<std::uint32_t>()) p.jinkPeriod = *v;
    if (const auto v = t["under_fire_ticks"].value<std::uint32_t>()) p.underFireTicks = *v;
    if (const auto v = t["decision_interval"].value<std::uint32_t>()) p.decisionInterval = *v;
    if (const auto v = t["fire_interval"].value<std::uint32_t>()) p.fireInterval = *v;
    if (const auto v = t["burst_count"].value<std::uint32_t>()) p.burstCount = *v;
    if (const auto v = t["burst_shot_interval"].value<std::uint32_t>()) p.burstShotInterval = *v;
    if (const auto v = t["reaction_jitter"].value<double>()) p.reactionJitter = *v;
    if (const auto v = t["aim_jitter"].value<double>()) p.aimJitter = *v;
    if (const auto v = t["danger_ignore_chance"].value<double>()) p.dangerIgnoreChance = *v;
    if (const auto v = t["supply_reserve"].value<std::uint32_t>()) p.supplyReserve = *v;
}

static void ReadFit(const toml::table& t, FitWeights& w)
{
    if (const auto v = t["fire_rate"].value<float>()) w.fireRate = *v;
    if (const auto v = t["weapon"].value<float>()) w.weapon = *v;
    if (const auto v = t["cannon"].value<float>()) w.cannon = *v;
    if (const auto v = t["missile"].value<float>()) w.missile = *v;
    if (const auto v = t["ammo"].value<float>()) w.ammo = *v;
    if (const auto v = t["engine"].value<float>()) w.engine = *v;
    if (const auto v = t["shield"].value<float>()) w.shield = *v;
    if (const auto v = t["boost"].value<float>()) w.boost = *v;
}

static void ReadGuidance(const toml::table& t, GuidanceParams& g)
{
    if (const auto v = t["max_speed"].value<double>()) g.maxSpeed = *v;
    if (const auto v = t["accel"].value<double>()) g.accel = *v;
    if (const auto v = t["transit_speed"].value<double>()) g.transitSpeed = *v;
    if (const auto v = t["merge_range"].value<double>()) g.mergeRange = *v;
    if (const auto v = t["flip_time"].value<double>()) g.flipTime = *v;
    if (const auto v = t["arrive_radius"].value<double>()) g.arriveRadius = *v;
    if (const auto v = t["orbit_radial_kp"].value<double>()) g.orbitRadialKp = *v;
    if (const auto v = t["max_radial_speed"].value<double>()) g.maxRadialSpeed = *v;
    if (const auto v = t["touchdown_speed"].value<double>()) g.touchdownSpeed = *v;
    if (const auto v = t["flare_altitude"].value<double>()) g.flareAltitude = *v;
}

static void ReadFlight(const toml::table& t, FlightControllerParams& f)
{
    if (const auto v = t["heading_kp"].value<double>()) f.headingKp = *v;
    if (const auto v = t["heading_kd"].value<double>()) f.headingKd = *v;
    if (const auto v = t["turn_deadband"].value<double>()) f.turnDeadband = *v;
    if (const auto v = t["aim_tolerance"].value<double>()) f.aimTolerance = *v;
    if (const auto v = t["velocity_deadband"].value<double>()) f.velocityDeadband = *v;
    if (const auto v = t["min_burn_ticks"].value<std::uint32_t>()) f.minBurnTicks = *v;
    if (const auto v = t["min_coast_ticks"].value<std::uint32_t>()) f.minCoastTicks = *v;
    if (const auto v = t["restart_factor"].value<double>()) f.restartFactor = *v;
    if (const auto v = t["urgent_factor"].value<double>()) f.urgentFactor = *v;
    if (const auto v = t["position_kp"].value<double>()) f.positionKp = *v;
    if (const auto v = t["max_approach_speed"].value<double>()) f.maxApproachSpeed = *v;
}

static void ReadStrategy(const toml::table& t, AIStrategyWeights& w)
{
    if (const auto v = t["dogfight"].value<double>()) w.dogfight = *v;
    if (const auto v = t["claim"].value<double>()) w.claim = *v;
    if (const auto v = t["attack_complex"].value<double>()) w.attackComplex = *v;
    if (const auto v = t["intercept_freighter"].value<double>()) w.interceptFreighter = *v;
    if (const auto v = t["defend"].value<double>()) w.defend = *v;
    if (const auto v = t["rearm"].value<double>()) w.rearm = *v;
}

// xorshift32, seeded by the caller from sim state (ADR 0001) -- nothing here
// is global.
static std::uint32_t NextRandom(std::uint32_t& state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

} // namespace Gravitaris
