# Code review response (commits 279d239, 80c50b9)

27 review points. Grouped by theme, with my position on each: **agreed**,
**agreed with a caveat**, **pushing back**, or **need clarification**.
Nothing implemented yet — this is the proposal.

Delete `src/game/system/review.txt` once the grouping question below is
settled; it shouldn't outlive the review.

---

## 1. Style violations — all three confirmed, all mine, all widespread

Counted, not estimated:

| Issue | Occurrences | Verdict |
| --- | --- | --- |
| `namespace { }` instead of `static` | 23 files | **agreed**, CLAUDE.md says so explicitly |
| `} else` instead of Stroustrup | 28 sites | **agreed**, CLAUDE.md says so explicitly |
| Duplicated `PI` | 15 defs across 9 files | **agreed** |

### 1a. anon namespace → `static` — one technical caveat

CLAUDE.md's rule ("Prefer this over anon. namespace") maps cleanly onto
*functions* and *constants*, but **there is no `static` equivalent for
types**. `economy-system.cpp`'s `struct PlanetEconomy` and
`freighter-system.cpp`'s helpers-with-local-structs can't be converted
without leaking the type into the enclosing namespace.

Proposed rule, to write into CLAUDE.md so this doesn't drift again:
- file-local **functions** → `static`
- file-local **constants** → `static constexpr`
- file-local **types** → keep an anonymous namespace, containing *only* the
  types

### 1b. `PI` → shared header — naming needs a decision

You wrote `math-utils.hpp` in one comment and "math-fns" in another. Pick
one; I'd suggest `include/gravitaris/game/math-utils.hpp`. Note 3 of the 9
PI sites are outside `game/` (`cgame/net/snapshot-interpolator.cpp`,
`cgame/ui/debug/perf-panel.cpp`, `tools/sim-test/main.cpp`) — all can
include from `game/`, so that location works.

### 1c. Generic math vs. domain math — refining your proposal

You suggested putting the duplicated `WrapToPi` **and** the intercept solver
in the same place. I'd split them:

- **generic** (`PI`, `WrapToPi`, clamps/lerps) → `game/math-utils.hpp`
- **domain** (`SolveInterceptTime`, duplicated verbatim between
  `ai-pilot-system.cpp` and `structure-defense-system.cpp`) → `game/gnc/`,
  next to `flight-controller`/`behaviors` where the rest of the guidance
  math already lives

Intercept solving is ballistics, not arithmetic. Burying it in a `math-utils`
grab-bag would make it undiscoverable for the next thing that needs to lead
a target. Note the existing comment claims the duplication is deliberate
("not shared since…") — with two copies plus two `WrapToPi` copies, that
justification has expired.

---

## 2. `OwnShipSync` in `game/net/` — you're right, and here's the proof

**Agreed, unreservedly.** `src/game/net/` contains 10 `.cpp` files. Nine are
compiled by `gravitaris-sim-test` and/or `gravitaris-server`.
`own-ship-sync.cpp` is the **only one no headless target builds** — it's
reachable exclusively from the GL client. That's mechanical evidence it's in
the wrong module, independent of taste.

It also holds `Magnum::Vector2 m_visualCorrectionOffset` and decays it by
**wall-clock `dtSeconds`** — presentation-time, not sim ticks. By this
codebase's own convention (see `CGame`'s comment: "presentation-only and
driven by real time, not the fixed sim tick") that's `cgame/` state.

**Action**: move to `cgame/net/own-ship-sync.{hpp,cpp}`.

### The criterion matters more than this one file

"It's client-only, so it belongs in cgame" would be the wrong lesson — it
would wrongly drag `NetClient` and `ClientPrediction` along, and those are
correctly in `game/net/`. The line that actually holds:

> A thing belongs in `game/` if a **headless target uses it** (sim-test /
> server) **and** it holds no presentation-time or presentation-space state.

- `NetClient`, `ClientPrediction` — sim-tested, tick-based → `game/` ✓
- `PredictedTickClock` — sim-tested (`TestPredictedTickClock`), pure integer
  ticks → `game/` ✓ (staying put)
- `OwnShipSync` — no headless user, holds a visual offset decayed in
  wall-clock seconds → `cgame/` ✗ (moving)

---

## 3. `CosmeticBulletReaper` — agreed, name is bad for two reasons

Not just tonally: in a **space combat game**, `Reaper` reads as a ship or
enemy class (and it is literally a Mass Effect faction). Worse, "reaper" is
a term of art for cleaning up things that are *already dead* (Unix zombie
reaping) — but this class **decides** the bullet is dead. The name is
imprecise as well as confusable.

Candidates, best first:

| Name | For | Against |
| --- | --- | --- |
| `CosmeticBulletDespawner` | "despawn" is standard game vocabulary and you used it yourself describing this exact bug | slightly long |
| `CosmeticBulletHits` | short, matches `CheckLocalHits`/`MatchImpact` | reads like a data container, not an actor |
| `PredictedBulletCollision` | says what it approximates | "Collision" overpromises — it's a radius check, not real collision |
| `CosmeticBulletCleanup` | plain | vague about *when* |

**Recommendation: `CosmeticBulletDespawner`.**

Open question worth settling at the same time: this class iterates entities
and destroys them, which is exactly what everything named `*System` in this
codebase does (15 of them). But its neighbours in `net/` don't use that
suffix (`SnapshotApplier`, `SnapshotInterpolator`, `ClientPrediction`). So
either name is defensible — do you want `net/` exempt from the `*System`
convention as a rule?

---

## 4. Cap'n Proto for snapshots — **pushing back on the fix, agreeing on the problem**

The problem you're pointing at is real, and it's worse than "ugly":
`SerializeSnapshot` and `ReadSnapshot` are two parallel field lists that
must be kept in the same order by hand. Any drift is silent wire
corruption, and every new field needs edits in both plus a
`SNAPSHOT_VERSION` bump. I've bumped that version 3 times this month and
each one was a chance to get it wrong.

**But Cap'n Proto is the wrong tool for this specific path**:

1. **It would make packets bigger.** The value in this format is bit-level
   quantization — `WriteQuantizedFloat(value, min, max, 16)` packs a float
   into 16 bits. Cap'n Proto gives fixed-width fields and zero-copy reads;
   it has no equivalent. At 60Hz × N entities, snapshot size is the one
   budget that actually matters, and this trade goes the wrong way.
2. **Zero-copy buys us nothing here.** We immediately deserialize into
   `SnapshotData` and interpolate; we never hold the wire buffer.
3. **It's a heavy dependency for the wasm build**, which we keep lean
   deliberately.

**Counter-proposal — kills the actual bug class, no dependency, keeps
quantization**: one templated visitor both directions share.

```cpp
template <typename Archive>
void Visit(Archive& ar, EntityState& e)
{
    ar.U32(e.netId);
    ar.Quantized(e.pos.x(), -WORLD, WORLD, 16);
    // ... one list, one order, one place
}
```

`WriteArchive` writes, `ReadArchive` reads. Drift becomes structurally
impossible rather than merely discouraged. Roughly an afternoon's work.

If you specifically want a *schema language* (versioning, cross-language
clients, tooling), that's a different and legitimate goal — but then the
conversation is FlatBuffers vs. Cap'n Proto vs. hand-rolled, and it should
be its own decision with the packet-size cost measured first, not folded in
as a cleanup.

---

## 5. flecs system API — **pushing back on migrating, but you're near a real perf win**

Your question: should systems register via flecs' system/pipeline API and
run under `world.progress()` instead of manual `m_xSystem.Update()` calls?

**My answer: no for scheduling, yes for queries.**

Against adopting the pipeline:
- **Determinism is the project's load-bearing invariant** (ADR 0001,
  sim-test's checksum). One function with 15 explicit calls in a fixed order
  is trivially auditable. Pipeline phases make ordering implicit, and the
  headline benefit — automatic multithreading — is actively dangerous here.
- The ordering comments in `Game::Update` are documentation of real bugs
  ("DamageSystem applies this step's bullet hits… so DeathSystem (next) sees
  final hp"). Those constraints would become phase annotations spread across
  15 files.
- Our "systems" aren't pure ECS systems anyway — `PhysicsSystem` owns
  Chipmunk spaces, most hold `EntitySpawner&`/`GameEventQueue&`. They're
  objects with an `Update()`, and that's fine.

**The real win you're circling**: we call `world.each(...)` everywhere, which
builds an **uncached query on every call, every tick**. Storing
`flecs::query` members (built once in each system's constructor) gets the
caching benefit — flecs' actual performance story — with zero scheduling
change and zero determinism risk. Given the frame-time work earlier, that's
worth measuring. `FactionSystem::Update` alone runs 4 separate `each` passes
per tick.

Separately: flecs' REST/explorer introspection is genuinely nice for
debugging and doesn't require adopting the pipeline. Could be a debug-only
build flag.

---

## 6. Registry construction order — **your instinct is right, your fix has a cost**

You asked why `m_registry` isn't constructed first and passed in, and
whether the scary delegating-constructor comment is still needed.

The hazard is real: `CreateEntitySpawner()` is evaluated as a *constructor
argument*, i.e. before `m_registry` exists, which is why there's both a
paragraph of UB explanation and a separate `m_entitySpawner->Init()` call in
the body.

**Passing the registry in from outside** does fix it, but moves world
*ownership* to every caller (`Game`, `CGame`, sim-test's ~8 `Game game(fs)`
sites, server) and makes lifetime a caller problem.

**Cheaper fix with the same benefit**: keep `m_registry` owned by `Game`,
but replace the virtual `CreateEntitySpawner()` with a factory invoked in
the constructor **body**, after every member is alive:

```cpp
Game(IFilesystem&, SpawnerFactory factory);  // factory(m_registry, m_resourceLoader)
```

That removes the vtable hazard **and** the separate `Init()` wart **and**
the explanatory comment — answering your third question: no, it would no
longer be needed. `CGame` passes a different factory instead of overriding a
virtual.

**Which do you want?** I lean factory-in-body (smaller blast radius), but
external ownership is defensible if you foresee tools that want to build a
world before a `Game`.

---

## 7. `NetId` rename — agreed, with scope warnings

You're right that the name lies: `EntitySpawner::AssignNetId` runs in
single-player too, and `EntityForNetId` is how `FactionSystem`,
`FreighterSystem`, and `StructureAttachmentSystem` resolve references with
no network involved.

**Suggestion: `StableId`** — it matches the header's own justification
("replication-stable identity… never serialize raw flecs ids, they're
process-local"). Alternatives: `PersistentId`, `SimId`. I'd avoid
`EntityId` (collides mentally with `flecs::entity`'s id).

Two warnings before pulling this trigger:
1. It's a **large mechanical diff** — the identifier is in the wire
   protocol, every system, and sim-test. Wants its own isolated commit with
   no behaviour changes riding along.
2. `docs/networking-plan.md` and ADR 0001 use "NetId" in prose. Renaming
   code without the docs creates exactly the drift this project has been
   good about avoiding.

---

## 8. Configurability + YAML replacement — **pushing back on bundling these**

Two changes of very different risk, in one comment:

- **Bind address/port configurable**: small, safe, obviously right. The port
  is already `argv[1]`-ish; the bind address is hardcoded `0.0.0.0`.
- **Replacing YAML**: touches *every* asset file in `data/models/**` plus
  the loader. Asset-wide migration.

I'd do the first now and treat the second as its own decision.

**Clarifying question**: what actually needs to be *file*-based? For a
server, `--bind`/`--port`/`--tick-rate` flags (plus env vars) may cover it
entirely and need **no config format at all**. The YAML in this project is
currently *asset metadata* (`scale:`, `physics:`), which is a different
problem from server config.

If we do replace it for assets: **toml++** (header-only, C++17,
Emscripten-clean, genuinely good error messages) is my pick over JSON
(no comments, noisy for hand-authored data) — CLAUDE.md already flags
yaml-cpp as wanting replacement, so the appetite is pre-existing.

---

## 9. `magic_enum` — agreed, with one gotcha and a bigger payoff than you asked for

Viable: header-only, C++17, works under MSVC/Clang/Emscripten.

**Gotcha**: your parsers take lowercase (`"blue"`) while the enumerators are
`Blue`. `magic_enum::enum_cast<TeamId>(name)` is case-sensitive by default;
you'd need the comparator overload. Not a blocker, just don't expect a
one-liner.

**Bigger win than parsing**: `enum_name()` would improve logging and the
debug UI everywhere we currently print raw integers for `TeamId`,
`StructureType`, `BuildOrder`, `GameEventType`, `AIPersonalityPreset`. That's
a better argument for adopting it than the two parse functions.

---

## 10. Straightforward agrees (no debate)

| # | Item | Notes |
| --- | --- | --- |
| 10a | `src/server/main.cpp` → `gravitaris-server.cpp` | matches target name |
| 10b | `namespace Gravitaris { namespace Log {` → `namespace Gravitaris::Log {` | C++17; sweep for others |
| 10c | `cpVect g = cpBodyGetPosition(body);` unused | **confirmed dead**, delete |
| 10d | Server spawn loop → debug utility in `game/` | note `Game::SpawnRandomAIShip` already exists — the server is duplicating it; generalize that instead of writing a third spawner |
| 10e | `Autopilot` → `game/gnc/` | **verified zero cgame/GL deps** — header and .cpp include only `game/`. Clean move, and it does unlock reuse for AI |
| 10f | `ReplayController` → `cgame/` | **verified zero client/SDL deps** (only `game/component/controls` + `game/input/input-log`). Suggest `cgame/input/` |
| 10g | `client/gravitaris.cpp` slimming | agreed in principle — needs a concrete pass to say *what* moves; `tickEvent`'s fixed-step accumulator is arguably cgame's, the SDL/window/DPI handling is genuinely client's |
| 10h | `isocline` for server REPL | fine, low priority (dev-only tool). Your "separate deps per binary" point is the important half and mostly already true — server links `game/` only |

---

## 11. `EvaluateOrbit` "redundant casts" — **partially agreed; one cast must stay**

Correctly redundant (all `float` → `double`, safe implicit widening):
`orbitTheta`, `orbitAngularSpeed`, `orbitRadius`, `orbitCenter.x()/.y()`.

**Do not remove** these two:

```cpp
const double elapsedSeconds =
        (static_cast<double>(atTick) - static_cast<double>(baseTick)) * Game::PHYSICS_DELTA;
```

`atTick` and `baseTick` are `std::uint64_t`. The function's own comment
notes `atTick` can legitimately be **behind** `baseTick` during
reconciliation replay. Subtracting first would wrap around unsigned and
produce a ~1.8e19 tick delta — planets would teleport. The casts are the
guard. Worth a short comment saying so, since they look removable.

---

## 12. `src/game/system/` grouping — proposal

16 entries in one folder, which is over CLAUDE.md's own "don't let one
folder get too large" line.

```
src/game/system/
  core/      physics-system  orbit-system  structure-attachment-system
  combat/    damage-system  death-system  bullet-lifetime-system  structure-defense-system
  ship/      input-system  ship-controls-system  ai-pilot-system  landing-state-system
  gwell/     conquest-system  faction-system  economy-system  freighter-system
```

Rationale: 3–4 files each, and `gwell/` maps 1:1 onto
`docs/gravity-well-mode-plan.md`, which is already how the mode's work is
tracked. `structure-attachment` sits in `core/` rather than `gwell/` because
it's generic "ride a planet's motion" mechanics that freighters reuse.

**Two questions before I do this:**
1. **Mirror the split in `include/gravitaris/game/system/`?** Consistent,
   but doubles the churn and touches every `#include` site.
2. It's ~16 path updates × 3 CMake targets + all includes — a big
   no-behaviour-change diff. Own commit, done when nothing else is in
   flight?

---

## 13. Open question I need you to answer

`src/game/net/snapshot.cpp`, above `EvaluateOrbit`:

```cpp
// Claude: redundant casts and namespaces
// also
```

The "also" is unfinished — what was the second half?
