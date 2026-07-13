# Vertical slice: component & system inventory

Scope: the "simple game mode" from IDEAS.md (solar system, sun + planet,
bases, 1 human + AI opponents, nothing persists between rounds). Decided in
IDEAS.md: fighter only, planets are conquered by landing (after destroying
anything built), player respawns on ship death (possibly at a cost), no fuel
concept, asteroids are shootable Asteroids-style. Suggested build order:
**slice zero** first — a 1v1 dogfight around one planet with no bases/economy
— then layer bases + conquest, then AI strategy.

Field lists are starting points, not contracts. The *replication class* of
each component IS a contract — see `docs/adr/0001-netcode-model.md`. Rule of
thumb: replicated components are POD, reference other entities only by
`NetId`, and live in `include/gravitaris/game/component/`.

## Components — replicated (game/, serialized server → client)

| Component | Fields (sketch) | Notes |
|---|---|---|
| `NetId` | `u32 id` | server-assigned, stable; never serialize ECS ids |
| `Transform` | pos, rot, scale | EXISTS |
| `Kinematics` | vel, angVel | extracted from chipmunk each tick for replication; chipmunk body itself is not replicated |
| `Team` | `u8 team` | owner/faction |
| `ShipClass` | enum / prefab ref | fighter, interceptor, shuttle, artillery, capital |
| `Subsystems` | hull, engines, weapons, shields: {hp, max} | IDEAS damage model: subsystem-local damage, repairable over time |
| `EnergyDist` | weapons/shields/engines weights, backup | power distribution, not a battery |
| `WeaponMount` | slot idx, weapon type, ammo, cooldown | child entity of ship (scene graph), `LocalTransform` for muzzle |
| `Projectile` | type, damage, ownerNetId | bullets: gravity-immune, `Lifetime` despawn (already implemented behavior) |
| `MissileGuidance` | targetNetId, detonation params | missiles ARE gravity-affected (IDEAS) |
| `GravityWell` | mu (GM) | on sun/planet; sim applies to ships + missiles, not bullets |
| `OrbitAnchor` | center, radius, angular vel, phase | kinematic orbiting bases — fixed path per IDEAS optimization note |
| `Base` | type, buildState | slice-one; starts with what Gravity Well has (guns, radar) |
| `PlanetOwner` | team, upgrade progress | flips on successful landing; auto-upgrade economy ticks here |
| `Asteroid` | size/split state | shootable, splits Asteroids-style |
| `Lifetime` | seconds remaining | EXISTS (bullet despawn) |

## Components — client-only (cgame/)

| Component | Purpose |
|---|---|
| `Renderable` | EXISTS — model resource ptr |
| `InterpBuffer` | last N snapshots for remote-entity interpolation |
| `Predicted` | tag/state for the local player ship only |
| `ThrustFx`, muzzle flashes, etc. | presentation triggered from replicated state changes, never authoritative |

## Components — server-only (game/, never serialized)

| Component | Purpose |
|---|---|
| `PhysicsRef` | EXISTS — generational handle into PhysicsSystem's slot storage, which owns the chipmunk body/shape handles (see docs/adr/0002-physics-ownership.md) |
| `RigidBodyDesc` | EXISTS — spawn intent (spaceId + Body resource), consumed by PhysicsSystem's observer |
| `InputQueue` | tick-stamped commands per controlled ship (human or AI both feed this) |
| `AIPilot` | flight behavior state (intercept/orbit/evade). WARNING: gravity-aware pilot AI is the highest-risk item in the slice — prototype before building strategy AI on top |
| `AIStrategy` | build/attack decisions (slice-one) |
| `LagCompHistory` | ring buffer of transforms for hitscan rewind (can be a resource keyed by NetId instead of a component) |

## System order (fixed tick, server-authoritative)

Server tick:
1. Collect `InputQueue` commands for this tick (network + AI pilots)
2. Ship controls: commands → forces/torques on chipmunk bodies (EXISTS as ship-controls-system)
3. Gravity: apply well forces to ships + missiles
4. Weapons: cooldowns, energy budget check, spawn projectiles, hitscan (with lag-comp rewind)
5. Missile guidance: steer toward target
6. Chipmunk step (EXISTS as physics-system) → write back Transform/Kinematics
7. Damage resolution + subsystem repair ticks (incl. hard-landing damage)
8. Landing/conquest resolution: gentle touchdown on undefended planet flips
   `PlanetOwner`; respawn dead players at an owned planet
9. `Lifetime` expiry (EXISTS as bullet-lifetime-system)
10. Snapshot emit (slice zero: full state at low rate is fine)

Client frame:
1. Sample input → send command (and apply locally to predicted ship)
2. Reconcile predicted ship on snapshot receipt
3. Interpolate remote entities from `InterpBuffer`
4. Render (ModelRenderer2 + GlowPostProcess — untouched by all this)

Single-player runs the same split in-process (server tick + client frame in
one loop, commands piped directly) — that's what keeps the netcode
constraints honest from day one.

## Scene graph shape (flecs, post-migration)

```
sun
planet (GravityWell)
├── base (OrbitAnchor or surface Transform)
ship (PhysicsRef, Subsystems, ...)
├── mount[0] (WeaponMount, LocalTransform)   <- ChildOf ship
├── mount[1] ...
```
Ship prefabs (`IsA`) define class stats + default mounts; instances override.
