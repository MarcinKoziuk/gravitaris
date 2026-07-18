# Corrade Containers migration plan

Goal: replace `std::unique_ptr`/`std::vector`/`std::string`/`std::optional`
with their `Corrade::Containers` equivalents across `include/` and `src/`
(not `extlibs/` — vendored third-party sources are never modified, and no
dependency is forked to accept Corrade types at its own API). Same execution
discipline as `docs/networking-plan.md` / `docs/refactoring-plan.md`: **one
phase per session**, each phase ends with both targets building, the
sim-test's determinism checksum unchanged, and a commit. Corrade is already
an existing dependency (pulled in for Magnum) — no new FetchContent entry
needed, just linking `Corrade::Containers` where a TU doesn't already get it
transitively.

## Why (and why this is bounded, not free)

Corrade's containers are deliberately narrower than the STL equivalents —
that narrowness is *both* the compile-time/debug-performance win (less
template machinery, no allocator-rebind generality, no exception-safety
scaffolding for cases that can't happen here) *and* the source of every
porting risk below. Read this section before touching any file; it's the
difference between a mechanical rename and a silent behavior change.

- **`Corrade::Containers::Pointer<T>` does not support custom deleters —
  confirmed from the vendored source's own doc comment**
  (`_deps/corrade-src/src/Corrade/Containers/Pointer.h`): "*Unlike
  `std::unique_ptr`, this class does not provide custom deleters... If you
  need a custom deleter, use either `ScopeGuard` or the standard
  `std::unique_ptr`.*" It always calls plain `delete`. This is a hard
  exception, not a deferred task (see Phase A).
- **`Corrade::Containers::Array<T>` has `Array(const Array&) = delete`** —
  confirmed from the same source tree. It's move-only, full stop, with no
  implicit copy fallback the way `std::vector`'s copy constructor gives you
  one silently. Every call site that currently *copies* a vector (explicit
  copy, a lambda capturing one by value, returning a member by value into a
  context that doesn't elide, storing one in another copyable container)
  will fail to compile and needs a decision: move it instead (usually the
  right fix and often reveals the copy was accidental), or use
  `Utility::copy()` for a real, intentional deep copy. **This is the single
  biggest risk category in this whole plan — audit, don't blind-replace.**
- **Growable `Array` is free functions, not methods**: `arrayAppend`,
  `arrayResize`, `arrayReserve`, `arrayRemove`, `arrayInsert` from
  `Corrade/Containers/GrowableArray.h`, called unqualified (ADL) on a plain
  `Array<T>`. `push_back`/`emplace_back` call sites become
  `Containers::arrayAppend(vec, value)`, not `vec.push_back(value)`. `size()`
  stays a method.
- **`Corrade::Containers::String`/`StringView` interop with `std::string`
  needs an explicit include**: `Corrade/Containers/StringStl.h` (and
  `StringStlHash.h` if a `String` is ever used as a hash-map key) provides
  the conversion operators. This is the boundary shim for yaml-cpp,
  `std::filesystem`/`std::istream` APIs, and anywhere else a third-party
  signature wants `std::string` — convert at the call site, never change the
  third-party signature.
- **`Corrade::Containers::Optional<T>`** is the closest 1:1 match of the four
  — construction, `operator bool`, `operator*`/`operator->`, copy/move all
  behave like `std::optional`. Lowest-risk category; still check for
  `value_or`/monadic (`and_then`/`transform`) usage, which `Optional` may
  lack depending on the vendored version — grep before assuming.

## Hard exception (does not migrate)

- **`include/gravitaris/game/util/chipmunk-safe.hpp`**: `cpShapeUniquePtr`,
  `cpBodyUniquePtr`, `cpSpaceUniquePtr` are `std::unique_ptr<T, CustomDeleter>`
  wrapping Chipmunk's `cpShapeFree`/`cpBodyFree`/`cpSpaceFree`. Per the
  confirmed `Pointer` limitation above, there is no Corrade type that holds a
  single owned pointer *and* supports a custom deleter — `Array`'s deleter
  signature is `void(*)(T*, std::size_t)` (array deallocation, not a single
  object) and doesn't fit either. **Leave these three typedefs as
  `std::unique_ptr` permanently.** Don't attempt `ScopeGuard` here — it's a
  scope-exit action, not a storable/nullable/reassignable handle, and
  `PhysicsBody` stores these as long-lived members, not scope-local RAII.

## Phase A — `std::unique_ptr` → `Corrade::Containers::Pointer` (simple cases)

Lowest risk, closest semantic match, real compile-time win (far less
template machinery than `unique_ptr`'s allocator/deleter-template dance).
Every file below except the chipmunk-safe.hpp exception:

- [ ] `include/gravitaris/cgame/audio/audio-system.hpp` — `m_backend`
  (`std::unique_ptr<IAudioBackend>`)
- [ ] `include/gravitaris/cgame/audio/backend/miniaudio-backend.hpp` —
  `sound`/`bufferRef` (plain `std::make_unique<ma_sound>()` etc., no custom
  deleter — these are fine)
- [ ] `include/gravitaris/cgame/cgame.hpp`, `src/cgame/cgame.cpp` —
  `CreateEntitySpawner()`'s return type
- [ ] `include/gravitaris/game/game.hpp`, `src/game/game.cpp` —
  `m_entitySpawner`, `CreateEntitySpawner()`, the `Game(IFilesystem&,
  std::unique_ptr<EntitySpawner>)` constructor parameter
- [ ] `include/gravitaris/game/fs/ifilesystem.hpp`,
  `include/gravitaris/game/fs/filesystem-physfs.hpp`,
  `src/game/fs/filesystem-phys-fs.cpp` — `OpenAsStream()` returns
  `std::unique_ptr<std::istream>`; `Pointer` wraps incomplete/polymorphic
  types fine (see the header's own "Usage with incomplete types" section),
  but confirm `std::istream`'s deletion is via its own virtual destructor
  (it is) so plain `delete` (what `Pointer` does) is correct
- [ ] `include/gravitaris/ui/ui.hpp`, `src/cgame/ui/ui.cpp` —
  `m_systemInterface`, `m_fileInterface`, `m_renderInterfaceGl3`,
  `m_buttonListener` (the last is `std::unique_ptr<Rml::EventListener>` —
  confirm RmlUi doesn't take ownership back or expect a `std::unique_ptr`
  specifically anywhere `Init()` hands it off)
- [ ] `src/cgame/resource/shape.cpp`, `src/game/resource/body.cpp`,
  `src/game/resource/detail/shape-common.cpp/.hpp`,
  `src/cgame/ui/detail/file-interface.hpp`, `src/client/gravitaris.cpp` —
  sweep remaining local/factory-return usages once the above are done and
  the pattern is proven

**Done when:** every `std::unique_ptr` outside `chipmunk-safe.hpp` is
`Corrade::Containers::Pointer`; both targets build; sim-test checksum
unchanged (this phase touches no gameplay logic, purely ownership types).

## Phase B — `std::vector` → `Array` / growable `Array` (internal-only first)

Split by usage shape, since the fix differs:

- [ ] **B.1 Fixed-size / built-once-then-read** (construct via `NoInit` +
  fill, or `Array<T>{count}` + loop — never grows after construction):
  candidates from the grep list —
  `include/gravitaris/game/perf-monitor.hpp` (`HISTORY_SIZE` ring — actually
  check this one first, it's currently `std::array`, not `std::vector`;
  verify before touching), `src/game/resource/detail/casteljau.cpp/.hpp`,
  `include/gravitaris/cgame/resource/shape.hpp`,
  `src/cgame/resource/shape.cpp`. Direct `Array<T>` swap, no growable
  helpers needed.
- [ ] **B.2 Growable (push_back/emplace_back heavy)**: everything matching
  `\.push_back(\|\.emplace_back(` (25 files at last count) —
  `src/game/system/physics-system.cpp`, `ai-pilot-system.cpp`,
  `bullet-lifetime-system.cpp`, `damage-system.cpp`, `death-system.cpp`,
  `include/gravitaris/game/system/physics-system.hpp`
  (`std::vector<ImpactEvent> m_impacts`), `trajectory-predictor.hpp/.cpp`,
  `src/cgame/renderer/*.cpp`, `src/cgame/hud/indicator-renderer.cpp`. Each
  site: `Containers::arrayAppend(vec, ...)` in place of `push_back`/
  `emplace_back`; audit every place the resulting `Array` gets copied,
  returned by value, or captured by value in a lambda (the Array-is-move-
  only gotcha above) — this phase is where that risk actually bites, budget
  time for it per file, don't batch-and-hope.
- [ ] **B.3 Boundary-crossing vectors deferred to their own review**: places
  a `std::vector` is handed to or received from flecs, Chipmunk, yaml-cpp, or
  RmlUi APIs expecting `std::vector`/`std::span`/pointer+size — e.g.
  `include/gravitaris/game/resource/body.hpp`'s shape lists if they cross
  into Chipmunk shape construction, `input-log.hpp`'s command buffer if
  serialized via an API expecting `std::vector<std::byte>`-like access.
  Identify these explicitly in B.1/B.2's sweep and list them here with the
  specific boundary before deciding per-site whether to keep `std::vector`
  locally (simplest, no shim) or convert with an explicit `.data()`/`.size()`
  adapter at the call.

**Done when:** every push_back/emplace_back-driven vector not identified as
B.3 is a growable `Array`; every fixed-size vector is `Array`; sim-test
checksum unchanged (any change here is a container swap, not a logic
change — a checksum drift means a bug was introduced, not "expected drift").

## Phase C — `std::string`/`const std::string&` → `Corrade::Containers::String`/`StringView`

- [ ] **C.1** Read-only string parameters (`const std::string&` used only for
  reading, never stored) → `Containers::StringView` (no allocation on the
  caller side, matches Corrade's own convention for Magnum/Corrade APIs
  already in this codebase's dependency surface).
- [ ] **C.2** Owned/stored strings → `Containers::String`.
  `include/gravitaris/game/perf-monitor.hpp`'s
  `std::unordered_map<std::string, Section>` needs
  `Corrade/Containers/StringStlHash.h` if the key type changes (or leave the
  map keyed on `std::string` and only convert the *values* — evaluate
  per-file, this map is a debug-overlay data structure, not hot path, low
  priority either way).
- [ ] **C.3 Boundary shims** — every crossing into yaml-cpp (`body.cpp`,
  `shape-common.cpp` likely parse YAML into `std::string` fields already
  owned by yaml-cpp's own `YAML::Node` — check whether these ever need to
  become `Corrade::String` at all, or whether the shim belongs at the point
  the parsed value is *stored* in our own structs, not at the yaml-cpp call
  itself), PhysFS (`filesystem-phys-fs.cpp`'s paths), and RmlUi (`ui.cpp`,
  `file-interface.hpp`/`system-interface.hpp`/`render-interface-gl3.hpp`
  under `src/cgame/ui/detail/` — RmlUi's own API is `Rml::String`, a
  std::string-compatible typedef; confirm whether `Rml::String` accepts a
  converted `Corrade::String` implicitly via the StringStl shim or needs an
  explicit `.data()` at each call). Include `Corrade/Containers/StringStl.h`
  at each shim site, not globally.
- [ ] **C.4 Shader source strings** (`src/cgame/renderer/shader/*.cpp`, 7
  files) — likely the simplest sub-batch: these build a GLSL source string
  once at shader-compile time, no growth, no boundary crossing beyond
  handing raw bytes to Magnum's shader-compile API (which already speaks
  Corrade types) — do these first in this phase to build confidence before
  the trickier yaml-cpp/RmlUi/PhysFS boundaries.

**Done when:** every internal-only string is `Corrade::Containers::String`/
`StringView`; every third-party-boundary string has an explicit,
file-local `StringStl.h` shim (not a global include); sim-test checksum
unchanged.

## Phase D — `std::optional` → `Corrade::Containers::Optional`

Lowest-risk phase, do last (by then the team/session doing this has the
pattern down from A-C, and Optional's mechanical parity means fewer
surprises to burn a fresh context on). Files: `autopilot.hpp/.cpp`,
`camera-director.hpp/.cpp`, `cgame.hpp/.cpp`, `hud/indicator-renderer.hpp/
.cpp`, `resource/model.hpp/.cpp`, `resource/shape.hpp`,
`game/game.hpp`, `system/ai-pilot-system.hpp/.cpp`, plus the debug panels
(`flight-panel.cpp`, `spawn-panel.cpp`, `trajectory-panel.cpp`) and
`src/client/gravitaris.cpp`.

- [ ] Grep every `std::optional` use for `.value_or(`, `.and_then(`,
  `.transform(`, `.or_else(` before swapping — confirm the vendored
  `Optional.h` has equivalents (or doesn't, in which case rewrite the call
  site's logic explicitly rather than half-porting).
- [ ] `std::nullopt` → `Corrade::Containers::NullOpt`.

**Done when:** every `std::optional` outside third-party API signatures
(flecs' own `try_get` etc. return raw pointers already, not `std::optional`,
so this shouldn't come up much) is `Corrade::Containers::Optional`; sim-test
checksum unchanged.

## Verification discipline (every phase)

1. Build `GravitarisNG` and `gravitaris-sim-test`.
2. Run `gravitaris-sim-test`; state **and** event checksums must match the
   pre-phase baseline exactly (`0x1a3096e5f4b36217` / `0x2e16d87965684ca9` as
   of the Phase 2 networking commit — update this line as later phases land
   and note the new baseline if a *legitimate* behavior change ever lands in
   the same window, which it shouldn't during this migration).
3. One commit per phase (or per sub-phase for B/C, given their size) —
   small, reviewable, bisectable diffs, matching this repo's established
   convention.
4. wasm build: not required to pass locally every phase (no emsdk on this
   machine as of writing), but avoid anything Emscripten-hostile —
   Corrade::Containers is already used throughout Magnum/Corrade's own
   Emscripten-targeted builds, so this is low-risk, but flag explicitly in
   the phase's commit message if a phase touches anything in `src/client/`
   or platform-facing code that the wasm port depends on.

## Explicitly out of scope

- `extlibs/` (vendored sources) — never modified.
- Forking or patching any dependency (flecs, Chipmunk, yaml-cpp, RmlUi,
  ankerl, miniaudio, nanosvg, PhysFS) to natively accept Corrade types.
  Every boundary gets a local conversion, not an upstream change.
- `chipmunk-safe.hpp`'s three custom-deleter `unique_ptr` typedefs (see
  "Hard exception" above).
- `std::array` (fixed compile-time-sized) — not part of this ask; revisit
  separately if `Corrade::Containers::StaticArray` turns out worth it later.
