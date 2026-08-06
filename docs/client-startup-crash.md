# Intermittent client startup crash (fixed)

Status: found 2026-08-01 while checking the round-setup screen, **fixed
2026-08-06**. Root cause and fix are at the bottom; the investigation below is
kept because parts of it pointed the wrong way and it is worth knowing which.

## Root cause

`SimpleModelRenderer::HandleModelAdded` asked the driver to read eight times
the vertex buffer it was given:

```cpp
buf.setData(Containers::ArrayView<const void>{
    vertexBuffer.data(),                            // const Vector2*
    vertexBuffer.size() * sizeof(vertexBuffer[0])   // already bytes
});
```

`ArrayView<const void>` has two constructors that both match here, and the
typed one wins:

- `(const void* data, size_t size)` — `size` is in bytes.
- `template<class T> (const T* data, size_t size)` — *"Size is recalculated to
  size in bytes"*, i.e. `_size = size * sizeof(T)`.

A typed pointer therefore selects the second and multiplies a byte count by
`sizeof(Vector2)` again. For the refit schematic that meant telling
`glBufferData` to copy 28160 bytes out of a 3520-byte heap block. The read
runs off the end of the allocation every single time; it only *faults* when
the 28 KB it walks happens to cross an unmapped page, which is what made it
look random and what made it scale with how many models a launch loaded.

The fix is `Containers::arrayView(pointer, count)`, whose size is an element
count and which converts to the void view with one multiplication.

Verified: 0 crashes in 30 launches after, against 4 in 11 immediately before.

`SafeUpload`'s "glBufferData occasionally raises a first-chance SEH exception
in the NVIDIA driver, root cause unknown" was this same bug seen from the
other renderer. Nothing is known to need that SEH swallow any more.

## What actually found it

A `SetUnhandledExceptionFilter` handler that symbolizes the faulting stack
with dbghelp (`src/client/crash-handler.cpp`), which is what the "how to make
progress" section below asked for. It named the call in one crashing run.
AddressSanitizer, tried first, was useless here: the over-read happens inside
the graphics driver, which ASan cannot instrument, and 10 ASan launches were
all clean.

## Where the old bisect misled

"It needs structures" and "it scales with entity count" were both true and
both beside the point -- structures simply meant more models loaded, and every
model load was a chance for the over-read to land badly. The bisect's
conclusion that the fault was in world building was wrong: the crash caught in
the end was in `CGame`'s own constructor, loading the refit schematic, before
`BuildWorld` ran at all. Removing `BuildWorld` cut the number of model loads,
not the bug.

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
| Generated sector, 4 factions | 1 crash in 3, later 2 in 10 |
| `BuildClassicWorld()` (the old 2-sun, 2-faction arena) | 1 crash in 6 |
| Nothing — `BuildWorld` call removed entirely | 0 crashes in 8 |
| `gravitaris-server`, same generated sector, headless | 0 crashes in 6 |
| Generated sector, **starting complexes skipped** | **0 crashes in 10** |
| Generated sector, planetside structures only (no High Port) | 2 crashes in 10 |
| Debug build, generated sector, 4 factions | 1 crash in 8 |

Three things follow:

- **It needs structures.** Celestials alone never crash, however many of
  them; adding the starting complexes brings it straight back. Both
  structure paths are implicated — dropping only the orbital High Port and
  keeping the planetside four still crashes at the full rate.
- **It is not an optimizer artifact.** It reproduces in a Debug build, which
  rules out the `-O2` dangling-swizzle family CLAUDE.md warns about.

- **It is in world building**, and specifically in the **cgame half** of it:
  the headless server runs the identical `BuildWorld` on the identical
  generated sector without ever crashing. What the client does extra is
  `CEntitySpawner::AddRenderable` — `ResourceLoader::Load<Model>` plus the
  GL mesh it builds.
- **It scales with entity count**, which is why it is not new: the classic
  world crashes too, just half as often. A 4-faction sector spawns roughly
  four times the structures and up to three times the celestials, so the
  same latent fault gets more chances per launch.

## Checked and cleared

- `EntitySpawner::SpawnStructure` / `SpawnOrbitingStructure`: read for the
  flecs "reference into table storage held across an entity creation"
  hazard. `SpawnOrbitingStructure` does hold a `const Transform&` to the
  planet, but only uses it before `SpawnStructureBase` runs; everything else
  is copied by value. Clean as far as that goes.
- `AudioSystem`: has no spawn observer, so it isn't reacting to structures
  being created off its own thread.

## Not yet ruled out

- **`StructureAttachmentSystem`, not the spawn itself.** Worth testing first
  next time: with no complexes there are also no attachments, so
  `SettleScenario`'s attachment pass is empty — every result above is
  equally consistent with the fault being there rather than in spawning. It
  looks up each planet by NetId and takes `planet.get<Transform>()`; the
  `is_alive()` guard covers a dead planet but not an alive one missing the
  component.
- Uninitialized read or a lifetime bug in the model/mesh resource path.
  `ResourceLoader::Load` (`resource-loader.inl`) has non-trivial weak-pointer
  caching with a documented history of duplicate-instance bugs, and the
  renderers keep shared per-id caches keyed off resource destruction — that
  combination is where an intermittent fault is most plausible. Note each
  structure model is loaded once per faction, so 4 factions means four
  Load calls per id where the classic world made two.
- A GL/driver interaction during resource upload. The run log shows a long
  list of NVIDIA driver workarounds active.

## How to make progress

No debugger is installed on this box — no `cdb` under the Windows Kits path,
so there is currently no way to get a stack trace. Either install the
Windows SDK debuggers and run `cdb -g -G GravitarisNG.exe` until it faults,
or add a `SetUnhandledExceptionFilter` handler to the client that writes a
minidump. A stack would almost certainly settle this in minutes; everything
above is inference from launch counts.

The Debug build reproduces it, so debugging does not have to fight an
optimized build.
