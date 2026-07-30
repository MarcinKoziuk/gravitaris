# Sector generation + 4 factions — implementation plan

Status: plan written 2026-07-29. Scope settled with Marcin the same day. No
implementation started.

This is the development increment that closes out `gravity-well-mode-plan.md`
Phase 6 ("Sector generation — deliberately late") and its U4 round-setup
screen, and widens the mode from the hardcoded blue-vs-red arena to N
factions with 4 as the default. Read Phase 6 and U4 in that document first;
this one supersedes their bullet lists and they now point here.

**How to use this doc**: same convention as `gravity-well-mode-plan.md` —
phases ordered by dependency, each individually completable and testable,
each with a "Done when" gate. Work one at a time; update the checkboxes and
add a "Verification status" note per phase as you go.

## Scope decisions (settled with Marcin 2026-07-29 — don't relitigate)

- **Layout: random star field with a fairness pass.** Stars are scattered by
  a seeded stream, then the layout is *scored* and re-rolled if a faction
  would start meaningfully worse off than its rivals. Not a symmetric ring —
  a hand-shaped ring reads as a level, and the point of generation is that
  no two rounds are the same.
- **Faction count is parametrized, default 4.** `SectorParams::factionCount`
  spans 2–6 (the number of usable `TeamId` colours). 1v1 stays reachable for
  quick testing without keeping a separate code path.
- **The classic scenario stays.** `BuildClassicScenario` remains the fixed,
  known-good arena that sim-test's existing proofs run on, so this
  increment's determinism work has a stable reference to compare against.
  Generated sectors become the default for real rounds; the classic layout
  is *not* re-expressed as a seed.
- **U4's round-setup screen is in scope**, grown out of the intro dialog
  that already exists (`data/ui/main.rml` — it already has a side-picker
  dropdown). U4's **round-over screen is not** in this increment.
- **The sector gets bigger**, but the bodies do not: star/planet radii and
  the orbit radii around a given star stay at their current values. What
  grows is the number of stars and the space between them.

## Invariants (inherited from gravity-well-mode-plan.md — all still apply)

- All generation is headless `game/` code. `cgame/` gets the extent for the
  minimap and nothing else.
- No wall clock, no `std::rand`, no iteration-order dependence. Every random
  draw goes through `SplitMix64*` (`include/gravitaris/game/util/splitmix.hpp`)
  off an explicit seed. **Generation runs once at world build**, so its
  stream is seeded from `SectorParams::seed` rather than a tick.
- The fairness pass's rejection loop must be bounded and deterministic:
  same seed ⇒ same number of attempts ⇒ same sector, on every platform.
- `gravitaris-sim-test`'s two-run checksum must stay stable after every
  phase.

## Reference scale

The classic arena is the unit of measure: two suns at `±11200` (separation
`22400`), planets on orbits of `4000`–`9600`, and `MinimapRenderer`'s
`worldRadius` at `24000`.

Starting point for the generated sector (tune during S1):

- Sector radius `≈ 11200 * sqrt(starCount)` — 4 stars lands near `22400`,
  6 stars near `27400`. Area grows with star count so density stays roughly
  constant rather than the map getting crowded or empty as `stars` changes.
- Minimum star separation `18000`, comfortably clear of the largest planet
  orbit (`9600`) on both sides so two stars never share planets.

## S1 — The generator

Goal: a deterministic, parametrized sector exists and can be built, with
nothing else in the game aware of it yet.

- [ ] `include/gravitaris/game/scenario/sector-scenario.hpp`:

  ```
  struct SectorParams {
      std::uint32_t seed = 0;
      int factionCount = 4;   // 2..6
      int stars = 4;          // 2..6
      int minPlanetsPerStar = 1;
      int maxPlanetsPerStar = 3;
  };

  struct SectorLayout {
      std::vector<flecs::entity> homes;  // one per faction, in roster order
      double extent = 0.;                // world radius covering every body
  };
  ```

- [ ] `src/game/scenario/sector-scenario.cpp`:
  `BuildSectorScenario(EntitySpawner&, const SectorParams&) -> SectorLayout`.
  Star placement by rejection sampling inside the sector disc against the
  minimum-separation rule, with a bounded attempt count (fall back to the
  best-separated candidate rather than looping forever). Per-star planet
  count, orbit radius, phase and direction all drawn from the same stream.
  Follow `classic-scenario.cpp` exactly for the spawn calls and the
  `effectiveMass` idiom — the orbit angular speed derivation is not
  something to re-derive here.
- [ ] `extent` computed from the outermost body's orbit apoapsis, not from
  the sector radius constant, so the minimap gets the real number.

Done when: calling it twice with one seed produces identical worlds, and
star/planet counts land inside the parameter bounds.

## S2 — Fairness pass + home selection

Goal: every faction starts somewhere defensible and comparable.

- [ ] Home selection: greedy max-min distance over the candidate planets
  (first home arbitrary-but-seeded, each subsequent one the planet furthest
  from all homes chosen so far), `factionCount` of them.
- [ ] Layout score, computed from the chosen homes:
  - spread of each home's distance to its nearest rival home (a faction
    with a neighbour twice as close as everyone else's is unfair);
  - count of unclaimed planets within an "expansion neighbourhood" radius
    of each home (a faction with nowhere to grow is unfair);
  - homes must not share a star.
- [ ] Reject-and-re-roll: derive the next attempt's seed from the current
  stream, up to a fixed attempt cap; keep the best-scoring layout seen.
  Deterministic by construction — the attempt count is a function of the
  seed alone.
- [ ] The chosen homes come back in roster order, so faction *i* always gets
  `homes[i]` for a given seed.

Done when: over a sample of seeds, no faction's nearest-rival distance or
neighbourhood planet count is an outlier, and the same seed always yields
the same assignment.

## S3 — N-faction bootstrap

Goal: rounds actually run on a generated sector with 4 sides.

- [ ] Faction roster: an ordered `TeamId` list (`Blue, Red, Green, Yellow,
  Magenta, Cyan`), truncated to `factionCount`. One place, shared by the
  sim, the server and the UI — `TeamId::None` is never in it.
- [ ] `Game::BuildWorld` (`src/game/game.cpp:63`): take `SectorParams`,
  build the sector, then loop the roster calling `BuildStartingComplex` per
  home — replacing the hardcoded blue/red pair. Keep a classic-scenario
  overload for sim-test.
- [ ] `Game::SpawnCombatants` (`src/game/game.cpp:74`): `AddAIFaction` for
  *every* non-human faction in the roster, not just the one opponent.
- [ ] `gravitaris-server.cpp:222`: same treatment, plus CLI arguments for
  seed / faction count / star count.

Done when: a single-player round starts on a generated 4-faction sector
with four developed complexes and three AI leaders in the field.

## S4 — Roster widening (multiplayer + console)

- [ ] `NetServer::AUTO_ASSIGN_ROSTER`
  (`include/gravitaris/game/net/net-server.hpp:75`) is currently a static
  `{Blue, Red}`. Drive it from the round's roster so peers round-robin
  across all four sides.
- [ ] Team-name parsing covers the whole roster in both places that hold a
  blue/red-only copy: `gravitaris-server.cpp:90` and
  `src/cgame/ui/ui.cpp:375`.

Done when: four clients joining a server land on four different factions,
each at its own starting complex.

## S5 — Minimap extent

- [ ] `MinimapRenderer::Config::worldRadius`
  (`include/gravitaris/cgame/renderer/minimap-renderer.hpp:42`) is hardcoded
  to `24000`. Drive it from `SectorLayout::extent`. A generated sector is
  otherwise either clipped or lost in a corner of the map.
- U1's real sector-map work (team-coloured rings, ship dots) stays out of
  this increment.

Done when: the whole generated sector fits the minimap with every star
visible, at both 2 and 6 stars.

## S6 — U4 round setup screen

Goal: the seed and the round's shape are chosen in the UI, not recompiled.

The intro dialog already exists and already carries a side-picker
(`data/ui/main.rml`, `UI::SetIntroConfirmCallback`), so this grows that
dialog rather than building a new document.

- [ ] Callback carries a `RoundSetup { TeamId team; std::uint32_t seed; int
  factionCount; std::string aiPreset; }` instead of a bare `TeamId`.
- [ ] Seed field: text input, blank means "pick one" (derived from
  something the sim already has, *not* the wall clock — see the invariants).
  Show the seed actually used, so a good round can be replayed.
- [ ] Faction count select (2–6, default 4). The side dropdown is rebuilt
  from the roster, so picking 4 offers exactly the four live colours —
  replacing the hardcoded blue/red `<option>`s and the comment above them.
- [ ] Per-AI personality preset select, from `AIPresetLibrary`
  (`data/ai-presets.toml`) — one shared preset for all AI factions first;
  per-faction presets only if it falls out cheaply.
- [ ] Round-over screen: **not in this increment** (still tracked in
  `gravity-well-mode-plan.md` U4).

Done when: you can start a 4-faction round on a chosen seed entirely from
the setup screen, and re-entering the same seed reproduces the sector.

## S7 — Sim-test + verification

- [ ] `TestSectorGeneration` in `tools/sim-test/main.cpp`:
  - same seed twice ⇒ identical checksum;
  - different seed ⇒ different layout;
  - star and per-star planet counts inside the parameter bounds;
  - `factionCount` distinct homes, no two on the same star, each carrying a
    full starting complex;
  - the S2 fairness invariant holds across a sample of seeds.
- [ ] Existing proofs keep running on `BuildClassicScenario` and keep
  passing unchanged.
- [ ] All four targets build clean: native `GravitarisNG`, wasm
  `GravitarisNG`, `gravitaris-sim-test`, `gravitaris-server`.
- [ ] Two-run determinism checksum stable.

Done when: the above passes and a hand-played 4-faction round on a
generated sector runs without incident.

## Build order

S1 → S2 → S3 → S5 → S7 (first pass) → S4 → S6 → S7 (final). S5 lands early
because a sector you can't see on the minimap is very hard to evaluate by
hand; S4 and S6 are the two that don't block anything else.
