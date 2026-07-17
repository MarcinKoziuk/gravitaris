#pragma once

#include <cstdint>

namespace Gravitaris {

// SplitMix64: a tiny, self-contained deterministic RNG. Advances `state` and
// returns the next value; no global state, so it satisfies ADR 0001's
// determinism constraint (RNG only via an explicit seeded stream, never a
// process-global like std::rand). Seed it from sim-derived quantities --
// typically (tick, entity id) -- so the same tick replays bit-identically.
inline std::uint64_t SplitMix64Next(std::uint64_t& state)
{
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// The same stream mapped to a double in [0, 1) (top 53 bits -> mantissa).
inline double SplitMix64NextUnit(std::uint64_t& state)
{
    return (SplitMix64Next(state) >> 11) * (1.0 / 9007199254740992.0);
}

// Conventional seed mix for a per-(tick, id) stream, so two systems seeding
// from the same pair still diverge and one entity's stream doesn't shadow
// another's. Matches what DeathSystem/AIPilotSystem already used inline.
inline std::uint64_t SplitMix64Seed(std::uint64_t tick, std::uint64_t id)
{
    return tick * 0x9E3779B97F4A7C15ull ^ (id + 0x632BE59Bull);
}

} // namespace Gravitaris
