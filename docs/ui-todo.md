# UI increment — wasm input, UI outside postprocess, build/ping readout

Status: todo list written 2026-07-26; T1–T4 implemented the same day. What's
left is live verification that needs hardware/sessions this machine can't
provide — see the unticked boxes below. Task 4b (enemy arrows in multiplayer)
was already fixed before this list was written — kept below as a confirmed
entry, since it explains the `SceneView` type the other tasks encounter.

Independent tasks; no ordering constraint between them beyond T3 → T4.
Suggested order: T3+T4, then T2, then T1 last (needs a browser to verify).

## T1 — wasm mouse coordinates are misaligned

Symptom: in the browser build the `main.rml` window (title bar + LAUNCH) only
reacts when the cursor is *outside* its visible box — hover/click land at the
wrong place, as if the pointer had to travel further than it looks.

`mouseMoveEvent`/`mousePressEvent` (`src/client/gravitaris.cpp:548`, `:574`)
scale the incoming position:

```cpp
const Vector2 p = event.position() * PixelScale();
m_ui.ProcessMouseMove(static_cast<int>(p.x()), static_cast<int>(p.y()));
```

`PixelScale()` (`:109`) is `max(framebufferSize()/windowSize(), dpiScaling())`.
On wasm the canvas is CSS-sized `1920x1080` with `max-width: 100%; max-height:
100vh` (`tools/wasm/index.html`), so the CSS box and the backing store differ
by a factor that is *not* 1 — and Emscripten's SDL port already reports
positions in canvas backing pixels. Suspected double-applied scale.

- [x] Hypothesis confirmed from the two sources involved, not from a log:
      - Magnum's `Sdl2Application::windowSize()` *and* `framebufferSize()`
        both return `emscripten_get_canvas_element_size("#canvas")` — the
        canvas **backing store** size — so their ratio is always exactly 1
        and `PixelScale()` collapses to `dpiScaling()`, which on Emscripten
        is `devicePixelRatio`.
      - Emscripten's `libbrowser.js` (`calculateMouseCoords`) scales client
        coordinates by `canvas.width / rect.width` before SDL ever sees
        them, i.e. positions already arrive in backing pixels.

      So the multiply applies `devicePixelRatio` a second time — exactly the
      "cursor must overshoot" direction, by exactly that factor. Measured in
      a running browser build: `devicePixelRatio` 1, backing 1920x1080, CSS
      box 1280x720 — a `dpr` of 1 makes old and new code identical, which is
      why this only shows up on HiDPI.
- [x] Fixed by skipping the multiply under `CORRADE_TARGET_EMSCRIPTEN`
      (`GravitarisApplication::UiPointerPosition`). The `index.html` route was
      rejected: pinning the CSS box to the backing size means forcing
      `dpr` to 1, throwing away browser HiDPI crispness, and it would fight
      the `onRuntimeInitialized` resize workaround.
- [x] Cross-checked: the RmlUi context is sized to `framebufferSize()` and
      the density-independent ratio is `PixelScale()`, so all three agree —
      backing pixels natively via the multiply, backing pixels on Emscripten
      without it.
- [ ] Verify native HiDPI (Windows display scaling ≠ 100%, and macOS Retina)
      still hit-tests correctly — the multiply is there for a real reason on
      those platforms. Low risk: the change is inside `#ifdef
      CORRADE_TARGET_EMSCRIPTEN`, so native codegen is unchanged.

## T2 — UI must not be touched by the shaders

**This supersedes `docs/selective-postprocess-ui.md`.** That document explores
four ways to make glow/CRT apply to UI *chrome* but not UI *text*, all of them
non-trivial (mask buffers, draw-call classification, z-order hazards with
overlapping dialogs). The decision has changed: no postprocess on the UI at
all. None of options 1–4 are needed.

That makes this a placement change, not a technique:

- [x] Draw RmlUi to the default framebuffer *after*
      `GlowPostProcess::EndSceneAndComposite`. The default framebuffer is
      bound and its viewport set explicitly first — RmlUi issues raw GL draws
      into whatever is bound, and the no-CRT composite path ends in a
      `blit()` rather than a `bind()`.
- [x] Retired `m_uiInWorld` and its `U` key toggle outright.
- [x] Minimap live-texture bridge: unaffected. `RenderInterfaceGL3` samples
      the texture by id at draw time and `MinimapRenderer` still runs before
      the glow pass claims the scene target; only the destination framebuffer
      of the RmlUi pass changed.
- [x] UI viewport uses `framebufferSize()` — `drawEvent` passes it to
      `UI::SetDimensions`, which is what `RenderInterfaceGL3::SetViewport`
      reads.
- [x] Marked `docs/selective-postprocess-ui.md` superseded. `CLAUDE.md` and
      the header comment in `data/ui/vector.rcss` described the old placement
      too, and were updated with it.
- [ ] Eyeball it: UI text should now be crisp with no bloom/scanlines over
      it, and the minimap should still draw. Not checked here — screenshotting
      meant capturing the desktop of a machine in active use.

Side effect worth knowing: the glow scene FBO has no stencil attachment, so
RmlUi's stencil-based clip masks were silently inert while the UI rendered
into it. The default framebuffer has one, so they now actually work.

Interaction with T1: this moves where the UI is drawn, but not the coordinate
space it's laid out in, so it should not change hit-testing. If T2 lands
first and the wasm misalignment changes character, re-measure in T1. (It
didn't — T1's cause turned out to be entirely in the input path.)

## T3 — generated build-info header

- [x] `cmake/build-config.hpp.in` → `${CMAKE_BINARY_DIR}/generated/gravitaris/
      build-config.hpp`, that directory on `GravitarisNG`'s include path.
      Carries platform (`windows`/`macos`/`linux`/`wasm`), build type, git
      short hash, dirty flag, timestamp.
- [x] Timestamp is a real **build** time, not a configure time: the
      `gravitaris-build-info` custom target re-runs `cmake/build-info.cmake`
      via `cmake -P` on every build. `configure_file` leaves the output
      untouched when nothing differs, so a rebuild inside the same minute
      costs no recompile; a rebuild across one recompiles a single TU. That
      trade-off, and the semantics, are stated in the generated header.
- [x] `BuildInfoString()` (`include/gravitaris/game/build-info.hpp`) formats
      it once. The generated header is an implementation detail of that one
      TU; nothing else includes it.

## T4 — bottom-right build + ping readout

- [x] `div#status_readout` in `data/ui/hud.rml`, fed by
      `UI::SetHudStatusText`.
- [x] Ping source is already measured — no netcode work needed:
      `NetClient::GetAveragePingMs()` is the smoothed EMA and the right
      default (`GetLastPingMs()` is a noisier single sample;
      `GetPingJitterMs()` is available if a ± reading is wanted). **Both
      return `-1` until the first Pong arrives**, so render a placeholder
      rather than `-1 ms`. See
      `include/gravitaris/game/net/net-client.hpp:296-303`. Renders as
      `ping --` until the first Pong.
- [x] Sits at `bottom: 196dp; right: 16dp` — clear of the minimap's 16dp
      margin + 170dp box + 2dp borders.
- [x] Inside the `pointer-events: none` body, and `pointer-events: none`
      itself.
- [x] Refreshed from `tickEvent` at 4 Hz (`RefreshHudReadout`), and
      `SetHudStatusText` drops repeat text, so `SetInnerRML` only runs on a
      real change. No data model introduced.
- [x] `UI` takes a formatted string; the client wiring reads `NetClient`
      (via `CGame::GetAveragePingMs`). Nothing net-aware on the RML side.

## T4b (done) — enemy arrows in multiplayer

Confirmed fixed 2026-07-26, in the working tree at the time this list was
written. Recorded because the fix introduced a type the tasks above will run
into.

The bug: `IndicatorRenderer` swept the sim registry for
`Transform + Team + Damageable`, but in multiplayer the registry holds nothing
but your own predicted ship — every other entity lives in `m_mirrorWorld`. It
also submitted overlays into `m_modelRenderer2`, which isn't the renderer
drawing that frame in multiplayer.

The fix is structural, not a patch: `include/gravitaris/cgame/scene-view.hpp`
introduces `SceneView { local, remote, overlays }` with an `Each()` that
sweeps both worlds, plus `CGame::CurrentSceneView()`. Consumers take a
`SceneView` rather than an optional `flecs::world*`, so the multiplayer half
can no longer be silently forgotten per call site — which is exactly how the
arrows shipped broken. `CameraDirector`, `MinimapRenderer` and
`SubmitPlanetOwnershipMarkers` moved to it too.

- [x] Sweep both worlds; submit overlays to the frame's actual renderer.
- [ ] Still worth an in-game confirmation in a live multiplayer session
      (arrows appear, point the right way, and fade at the screen edge).
