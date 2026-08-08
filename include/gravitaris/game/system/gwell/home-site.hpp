#pragma once

#include <cstdint>
#include <vector>

#include <flecs.h>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/component/team.hpp>

namespace Gravitaris {

// Somewhere a faction can come home to: its own planet carrying both a Base
// and a Colony. The Base is the pad and the Colony is the reason anyone is on
// the rock at all, which is why a respawn, a repair and an AI's trip home all
// ask this same question -- they are the same question.
bool IsHomePlanet(flecs::world& registry, flecs::entity planet, TeamId team);

// The planet a landing counts against. A High Port only ever orbits one, and
// the pairing that makes the rock home is down there, so setting down on the
// deck is setting down at that planet as far as repairs, refits and respawns
// are concerned. Returns the site's own NetId when it is already a planet,
// and 0 for a dead entity.
std::uint32_t SitePlanetNetId(flecs::entity site);

// Somewhere a ship can actually put its legs down: one of a faction's home
// planets, or a High Port it holds in orbit over one. The two are
// interchangeable for everything a pilot comes home to do -- ResearchSystem
// fits parts at either, RepairSystem gives hull back at either -- so what
// picks between them is reach, not kind.
struct HomeSite {
    flecs::entity entity; // what to set down on: the planet, or the station
    flecs::entity planet; // that planet, or the one the station orbits
    TeamId team = TeamId::None;
    std::uint32_t netId = 0;
    std::uint32_t planetNetId = 0;
    Magnum::Vector2d pos;
    Magnum::Vector2d vel;
    // What the descent flares against: the planet's surface, or the station's
    // own extent. A deck has no surface radius of its own, so this comes from
    // the model (Body::GetBoundingRadius).
    double radius = 0.0;
    bool station = false;
    bool hasLab = false;

    [[nodiscard]] bool IsValid() const { return entity.is_alive() && planet.is_alive(); }
};

// What a descent onto `site` has to flare against: a planet's own surface
// radius, or the outward extent of a station's deck, since a deck has no
// radius of its own.
double LandingRadius(flecs::entity site);

// How much room `site` takes up as something to fly around, which is a
// different question and a much bigger number for a station: a deck is one
// face of a structure that sprawls in every direction, and what has to be
// steered clear of is the sprawl.
double ObstacleRadius(flecs::entity site);

// Every landing site every faction currently holds. Gathered in one pass and
// handed to SelectHomeSite because the systems that need it are walking
// pilots when they ask, and a query run inside another query's callback
// yields nothing (see CLAUDE.md).
std::vector<HomeSite> GatherHomeSites(flecs::world& registry);

// The faction's nearest site to `from`, preferring a High Port it holds over
// the planet underneath: the deck is the shorter trip, it costs no descent
// through the well, and it serves a pilot identically. `needsLab` restricts
// the choice to sites whose planet hosts one of the faction's own labs -- the
// only places a part can actually be fitted (ResearchSystem's collector
// rule), so a trip made for one has to end at one of these or it is wasted;
// callers fall back to the unrestricted search.
//
// Only ever the faction's own: a pilot never sets down on an enemy deck.
// Ties break by NetId so every run picks the same one (ADR 0001).
HomeSite SelectHomeSite(const std::vector<HomeSite>& sites, TeamId team,
                        const Magnum::Vector2d& from, bool needsLab);

} // namespace Gravitaris
