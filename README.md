# GravitarisNG

Retro vector-display arcade game: gravity-well combat and orbital navigation,
inspired by *Gravity Well* (1997, Software Engineering, Inc.).

## Stack

- CMake + Ninja, modern C++ (upgrading to C++23 planned)
- [Magnum](https://magnum.graphics/) (OpenGL rendering)
- ECS: [entt](https://github.com/skypjack/entt) (migrating to [flecs](https://github.com/SanderMertens/flecs))
- [Chipmunk2D](https://chipmunk-physics.net/) (physics)
- [PhysFS](https://icculus.org/physfs/) (virtual filesystem)
- [RmlUi](https://github.com/mikke89/RmlUi) (game UI)
- [nanosvg](https://github.com/memononen/nanosvg) (SVG parsing for ship/body assets)

Ships/bodies are authored as SVG + YAML and converted to Chipmunk shapes at
load time (see `src/game/resource/`).

The game is architecturally split into client and server modules (quake3-style)
in anticipation of multiplayer.

## Building

Built with CMake + Ninja. Aiming to be as portable/cross-platform as possible.
Currently tested on Windows and MacOS (apple silicon).

## Documentation

- [`IDEAS.md`](IDEAS.md): design vision; gameplay modes, ships, weapons,
  shields/armour, upgrades, and long-term direction.
- [`docs/`](docs/): temporary reference notes and architecture decision records from conversation wtih Claude
  - [`docs/gravity-well-1997.md`](docs/gravity-well-1997.md) — notes on the
    inspiration game's documented mechanics.
  - [`docs/slice-components.md`](docs/slice-components.md) — component &
    system inventory for the first vertical slice.
  - [`docs/flecs-migration.md`](docs/flecs-migration.md) — entt → flecs
    migration recipe and rationale.
  - [`docs/adr/`](docs/adr) — architecture decision records (e.g.
    [`0001-netcode-model.md`](docs/adr/0001-netcode-model.md) on the
    authoritative-server netcode model).

## AI-assisted development

This project's original prototype was hand-written. Development is now being
accelerated with Claude for implementation and
prototyping. Some AI-generated content will be cleaned up as the
project matures.

## License

GPLv3; see [`LICENSE`](LICENSE).
This may change in the future.
