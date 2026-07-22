# Open review items (commits 279d239, 80c50b9)

Everything below is still unimplemented. See `docs/code-review-response.md`
for the full discussion/reasoning behind each position. The original
`// Claude: ...` comment is quoted verbatim under each item so nothing gets
lost if the source comment itself is later trimmed or moved.

---

## 1. Math utils / gnc split

Agreed (see code-review-response.md #1b/#1c): generic math (`PI`, `WrapToPi`)
→ `include/gravitaris/game/math-utils.hpp`; domain math (`SolveInterceptTime`,
currently duplicated verbatim in two files) → `game/gnc/`, next to
`flight-controller`/`behaviors`.

- `src/game/system/combat/structure-defense-system.cpp:27`
  > `// Claude: we have so many duplicates, please create a math utils: include/gravitaris/game/math-utils.hpp (move PI there too)`
- `src/game/system/combat/structure-defense-system.cpp:35`
  > `// Claude: how can we best share code? I think small generic math units can go into math-fns`
- `src/game/system/combat/death-system.cpp:20`
  > `// Claude: you like to make a lot of Pies!`
- `src/server/gravitaris-server.cpp:39`
  > `// Claude: we have hundreds of these, can you add it to gravitaris.hpp?`

## 2. `magic_enum` adoption

Agreed (code-review-response.md #9), with a gotcha: parsers take lowercase
(`"blue"`) while enumerators are `Blue` — `magic_enum::enum_cast` is
case-sensitive by default, needs the comparator overload. Bigger payoff than
the two parse functions: `enum_name()` for `TeamId`/`StructureType`/
`BuildOrder`/`GameEventType`/`AIPersonalityPreset` logging and debug UI.

- `src/server/gravitaris-server.cpp:82`
  > `// Claude: can we use https://github.com/Neargye/magic_enum for this stuffie?`
- `src/server/gravitaris-server.cpp:93`
  > `// Claude: can we use https://github.com/Neargye/magic_enum for this stuffie?`

## 3. `isocline` for the server REPL

Agreed, low priority (dev-only tool).

- `src/server/gravitaris-server.cpp:49`
  > `// Claude: lets use isocline`

## 4. Stroustrup if/else style violation

Straightforward style fix (CLAUDE.md), not yet applied at this call site.

- `src/server/gravitaris-server.cpp:114`
  > `// Claude: you are forgetting use stroustrup if/else style (CLAUDE.md)`

## 5. Server debug spawn utility

`Game::SpawnRandomAIShip` already exists — generalize that instead of writing
a third spawner (code-review-response.md #10d).

- `src/server/gravitaris-server.cpp:130`
  > `// Claude: It's ugly that it's here, make a debug utility in game/ or something to spawn shit`

## 6. Server bind address/port configurability

Agreed, small and safe (code-review-response.md #8) — bind address is
hardcoded `0.0.0.0`. Treated separately from the YAML/config-format question,
which is now resolved (toml++).

- `src/server/gravitaris-server.cpp:198`
  > `// Claude: this binding must be configurable`

## 7. `client/gravitaris.cpp` slimming

Agreed in principle (code-review-response.md #10g) — needs a concrete pass to
say *what* moves; `tickEvent`'s fixed-step accumulator is arguably cgame's,
the SDL/window/DPI handling is genuinely client's.

- `src/client/gravitaris.cpp:162`
  > `// Claude: there is stuff here that would belong to cgame. client/ is only for wiring everything up and maybe later platform- specific startup  logic`

## 8. `NetId` → `StableId` rename

Agreed (code-review-response.md #7), with scope warnings: large mechanical
diff across the wire protocol/every system/sim-test, wants its own isolated
commit with no behavior changes riding along; `docs/networking-plan.md` and
ADR 0001 also say "NetId" in prose and need updating alongside the code.

- `include/gravitaris/game/component/net-id.hpp:7`
  > `// Claude: any suggestions other name than "Net" ID ? Since it's also relevant for single player?`

## 9. `camera-director.cpp` cleanup

Two separate asks, neither done yet:

- `src/cgame/camera-director.cpp:14`
  > `// Claude: move to a new camera/ folder`
- `src/cgame/camera-director.cpp:15`
  > `// Claude: i've told you/documented I prefer static over anon. namespace`

## 10. Registry construction order — waiting on your call

Your instinct is right (code-review-response.md #6): `CreateEntitySpawner()`
runs as a constructor argument before `m_registry` exists. Cheapest fix with
the same benefit: keep `m_registry` owned by `Game`, replace the virtual
`CreateEntitySpawner()` with a factory invoked in the constructor **body**
(`Game(IFilesystem&, SpawnerFactory factory)`), which removes the vtable
hazard, the separate `Init()` call, and the explanatory comment. Alternative:
pass the registry in from outside, but that moves world ownership to every
caller (`Game`, `CGame`, ~8 sim-test call sites, server). I lean
factory-in-body; still need your answer on which one.

- `src/game/game.cpp:42`
  > `// Claude: if registry must be initiated first, why don't we construct it before, and pass it to Game::? (could be even moved?)`
- `src/game/game.cpp:65`
  > `// Claude: am confused, is this still needed via my suggestion for m_registry?`

