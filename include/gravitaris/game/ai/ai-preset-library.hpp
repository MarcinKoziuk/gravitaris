#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gravitaris/game/component/ai-pilot.hpp>
#include <gravitaris/game/component/ai-strategy.hpp>
#include <gravitaris/game/fwd.hpp>
#include <gravitaris/game/id.hpp>

namespace Gravitaris {

// One named AI temperament, loaded from data/ai-presets.toml. The four tuning
// blocks are already merged over their struct defaults at load, so applying a
// preset is a struct copy rather than a per-field merge.
struct AIPreset {
    id_t id = 0; // FNV of the toml key
    std::string key;
    std::string name;
    std::string description;
    float weight = 1.f; // share of a random pick

    AIPersonality personality;
    GuidanceParams guidance;
    FlightControllerParams flight;
    AIStrategyWeights strategy;
};

// The AI temperaments, parsed from data/ai-presets.toml. Shaped like
// UpgradeCatalog: data supplies the magnitudes, the sim supplies the rules,
// and adding a preset is a file edit -- there is no enum of them.
//
// A missing or malformed file leaves one built-in "balanced" preset (every
// struct default), so an AI ship always has something to fly by.
class AIPresetLibrary {
public:
    AIPresetLibrary();

    // Reads `path` (default "ai-presets.toml"). Returns false and keeps the
    // built-in fallback if it can't be read or parsed.
    bool Load(IFilesystem& filesystem, const char* path = "ai-presets.toml");

    [[nodiscard]] const std::vector<AIPreset>& All() const { return m_presets; }

    // Null for a key/id that isn't in the file. Callers that must have one --
    // spawning a ship -- use Default() instead.
    [[nodiscard]] const AIPreset* Find(id_t id) const;
    [[nodiscard]] const AIPreset* FindByKey(std::string_view key) const;

    // The first preset in the file, which is what anything unconfigured flies.
    [[nodiscard]] const AIPreset& Default() const { return m_presets.front(); }

    // Weighted pick. `seed` must come from sim state only (ADR 0001: no
    // std::rand), so a replay spawns the same temperaments.
    [[nodiscard]] const AIPreset& PickRandom(std::uint32_t seed) const;

    // Overwrites pilot.personality/guidance/flight; leaves behavior, target
    // and cooldowns alone.
    static void Apply(const AIPreset& preset, AIPilot& pilot);

    // The same preset read as strategic temperament: overwrites
    // strategy.weights only, leaving the active goal alone. Split from Apply
    // because most AI ships are dogfight fodder with no AIStrategy at all.
    static void ApplyStrategy(const AIPreset& preset, AIStrategy& strategy);

private:
    std::vector<AIPreset> m_presets;
};

} // namespace Gravitaris
