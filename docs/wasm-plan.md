# WebAssembly (browser) port — preparation & execution plan

Goal: build GravitarisNG as WebAssembly + WebGL2 via Emscripten so it runs in a
browser, alongside the existing native builds (no regression to desktop).

Rules for executing this plan (same convention as `docs/networking-plan.md`):

- **One phase per session/PR.** Each phase ends with a "Done when" gate; stop
  there and let the user try it before the next.
- Never start phase N+1 with phase N's gate unmet.
- Keep desktop builds green at every phase — all web-only changes go behind
  `if(EMSCRIPTEN)` / `#ifdef CORRADE_TARGET_EMSCRIPTEN`.

## Why this is feasible (current-state audit, 2026-07-18)

The engine is well-positioned for a web port:

- **No threads anywhere** (`grep std::thread|pthread` = empty). Avoids
  SharedArrayBuffer, COOP/COEP headers, and pthread-in-wasm entirely.
- **Shaders already have GLES branches** — every shader in
  `src/cgame/renderer/shader/*.cpp` selects `GLES300` under
  `MAGNUM_TARGET_GLES`, which is WebGL2. The GLSL uses ES-friendly
  `in`/`out`/`highp`/`lowp`, `fwidth`, `gl_FragCoord` — all valid in GLSL ES 3.00.
- **Corrade / Magnum / magnum-integration are first-class Emscripten targets**
  (mosra ships an Emscripten toolchain and maintains WebGL builds).
- **Single WebGL2-compatible render path** — the glow post-process FBOs
  (`glow-post-process.cpp`) are plain single-sample color textures with linear
  downscale blits, all legal in WebGL2.
- `if(EMSCRIPTEN) set(CORRADE_TARGET_EMSCRIPTEN TRUE)` is already stubbed at
  the top of `CMakeLists.txt`.

## Dependency status

Fetched deps that are portable C/C++ and build for wasm unchanged: Chipmunk2D,
physfs, flecs, yaml-cpp, RmlUi, poly2tri, ImGui, Corrade, Magnum,
magnum-integration.

The three that need substitution:

| Dep         | Native            | Web                                              |
|-------------|-------------------|--------------------------------------------------|
| SDL2        | FetchContent build| Emscripten's built-in port (`-sUSE_SDL=2`)       |
| freetype    | FetchContent build| Emscripten port (`-sUSE_FREETYPE=1`) or source   |
| OpenAL-soft | FetchContent build| **removed entirely — see Phase 0**               |

## Decisions

- **Application class**: keep Magnum `Sdl2Application` + `MAGNUM_APPLICATION_MAIN`.
  It supports the Emscripten SDL port, so all existing
  `keyPressEvent`/`scrollEvent`/`drawEvent` handlers work unchanged. (Switching
  to `EmscriptenApplication` is a possible later cleanup, not required.)

- **Audio**: **remove the OpenAL-soft backend on all platforms**, keep miniaudio
  as the sole backend (it has a Web Audio backend). Rationale: OpenAL has been
  the source of audio pops / first-chance issues in this project, it's only a
  debug-tab A/B toggle today (default is already miniaudio), and it does not
  build for web. This is Phase 0 below.

- **Exceptions**: aim to build with C++ exceptions **disabled** (smaller/faster
  wasm). The only `try/catch` in our code is around poly2tri in
  `model-renderer2.cpp` (polygon fill triangulation). Note that *vendoring
  poly2tri does not by itself remove its exceptions* — poly2tri throws
  internally on degenerate polygons, so with exceptions off that path would
  `abort()`. Approach: guard fill inputs (skip <3-point / duplicate / obviously
  degenerate polygons before calling `Triangulate`) so the throw path isn't
  reached; keep `-fexceptions` as a fallback only if this proves flaky. Fills
  are cosmetic, so worst case is a missing fill, never a crash. Non-blocking for
  Phases 0–1.

- **Data delivery**: `--preload-file data@/data`; add a `/data` physfs mount for
  the Emscripten virtual FS (host-path mounts stay for native).

## Phases

### Phase 0 — Remove OpenAL (all platforms)

Prep that also unblocks web audio. Desktop stays on miniaudio (already default).

- Delete the OpenAL-soft `FetchContent` block and its link/`MAGNUM_WITH_AUDIO`
  wiring in `CMakeLists.txt` (verify `MAGNUM_WITH_AUDIO` isn't needed once the
  Magnum OpenAL backend is gone).
- Remove `magnum-openal-backend.{hpp,cpp}` and the `AudioBackendPreference::
  PreferOpenAL` path; simplify `audio-backend-factory.cpp` to always build the
  miniaudio backend.
- Drop the OpenAL A/B toggle from the Audio debug tab.
- **Done when**: native build is green and audio still plays (miniaudio), with
  no OpenAL references left (`grep -ri openal src include CMakeLists.txt` clean).

### Phase 1 — Emscripten toolchain + configure

Get `emcmake cmake` to configure the project for wasm (no full link yet).

- Prereq: developer installs emsdk (`git clone emscripten-core/emsdk`,
  `./emsdk install/activate latest`, `source emsdk_env.sh`). Not vendored.
- Add `if(EMSCRIPTEN)` gating so configuration succeeds: skip the SDL2
  FetchContent (use `-sUSE_SDL=2`), skip freetype FetchContent (use the port),
  set Magnum's WebGL/GLES target options.
- Add a `tools/wasm/build.sh` wrapper (runs `emcmake cmake` + `cmake --build`)
  and an `index.html` shell.
- **Done when**: `tools/wasm/build.sh` configures without CMake errors.

### Phase 2 — Link + boot

- Executable link flags: `-sMAX_WEBGL_VERSION=2 -sMIN_WEBGL_VERSION=2`,
  `-sFULL_ES3=1`, `-sALLOW_MEMORY_GROWTH=1`, `--preload-file data@/data`,
  miniaudio's web audio.
- Runtime: add the `/data` physfs mount (`filesystem-phys-fs.cpp`); size the
  view to the HTML canvas instead of the hardcoded `1920x1080`
  (`gravitaris.cpp` `CreateConfiguration`).
- **Done when**: the page loads to the first rendered frame of the game.

### Phase 3 — Polish

- Verify audio, keyboard/mouse/scroll, HiDPI/canvas resize.
- Simple served `index.html` (canvas + loading indicator).
- Input replay (`InputLog::Save/Load`) won't persist on MEMFS — leave in-session
  or mount IDBFS later (not a blocker).
- **Done when**: playable in a browser at a stable framerate.

## Known risks / gotchas

- **RmlUi + freetype under Emscripten** is the least-travelled path (the UI
  stack). Fallback if it fights back: gate the RmlUi UI off on web initially and
  ship the game view + ImGui debug UI first.
- **MSAA**: `setSampleCount(4)` on the default framebuffer — keep (WebGL2
  `antialias:true`) or drop; the scene renders into the single-sample glow FBO
  anyway, so it barely matters.
- The Windows `__try/__except` in `gl-safe-upload.hpp` is `#if _WIN32` only, so
  wasm uses the plain `setData` path — no action.
- No emsdk in the current dev environment as of writing, so build steps below
  are authored to mosra's documented Emscripten conventions but must be
  compile-verified once emsdk is installed.
