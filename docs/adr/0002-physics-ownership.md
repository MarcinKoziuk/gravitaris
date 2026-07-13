# ADR 0002: PhysicsSystem owns Chipmunk state; ECS stores handles

Status: accepted (implemented on the entt-to-flecs branch)

## Decision

Chipmunk bodies/shapes/space references live in `PhysicsSystem`'s own slot
storage (`std::vector<PhysicsBody>` + free-list), NOT in ECS components.
Entities carry two small components instead:

- `RigidBodyDesc` — spawn intent (spaceId + Body resource). Set by spawners;
  consumed by PhysicsSystem's `OnSet` observer, which allocates a slot, builds
  the Chipmunk body/shapes, and sets `PhysicsRef` on the entity.
- `PhysicsRef` — `{index, generation}` handle into the slot storage. Plain
  data, safe under flecs's default archetype storage. `OnRemove` (fired on
  both component removal and entity destruction) frees the slot.

Sim systems resolve the handle via `PhysicsSystem::GetBody(ref)`.

## Why

The old `Physics` component owned `cpBodyUniquePtr`/`cpShapeUniquePtr`
handles directly. Under entt's sparse sets this worked (components never
move); under flecs's archetype storage it produced two successive crashes:

1. Table relocation: every component add moves the entity between tables,
   relocating the resource-owning struct while cpSpace holds raw pointers
   registered against it (bullet-despawn crash).
2. The `flecs::Sparse` opt-out used to fix (1) introduced sparse-storage
   iteration/lifecycle hazards of its own ("entity 0 does not exist" assert
   via `flecs_field_shared` during a plain `each<Physics>()`).

Rather than a third storage-specific fix, the ownership split removes the
class of bug: no query ever touches a resource-owning type again. This also
matches ADR 0001's replication split — Chipmunk handles were always meant to
be server-only, never-serialized state.

## Generational handles

Slot indices are recycled through a free-list; each slot has a generation
bumped on free. `PhysicsRef` carries the generation it was created with, so a
stale ref (e.g. an entity whose slot was bulk-freed by `UnloadSpace`, see
below) is detected instead of freeing someone else's slot. `GetBody` asserts
liveness+generation in debug; `HandleBodyRemoved` treats stale refs as a
no-op by design.

## Arena-style level teardown

Two complementary bulk mechanisms, so level unload never iterates cleanup by
hand:

- **ECS side**: parent every level-scoped entity under one root entity
  (`ChildOf`); `levelRoot.destruct()` cascades through all children. (Or tag
  everything with a session id and use `world.delete_with`.) Component
  destructors still run — this eliminates bookkeeping, not dtor cost. Prefer
  this over a fresh `flecs::world` per round: a new world requires
  re-registering all components/observers/systems.
- **Physics side**: `PhysicsSystem::UnloadSpace(spaceId)` frees every
  body/shape in the space raw (skipping the per-object `cpSpaceRemove*` the
  deleters do — pointless when the whole space dies) and then frees the space.
  This is the actual bulk-free win; the per-object path remains correct for
  individual deaths (bullets) while the space stays alive.

Unload sequence, safe in either order thanks to generations:

```cpp
m_physicsSystem.UnloadSpace(spaceId);
levelRoot.destruct();
```

## Rules for future code

- Never store Chipmunk (or any externally-registered/raw-pointer) handles in
  an ECS component. Handle + system-owned storage instead.
- Components referencing another entity's physics use `PhysicsRef` (or the
  entity + lookup), never a cached `cpBody*`.
- Hardpoints/turrets (scene-graph children) don't get their own body/slot;
  they compose local transforms onto the physics-driven root.
