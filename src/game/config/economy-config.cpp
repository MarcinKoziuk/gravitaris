#include <string>

#include <toml++/toml.h>

#include <gravitaris/game/config/economy-config.hpp>
#include <gravitaris/game/fs/ifilesystem.hpp>
#include <gravitaris/game/logging.hpp>

namespace Gravitaris {

bool EconomyConfig::Load(IFilesystem& filesystem, const char* path)
{
    std::string text;
    if (!filesystem.ReadString(std::string(path), &text)) {
        LOG(warning) << "economy: " << path << " not found; using built-in defaults";
        return false;
    }

    toml::table root;
    try {
        root = toml::parse(text, std::string(path));
    }
    catch (const toml::parse_error& error) {
        LOG(error) << "economy: " << path << ": " << error.description();
        return false;
    }

    if (const toml::table* t = root["colony"].as_table()) {
        if (const auto v = (*t)["raw_production_per_tick"].value<float>()) colony.rawProductionPerTick = *v;
        if (const auto v = (*t)["raw_cap"].value<float>()) colony.rawCap = *v;
        if (const auto v = (*t)["supply_rate"].value<float>()) colony.supplyRate = *v;
    }
    if (const toml::table* t = root["production"].as_table()) {
        if (const auto v = (*t)["conversion_rate"].value<float>()) production.conversionRate = *v;
        if (const auto v = (*t)["finished_cap"].value<float>()) production.finishedCap = *v;
        if (const auto v = (*t)["freighter_cost"].value<float>()) production.freighterCost = *v;
        if (const auto v = (*t)["self_development_cost"].value<float>()) production.selfDevelopmentCost = *v;
    }
    if (const toml::table* t = root["freighter"].as_table()) {
        if (const auto v = (*t)["transit_speed"].value<double>()) freighter.transitSpeed = *v;
        if (const auto v = (*t)["transit_acceleration"].value<double>()) freighter.transitAcceleration = *v;
        if (const auto v = (*t)["arrival_radius"].value<double>()) freighter.arrivalRadius = *v;
        if (const auto v = (*t)["cargo_unload_interval_ticks"].value<std::uint32_t>()) {
            freighter.cargoUnloadIntervalTicks = *v;
        }
        if (const auto v = (*t)["cargo_one_raw_materials"].value<float>()) freighter.cargoOneRawMaterials = *v;
    }
    if (const toml::table* t = root["conquest"].as_table()) {
        if (const auto v = (*t)["claim_ticks"].value<std::uint32_t>()) conquest.claimTicks = *v;
    }
    if (const toml::table* t = root["research"].as_table()) {
        if (const auto v = (*t)["seconds_per_upgrade"].value<double>()) research.secondsPerUpgrade = *v;
    }
    if (const toml::table* t = root["repair"].as_table()) {
        if (const auto v = (*t)["hull_per_second"].value<float>()) repair.hullPerSecond = *v;
    }

    LOG(info) << "economy: loaded " << path;
    return true;
}

} // namespace Gravitaris
