# Intermittent client startup crash (open)

Status: found 2026-08-01 while checking the round-setup screen. **Not fixed
— not yet diagnosed past the bisect below.** Pre-existing; the sector
-generation increment did not introduce it, but makes it noticeably more
frequent.

## Symptom

`GravitarisNG` segfaults during startup, before the first frame. Roughly one
launch in three to one in six. The log always stops at the same place:

```
[audio] using backend: miniaudio          <- last line on a crashed run
[RmlUi]: Loaded font face 'Chakra Petch'  <- next line on a good run
```

Nothing logs between those two, so the log alone doesn't localize it further
than "somewhere in world building".

## Bisect (2026-08-01)

Each row is repeated launches of the same binary, `timeout` used to end
healthy runs (124 = survived, 139 = segfault).

| What the client built at startup | Result |
|---|---|
| Generated sector, 4 factions | 1 crash in 3 |
| `BuildClassicWorld()` (the old 2-sun, 2-faction arena) | 1 crash in 6 |
| Nothing — `BuildWorld` call removed entirely | 0 crashes in 8 |
| `gravitaris-server`, same generated sector, headless | 0 crashes in 6 |

Two things follow:

- **It is in world building**, and specifically in the **cgame half** of it:
  the headless server runs the identical `BuildWorld` on the identical
  generated sector without ever crashing. What the client does extra is
  `CEntitySpawner::AddRenderable` — `ResourceLoader::Load<Model>` plus the
  GL mesh it builds.
- **It scales with entity count**, which is why it is not new: the classic
  world crashes too, just half as often. A 4-faction sector spawns roughly
  four times the structures and up to three times the celestials, so the
  same latent fault gets more chances per launch.

## Not yet ruled out

- Uninitialized read or a lifetime bug in the model/mesh resource path.
  `ResourceLoader::Load` (`resource-loader.inl`) has non-trivial weak-pointer
  caching with a documented history of duplicate-instance bugs, and the
  renderers keep shared per-id caches keyed off resource destruction — that
  combination is where an intermittent fault is most plausible.
- The `-O2`-only dangling-swizzle class of bug CLAUDE.md warns about. A grep
  for the obvious `const Vector2& x = expr.xy()` shape over `cgame/` and
  `client/` found nothing, but the search only covers the literal spelling.
- A GL/driver interaction during resource upload. The run log shows a long
  list of NVIDIA driver workarounds active.

## How to make progress

No debugger is installed on this box — no `cdb` under the Windows Kits path,
so there is currently no way to get a stack trace. Either install the
Windows SDK debuggers and run `cdb -g -G GravitarisNG.exe` until it faults,
or add a `SetUnhandledExceptionFilter` handler to the client that writes a
minidump. A stack would almost certainly settle this in minutes; everything
above is inference from launch counts.

A Debug build was not tried — worth doing early, since the fault may simply
not reproduce there (which would itself point at the `-O2`/uninitialized
family).
