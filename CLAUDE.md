# Gravitaris

Retro vector-display arcade game: gravity-well combat and orbital navigation.

## Workflow

- **Work directly on `main`.** Only branch for special/obvious cases (e.g. a
  large or risky rewrite) or if explicitly asked to.
- **Build into `out/`** (gitignored). Don't create other top-level build
  directories (`build-*/`, etc.) — if a different build type is needed,
  configure it as a second config under `out/` rather than a sibling folder.
- Commit only when asked. Don't commit build directories or scratch files.

## Stack

- C++17, CMake + Ninja, Magnum (OpenGL rendering), entt (ECS), Chipmunk2D
  (physics), RmlUi (UI), nanosvg (asset import), yaml-cpp.
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

## Rendering

- Vector/CRT aesthetic: phosphor glow (multi-pass blur + additive composite)
  and CRT scanlines live in `GlowPostProcess`
  (`src/cgame/renderer/glow-post-process.{hpp,cpp}`), both toggleable at
  runtime (`B` = bloom, `V` = CRT scanlines, see `keyPressEvent`).
- UI (RmlUi) is drawn into the same offscreen scene target before compositing
  when `m_uiInWorld` is true, so it picks up bloom/CRT like the game world.
