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

### Phase 1 — Emscripten toolchain + configure — done (2026-07-18)

Original gate was "configures without CMake errors"; ended up reaching a full
clean link (`GravitarisNG.js` + `.wasm`) by iterating on real `emcmake`/`em++`
output rather than guessing, so this phase absorbed some of what was expected
to land in Phase 2. Actual work, in the order hit:

- Prereq: emsdk installed at `~/Projects/emsdk` (`git clone emscripten-core/
  emsdk`, `./emsdk install/activate latest`). Fish-shell users: skip
  `source emsdk_env.sh` (bash-only) — `tools/wasm/build.sh` (below) finds and
  `PATH`-exports `emcc`/`emcmake` itself, no shell activation needed.
- **SDL2**: skip the FetchContent entirely under `EMSCRIPTEN` (real SDL2
  upstream has no Emscripten backend to build from source against). Magnum's
  own bundled `FindSDL2.cmake` already has a dedicated Emscripten branch (skips
  linking a library, finds `SDL_scancode.h` under an `SDL` path suffix, adds
  `-sUSE_SDL=2` itself) — the only requirement is to NOT pre-create an
  `SDL2::SDL2` target ourselves, which would make that module take its
  "already have a target" branch instead and fail on a missing
  `INTERFACE_INCLUDE_DIRECTORIES` it never expects us to set.
- **Freetype**: needed no special-casing at all — it's portable C and just
  cross-compiles via `em++` like it does natively. (Original plan assumed the
  Emscripten port would be needed; wrong guess, corrected once tested.)
- **`CORRADE_TARGET_UNIX` vs `CORRADE_TARGET_EMSCRIPTEN` bug**: the existing
  `if(WIN32)...elseif(APPLE)...elseif(UNIX)` chain set `CORRADE_TARGET_UNIX`
  even when cross-compiling to Emscripten, because Emscripten's own CMake
  toolchain sets CMake's built-in `UNIX` to `1` (POSIX-emulation, unrelated to
  Corrade's meaning of "real desktop Unix"). With both `CORRADE_TARGET_UNIX`
  and `CORRADE_TARGET_EMSCRIPTEN` true, Magnum's GL-context-selection cascade
  built an erroneous GLX (X11) context alongside the correct Emscripten one,
  and GLX doesn't compile under `em++` (no real X11 headers) — `#error
  unsupported platform`. Fixed by checking `EMSCRIPTEN` first in that chain,
  mutually exclusive with the rest.
- **Chipmunk2D's `cpHastySpace.c`** (the optional threaded solver — unused;
  nothing in this codebase touches Chipmunk's threading API) unconditionally
  `#include <sys/sysctl.h>` on any non-Windows platform, true on macOS/BSD but
  not Linux or Emscripten, and Chipmunk's own CMakeLists globs `src/*.c` with
  no option to exclude it. Fixed by stripping that one file from the
  already-created `chipmunk_static` target's `SOURCES` under `EMSCRIPTEN`.
- **RmlUi/poly2tri shared-library default**: RmlUi's `BUILD_SHARED_LIBS` option
  defaults `ON`, and poly2tri's untyped `add_library()` inherits the same
  ambient variable — harmless natively (plain `.dylib`/`.so` + the existing
  `BUILD_RPATH` fixup), but Emscripten's "shared library" is a side module
  requiring `-fPIC`/`-sMAIN_MODULE`, and linking failed with `relocation ...
  cannot be used against symbol ...; recompile with -fPIC`. Fixed by forcing
  `BUILD_SHARED_LIBS OFF` under `EMSCRIPTEN` before the RmlUi fetch.
- **GLES2 vs GLES3/WebGL2**: Magnum defaults an Emscripten build to
  `MAGNUM_TARGET_GLES2 ON` (widest device reach), but our shaders explicitly
  request `GLES300` and the code uses GLES3/WebGL2-only GL features
  (`TextureFormat::RGBA8`, `Framebuffer::clearColor`/`AbstractFramebuffer::
  blit`, core (non-OES) VAOs) — all missing/uncompilable under GLES2. Fixed by
  forcing `MAGNUM_TARGET_GLES2`/`TARGET_GLES2 OFF` under `EMSCRIPTEN`, **plus**
  passing `-sFULL_ES3=1` at COMPILE time too (separate from Magnum's own C++
  option — it's what gates GLES3 declarations like `glGenVertexArrays`/
  `GL_RGBA8` being visible in Emscripten's GL headers at all), and
  `-sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2` at link time (pins the actual
  browser context `Sdl2Application` requests — without it Emscripten still
  negotiates WebGL1 by default even once the code compiles against ES3 headers).
- Added `tools/wasm/build.sh` (locates emsdk, `emcmake cmake` + `cmake --build`,
  works from any shell) and `.gitignore`d `out-wasm/`.
- **NOT done yet** (deliberately left for Phase 2, since none of it was needed
  to reach a link): `--preload-file`/physfs `/data` mount, canvas sizing,
  `index.html` shell, and — critically — **never actually run in a browser**.
  A clean link only proves the code compiles and the linker's happy; it says
  nothing about whether the game boots, finds its data, or renders anything.
- **Done when** (met): `tools/wasm/build.sh` builds `GravitarisNG.js`/`.wasm`
  with no CMake or compiler/linker errors.

### Phase 2 — Data + boot

GL version/context flags already landed in Phase 1 (see above) — this phase is
now just data delivery and getting an actual browser tab to show something.

- Executable link flags still needed: `-sALLOW_MEMORY_GROWTH=1`,
  `--preload-file data@/data`.
- Runtime: add the `/data` physfs mount (`filesystem-phys-fs.cpp`); size the
  view to the HTML canvas instead of the hardcoded `1920x1080`
  (`gravitaris.cpp` `CreateConfiguration`).
- Write `index.html` (canvas + script tag for `GravitarisNG.js`).
- Actually load the page in a browser and see what happens — first real
  browser-runtime signal of this whole effort.
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
