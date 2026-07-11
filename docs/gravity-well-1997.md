# Gravity Well (1997) — reference notes on the inspiration game

Gravitaris' gameplay loop, tactile feel, and visual style are inspired by
*Gravity Well* by Software Engineering, Inc. (Windows, 1997, shareware).
These notes summarize its documented mechanics so future design/implementation
work can consult them without re-researching.

**Disambiguation**: several 90s games share the name. In particular a 1996
Lunar-Lander-style *Gravity Well* by PLBM Games (fuel/landers/diamonds
mechanics) shows up in the same searches and is NOT the inspiration. When
researching, filter for Software Engineering, Inc., Windows, 1997.

## Documented mechanics

- **Strategy played as an arcade game.** Fast-paced planetary conquest where a
  large part of the game window is direct control of **wireframe ships** in
  **Asteroids-style combat**. The wireframe/vector look is the source of
  Gravitaris' aesthetic — it's faithful, not just a stylistic overlay.
- **Conquest loop via logistics.** The player *scouts* planets and establishes
  landing sites; **NPC colonization ships and freighters then follow** and
  build the infrastructure — colonies on habitable planets, outposts/tech
  bases on barren ones. The player claims and defends; freighters build.
- **Landing as a core skill.** Landing on a planet or star base requires
  heading straight for the target, then flipping and retro-thrusting at the
  right moment to kill velocity against gravitational attraction.
- **Structure.** Four playable races. 'Solitaire' mode vs. three AI opponents,
  or multiplayer.

## Design implications for Gravitaris (status per IDEAS.md)

- **Landing as a skill mechanic: adopted.** Planets are conquered by landing
  on them (after destroying anything built there); hard landings damage the
  ship. This makes landing physics core to the loop, worth real tuning
  effort.
- **Economy: the simple auto-upgrade model was chosen** for the base mode
  (upgrades tick faster with more bases), not the original's physical
  freighter logistics. Freighters/supply lines may still fit the campaign
  mode later — this doc keeps the description for that purpose.

## Sources

- [MobyGames — Gravity Well (1997)](https://www.mobygames.com/game/50327/gravity-well/)
- [Internet Archive — Gravity Well v3.5 (playable/downloadable)](https://archive.org/details/GWELL35X)
- [PLBM Games — the *other* Gravity Well (disambiguation)](http://plbm.com/oldsite/gravwell.htm)
- [Tom's Hardware thread identifying the wireframe look](https://forums.tomshardware.com/threads/old-windows-95-game-w-vector-style-graphics.106958/)
