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

### Phase 2 — Data + boot — done (2026-07-18)

GL version/context flags already landed in Phase 1. This phase is data
delivery + actually loading the page in a browser, which is where the real
signal is — a clean link (Phase 1) only proves the compiler/linker are happy.

**Done:**
- Link flags: `-sALLOW_MEMORY_GROWTH=1`, `-sSTACK_SIZE=1048576` (Emscripten's
  64KB default stack overflowed silently — flecs/RmlUi/template call chains
  need more), `--preload-file=<repo>/data@/data`.
- `filesystem-phys-fs.cpp`: added a `PHYSFS_mount("/data", ...)` candidate (the
  preload path above populates Emscripten's MEMFS there before `main()` runs;
  PhysFS's POSIX driver reads through libc, so this is an ordinary mount as far
  as PhysFS is concerned).
- **Two real `PHYSFS_init()` bugs found by instrumenting PhysFS's own source
  directly** (temporarily, in the gitignored `out-wasm/_deps` checkout — not a
  guess-and-check loop):
  1. Its user-dir lookup (`getenv("HOME")`, falling back to a passwd/UID
     lookup) — Emscripten leaves `HOME` unset and has no real passwd database,
     so both fail. Fixed: `setenv("HOME", "/", 1)` before `PHYSFS_init()`
     under `__EMSCRIPTEN__` (`/` always exists in MEMFS).
  2. Its *base*-dir lookup (`/proc/self/exe` symlink, then an argv0 search
     through `$PATH`) — neither exists under Emscripten, so with a bare
     program-name argv0 (`GRAVITARIS_NAME`, no `/` in it) it fails outright.
     The failure surfaced as `PHYSFS_getLastErrorCode()` reporting "no error"
     — a separate PhysFS quirk (its error state is a `pthread_self()`-keyed
     per-thread lookup, which doesn't roundtrip correctly in a non-`-pthread`
     Emscripten build) that made the *real* failure look like the user-dir one
     above at first; only confirmed by temporarily adding `fprintf(stderr, ...)`
     at each of `PHYSFS_init`'s internal `goto initFailed` sites. Fixed:
     passing `"/" GRAVITARIS_NAME` as argv0 under `__EMSCRIPTEN__` instead —
     PhysFS's own documented fallback (chop at the last `/`) then trivially
     succeeds. **Both fixes are `#if defined(__EMSCRIPTEN__)`, native
     unaffected** (reconfirmed: native rebuild + smoke run green).
- **GLSL ES requires an explicit default float precision in fragment shaders**
  (`precision highp float;`) — desktop GLSL has no such requirement. All 7
  actively-loaded fragment shaders were missing it (`line2.f.glsl`,
  `starfield.f.glsl`, `postprocess/{crt,glow-blur,glow-composite,
  glow-threshold}.f.glsl`, `simple/lines.f.glsl`); the directive is a no-op on
  desktop GLSL, so added unconditionally rather than platform-guarded.
- `tools/wasm/index.html`: canvas with the hardcoded `id="canvas"` Magnum's
  Sdl2Application requires on Emscripten (`emscripten_get_element_css_size`
  keys off it); CSS size doubles as the initial canvas size query, so no
  separate C++-side "size to canvas" change was needed. `build.sh` copies it
  next to the built artifacts.
- **Verified live in a browser** (this machine's Chromium-based preview,
  WebGL2 confirmed: `OpenGL ES 3.0 (WebGL 2.0 (OpenGL ES 3.0 Chromium))`):
  boot reaches PhysFS mount (90 files enumerated) → miniaudio backend →
  Chipmunk physics space → RmlUi font loading → the sim loop actually
  running (a "sim step cap hit" warning proves ticks are executing, just
  slow — expected for an unoptimized `Debug` wasm build).

**The `GL_INVALID_OPERATION` root cause (found, not RmlUi's fault):**
`GlowPostProcess::EndSceneAndComposite` blits its single-sampled composited
result into the default framebuffer every frame
(`AbstractFramebuffer::blit`). `CreateGLConfiguration` requested 4x MSAA on
that same default framebuffer (`conf.setSampleCount(4)`). GLES3/WebGL2
forbids blitting into a multisampled destination outright — desktop GL
drivers are just more lenient about the identical spec violation, which is
why this never surfaced natively. The browser's own console eventually gave
the real error verbatim once the user tested it independently:
`glBlitFramebuffer: Invalid operation on multisampled framebuffer` — RmlUi's
`CompileGeometry`/`RenderGeometry` `GL_INVALID_OPERATION` reports upstream
were a symptom (the corrupted/errored GL state from the earlier failed blit
each frame), not their own bug. Fixed: skip `setSampleCount(4)` under
`CORRADE_TARGET_EMSCRIPTEN` (lines already get analytic edge AA from their
own shader, so the visual cost is minimal). Native keeps 4x MSAA, unaffected.

**Confirmed live in a browser: the game fully renders** — starfield, player
ship, RmlUi HUD panel with a "LAUNCH" button, minimap with planets. First
successful end-to-end render of this port.

**`--release`/`--relwithdebinfo` builds now work** (`tools/wasm/build.sh
--release` / `--relwithdebinfo`) — needed one real, general fix:
`include/gravitaris/game/id.hpp` used `std::size_t` without including
`<cstddef>`; it happened to compile under Debug only because `<string>` (guarded
by `#ifndef NDEBUG`, pulled in for the debug ID-collision checker) transitively
supplied it. Under any `NDEBUG` build (Release, RelWithDebInfo — native *or*
wasm) that include disappears and the missing direct one is exposed. This was
a latent bug independent of the wasm port; fixed by including `<cstddef>`
unconditionally.

**Open, unresolved bug (2026-07-19): the game renders black (UI still shows)
specifically in Chrome, both `--release` and `--relwithdebinfo`, even from a
fully clean rebuild** — confirmed by the user on their machine. Notably:
- **Safari renders correctly** with the same build.
- **Native `RelWithDebInfo` (no sanitizers) renders correctly.**
- Native `RelWithDebInfo` + UBSan (`-fsanitize=undefined`) ran clean for 6s at
  startup — no sanitizer reports, so if this is C++ UB, it's not a class UBSan
  catches, or it only triggers in wasm codegen specifically.
- On the assistant's own machine, a clean `--relwithdebinfo` wasm build
  rendered fine in the (Chromium-based) preview browser, but a clean
  `--release` (`-O3 -flto`) build reproduced the same black-scene symptom —
  suggesting `-O3`/LTO specifically, *and/or* something Chrome/ANGLE-specific
  (Chrome's GL calls go through ANGLE's translation-to-Metal layer on macOS;
  Safari's WebKit WebGL implementation is native, no ANGLE) given the earlier
  MSAA/blit bug was exactly this shape: a spec violation only some
  implementations enforce strictly. Both explanations are plausible and not
  mutually exclusive; **not disambiguated yet**.
- Next step whoever picks this up: test `--relwithdebinfo` (not `--release`)
  specifically in Chrome on the user's machine to separate the `-O3`/LTO axis
  from the Chrome/ANGLE axis (native RelWithDebInfo+Safari/Chrome-in-wasm
  hasn't been cross-tested either). If `--relwithdebinfo` also reproduces in
  Chrome, it's not LTO-specific and the Chrome/ANGLE angle is the one to chase
  — likely another stricter-spec-validation case like the MSAA blit fix above;
  bracket the game's own render calls (`CGame::Render`,
  `GlowPostProcess`, `StarfieldRenderer`, `ModelRenderer2`) with `glGetError()`
  checks the way `RenderInterfaceGL3::CheckGLError` already does for RmlUi.

**Not done / next** (Phase 3 territory):
- Resolve the Chrome-black-scene bug above.
- Canvas/window resize handling (the canvas backing size was observed growing
  across repeated resizes — not yet investigated). A JS-side workaround
  (forcing a genuine 1px canvas resize right after boot, since Magnum only
  recomputes canvas size/DPI on an actual resize event) is in `index.html`
  but is unverified against the *original* aspect-ratio complaint, since it's
  since been superseded by the Chrome black-scene issue in every test.
- Verify audio actually plays, keyboard/mouse/scroll input works, and the sim
  runs at a reasonable framerate in `--release` (blocked on the above).
- **Done when** (met): the page loads to the first rendered frame of the game.
  (Only reached so far in a Debug build and in Safari for Release-class
  builds — Chrome + Release/RelWithDebInfo is the open gap.)

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
