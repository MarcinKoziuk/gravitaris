# Gravitaris

Retro vector-display arcade game: gravity-well combat and orbital navigation.

Design vision lives in `IDEAS.md`. Reference notes and architecture decision
records live in `docs/` — consult these before designing gameplay/netcode/ECS
work.

## Workflow

- **Work directly on `main`.** Only branch for special/obvious cases (e.g. a
  large or risky rewrite) or if explicitly asked to.
- **Build into `out/`** (gitignored). Use subdirectories for different build types.
- Commit only when asked. Don't commit build directories or scratch files.

## Stack
- CMake + Ninja
- C++17 (I wanna upgrade to C++23)
- Magnum (for OpenGL)
- ECS: entt (but wanna move to flecs)
- Chipmunk2D (physics)
- PhysFS (vfs)
- RmlUI (game ui)
- nanosvg (svg parsing)
- TODO: Dear ImGui for dev/debug UI
- Config parsing: yaml-cpp, but we need to switch to something better

## Architecture
Eventually the game should be multiplayer, so the game is split into separate modules for client and server (like quake3).

## Game assets
- Ships/bodies are authored as SVG + YAML, parsed via nanosvg and converted
  to Chipmunk shapes (see `src/game/resource/`).

## Known gotchas

- **macOS DPI**: `Sdl2Application::dpiScaling()` reports `{1, 1}` on macOS
  even on Retina displays where `windowSize()` and `framebufferSize()`
  genuinely differ. Use `framebufferSize() / windowSize()` for the real
  backing-store scale (see `GravitarisApplication::PixelScale()` in
  `src/client/gravitaris.cpp`) — don't reach for `dpiScaling()` there.
- The post-process pipeline (bloom + CRT scanlines, `GlowPostProcess`) sizes
  its offscreen targets to `framebufferSize()`, so on HiDPI/Retina displays
  it runs at up to 4x the pixel count of an equivalent 1x display — a likely
  cost driver if frame time regresses on Mac vs. a 1x Windows box on the same
  physical screen.
- **Magnum swizzles return references**: non-const `Vector4::xy()` returns a
  `Vector2&` aliasing the vector's own storage. Binding a reference to a
  swizzle of a temporary dangles — `const Vector2d& p = (m * v).xy();` works
  in Debug but reads garbage at `-O2`. Always take swizzle results by value.
  (This class of UB hides in Debug builds; test RelWithDebInfo regularly.)

## Rendering

- Vector/CRT aesthetic: phosphor glow (multi-pass blur + additive composite)
  and CRT scanlines
- UI (RmlUi) is drawn into the same offscreen scene target before compositing
  when `m_uiInWorld` is true, so it picks up bloom/CRT like the game world.

## Code style
- Keep consistent with current style 
- Do not indent top-level Gravitaris namespaces
- use `m_` prefix for members *except* for plain structs
- i like `PascalCase` for functions
  - exception is if we inherit a class using a different convention,
    then adhere to that convention
  - static or free helper methods should still use our own convention
- `snake-case.cpp` for filenames
- `.hpp` extension for headers
- shared API headers between multiple modules (game, cgame, etc.) go into `include/`, 
  other headers that don't need to exposed, can be put into the same folder as the cpp file
- vendored source files go in `extlibs/`
- includes order:
```cpp
// Standard library imports first
#include <cstring>

// Then windows/posix if needed
#include <windows.h>

// Then magnum
#include <Magnum/Math/Vector3.h>
#include <Magnum/Math/Matrix4.h>

// Then other libs
#include <nanosvg/nanosvg.h>

// Then Gravitaris API headers
#include <gravitaris/game/logging.hpp>
#include <gravitaris/cgame/resource/shape.hpp>

// Then module-local headers
#include "game/resource/detail/shape-common.hpp"
#include "game/resource/detail/casteljau.hpp"

// Then the header corresponding to this cpp file (if applicable)
#include "example.hpp"
```
- please don't pollute files with `windows.h` or other intrusive headers,
  e.g. by encapsulating that code
- try to keep number of source files in one folder not too large; split them up and feel free to use e.g. `detail/`
  folder for implementation details (the current state is how I mostly like it) 
- instead of `out` pointer parameters, prefer to use move constructors if possible,
  unless doing that is more complex
- misc:
```c++
if {
    stmt;
}
else { // Use stroustrup if/else style
    then;
}
```

### Comments
- Comments only if really necessary/a good idea. No "obvious" comments
- Don't write too long comments or comments as explanation to me as result of a AI coding session
```c++
    // BAD EXAMPLE

    // Use the real framebuffer size (pixels), not windowSize() (logical
    // points). On HiDPI/Retina displays the two differ by the DPI scale
    // factor; sizing viewports/offscreen targets and RmlUi's canvas off the
    // smaller logical size left the actual framebuffer mostly uninitialized
    // (visible as a small rendered region in one corner and black
    // elsewhere), and the postprocess pass sampling across that boundary is
    // what showed up as glitching once glow was enabled.
    const Magnum::Vector2i fbSize = framebufferSize();

    // GOOD EXAMPLE
    // framebufferSize() is required on HiDPI displays; windowSize() causes
    // incorrect viewport sizing and postprocess artifacts.
    const Magnum::Vector2i fbSize = framebufferSize();
```
