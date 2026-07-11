# entt → flecs migration recipe

Why flecs (decided): first-class relationships give us the scene graph
(turrets/hardpoints on ships, bases on/around planets) and hierarchy queries;
multi-component iteration is cache-friendly by default (archetype/SoA);
observers are ergonomically better than entt sinks (capturing lambdas work);
the web-based Explorer covers the dev-tooling wish. Tradeoffs accepted:
queries are runtime-typed (typos become empty results, not compile errors),
and add/remove of components physically moves entities between tables.

API names below are flecs v4; verify against the vendored version's docs —
v3→v4 renamed several calls (`get_mut`/`ensure` etc.). Pin a release tag in
`extlibs/` (single amalgamated `flecs.c`/`flecs.h`), never master.

## Current entt surface (inventoried, small — this is a modest migration)

- `entt::registry` owned by `Game`; `GetPlayer()` returns
  `std::optional<entt::entity>`.
- Components: `Transform`, `Physics` (chipmunk body handle), `Controls`,
  `Renderable` (cgame).
- ~9 `registry.view` iteration sites: physics-system, ship-controls-system,
  bullet-lifetime-system, three renderers (only ModelRenderer2 matters;
  ModelRenderer/SimpleModelRenderer are slated for removal — delete them
  first, don't port them).
- Spawners: `EntitySpawner` (game) / `CEntitySpawner` (cgame override) do
  `create` + `emplace`.
- `entt::sigh`/`sink` in `ResourceLoader` (`OnCreate<Model>`/`OnDestroy<Model>`)
  — NOT entity-related. Per IDEAS.md, replace with a small standalone signal
  header lib. Do this FIRST; it decouples resources from the ECS choice.

## Order of work

1. Delete `ModelRenderer` + `SimpleModelRenderer` (shrinks the surface).
2. Swap `ResourceLoader` signals to a standalone signal lib (entt-free).
3. Vendor flecs into `extlibs/flecs/`, add to CMake, pin release tag.
4. Big-bang the rest in one branch-less commit series (the surface is ~6
   files; dual-running two ECSes is more work than it saves here):
   components → spawners → systems → ModelRenderer2 → `GetPlayer`/client.
5. Wire the Explorer in dev builds (see Tooling below) and verify entity
   hierarchy visually.
6. Only then: adopt relationships (scene graph) as new work, not as part of
   the mechanical port.

## Idiom mapping

| entt | flecs (v4 C++) |
|---|---|
| `registry.create()` | `world.entity()` |
| `registry.emplace<T>(e, args)` | `e.set<T>({args})` (or `e.add<T>()` for tags) |
| `registry.get<T>(e)` | `e.get_mut<T>()` / `e.get<T>()` (const) |
| `registry.try_get<T>(e)` | `e.try_get<T>()` → `const T*` or nullptr |
| `registry.all_of<A, B>(e)` | `e.has<A>() && e.has<B>()` |
| `registry.destroy(e)` | `e.destruct()` |
| `registry.view<A, const B>().each(fn)` | `world.query<A, const B>()` built once + `.each(fn)`, or a `world.system<A, const B>()` |
| `registry.on_construct<T>()`/`on_destroy<T>()` | `world.observer<T>().event(flecs::OnAdd / OnRemove / OnSet).each(fn)` — capturing lambdas OK |
| `entt::entity` + `entt::null` | `flecs::entity` + `e.is_alive()` / default-constructed |
| ad-hoc singleton entity | `world.set<T>({...})` / `world.get<T>()` singletons |

## Traps (the reason this doc exists)

- **Archetype churn**: `add`/`remove` of a component/tag moves the entity to
  another table. Never toggle tags per-frame (e.g. a `Thrusting` tag) — keep
  per-frame state as fields in a component (as `Controls::actionFlags`
  already does), or use flecs toggle-able components (bitset enable/disable)
  if tag-like queries are truly wanted.
- **Deferred operations**: inside a system/observer callback, structural
  changes (add/remove/destruct) are queued until the end of the phase, not
  applied immediately. Code that adds a component then immediately queries
  for it in the same callback will not see it. Spawner-style code that must
  see its writes should run outside `progress()` or force a sync.
- **Pointer invalidation**: component pointers/refs are into table columns;
  any structural change on that entity (or table growth) can invalidate
  them. Don't hold `T*` across anything that might add/remove components.
- **Runtime-typed queries fail silently**: a query with a wrong term matches
  nothing instead of failing to compile. When a system "does nothing",
  check the Explorer for what the query actually matches.
- **Keep our loop**: we own a fixed-timestep accumulator
  (`Game::PHYSICS_DELTA`) and the render loop. Don't adopt `flecs::app` /
  its main loop. Either call `world.progress(dt)` from our tick, or (v1 of
  the port) just keep plain functions that run cached queries — systems and
  pipelines can come later. Leave flecs multithreading OFF initially.
- **Chipmunk owns motion**: `Physics` holds a body handle; transforms are
  written back from chipmunk each tick. flecs relationships don't change
  that — a `ChildOf` scene graph must not fight the physics for entities
  that have bodies (hardpoints/turrets: children with LOCAL transforms,
  composed onto the physics-driven parent transform at render/aim time).

## Scene graph (the new capability, after the port)

- `turret.child_of(ship)` + `LocalTransform` component on children.
- World transform propagation: query with a parent term and `cascade`
  (breadth-first, parents before children) composing
  `World = parentWorld * Local`. See flecs docs "hierarchies" example.
- `e.destruct()` on a parent recursively deletes children — matches what we
  want for ship death cleaning up mounted parts.
- Ship classes map naturally to prefabs (`world.prefab()` + `IsA`) —
  fighter/interceptor/etc. as prefabs, instances inherit stats; slots as
  prefab children. Prototype this before committing the ship-part design.

## Tooling

Dev builds: `world.import<flecs::stats>(); world.set<flecs::Rest>({});`
then open https://flecs.dev/explorer — live entity/query inspection in the
browser. Gate behind a debug flag; don't ship the REST server enabled.
