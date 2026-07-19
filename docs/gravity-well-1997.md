# Gravity Well (1997) — reference notes on the inspiration game

Gravitaris' gameplay loop, tactile feel, and visual style are inspired by
*Gravity Well* by Software Engineering, Inc. (Windows, 1997, shareware).
These notes summarize its documented mechanics so future design/implementation
work can consult them without re-researching.

**Primary sources are vendored in this repo** (added 2026-07-19):

- `docs/gwell/manual/GWELL.html` — the original in-game manual, converted to
  HTML. The full text is distilled into this document; consult the original
  only if something here seems ambiguous.
- `docs/gwell/screenshots/` — four screenshots from a real session:
  - `start-game.png` — **annotated** (by Marcin): the Opponents dialog with AI
    personality presets, a labeled freighter with its two "unpacked" cargo
    pods, and a labeled planetary complex + high port + fighter.
  - `fight-near-base.png` — a developed planet under attack; freighters
    inbound; high port with attached units in orbit.
  - `fight-near-freighter.png` — full UI at war: 40+ unit ship list, a
    defeated opponent (black box), the nearest-enemy dial with red
    background.
  - `fight-missiles-and-undeveloped-base.png` — an early-stage complex (base
    only) with fighters skirmishing.

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

## Full mechanics reference (distilled from the vendored manual)

### Structure of a round ("sector")

- 4–6 stars, each with 1–3 orbiting planets (4–18 planets total).
- Four factions by color: **blue = the player**, red, purple, yellow (three AI
  opponents in solitaire mode). Each AI leader gets a personality preset,
  chosen in an Opponents dialog at round start — presets seen/annotated:
  Aggressive, Determined, Shrewd, Tenacious, Voracious, Defensive, Maniacal.
- Everyone starts with one fully developed complex. Objective: claim every
  planet in the sector. **An opponent is defeated when all of its colonies AND
  freighters are destroyed** (it can no longer expand). The manual calls this
  the only sure way to win.

### The conquest loop

1. Player scouts a planet and **lands on it** — that claims it (plants the
   flag). If an enemy complex is there, it must be destroyed first.
2. Friendly **freighters are dispatched automatically**. A freighter must be
   fully loaded with cargo before it can build. Each construction **consumes
   the freighter along with its cargo**.
3. Freighters build, in fixed order: **Base → Colony → High Port**. The
   Base/High Port then themselves construct their sub-installations (see
   unit list). Eventually the new complex parallels the starting one.
4. New fighters/freighters are built automatically by Labs and Space Docks.
   **Landing at a base/high port designates it as the fighter construction
   site** — future replacement fighters appear there. (This is the respawn
   mechanic: your fighter is rebuilt where you last designated.)

### Landing rules

- Two safe landing sites: a planet's surface, and a High Port's flight deck
  (planet is easier). Neither has a gameplay advantage.
- Requirements: correct rotation (thrusters firing against gravity, i.e.
  retro-burn attitude) + low enough speed on contact.
- The HUD's velocity indicator background turns blue when slow enough to
  land safely.
- Wrong rotation at low speed → damage, usually settles upright. Excessive
  speed → instant destruction.

### The nine unit types

Planetside (drawn inside the planet's outline in the original):

| Unit | Role | Notes |
|---|---|---|
| **Base** | HQ; converts raw → finished materials; auto-firing planetary defenses | First thing built on a claimed planet; constructs Lab + Comm Center itself |
| **Colony** | The ONLY producer of raw materials | Losing all colonies+freighters = defeat |
| **Lab** | Builds fighters + freighters; researches fighter upgrades | All labs cooperate; more labs = faster tech. Labs flash when an upgrade is ready — land at an accompanying base/high port to install it |
| **Comm Center** | Sensor coverage feeding the radar display | |

Orbiting:

| Unit | Role | Notes |
|---|---|---|
| **High Port** | Orbiting base: fighter landing deck, converts raw → finished, orbital defenses | Constructs Space Dock + Sensor Array itself |
| **Space Dock** | Builds fighters + freighters (orbital Lab) | Attached to a High Port; named "Space Dock-N" after High Port #N |
| **Sensor Array** | Orbital Comm Center (radar coverage) | Attached to a High Port, same naming |

Mobile:

| Unit | Role | Notes |
|---|---|---|
| **Fighter** | The player's ship (and each AI leader's). Scout/claim/attack/defend | Only directly-controlled unit |
| **Freighter** | Colonization + resupply. Slow; hull + two visible cargo pods | Neutral commerce: all freighters render in the same color on radar regardless of owner. Interceptable — killing enemy freighters strangles their expansion |

### Economic model (fully automated, no user interaction)

- Colony produces **raw materials**; supplies its Base and the High Port
  overhead.
- Base and High Port convert raw → **finished materials**; everything repairs
  by consuming finished materials from its host.
- Labs/Space Docks build fighters + freighters from their host's finished
  materials, automatically, when none of that type is present where needed.
- Freighters load raw materials at colonies; deliver to needy planets (a
  planet without a colony slowly drains its stores on repairs); when full,
  they can construct Base/Colony/High Port, consuming themselves.
- New-unit rule: a constructor only builds a unit if no other unit of that
  type is already present on/above that planet.

### Controls (original bindings)

Rotate left/right (arrows), thrust (up), fire gun (down or F), fire
rocket/missile (D), zoom in/out/reset (PgUp/PgDn/End), view own/red/purple/
yellow leader fighter (F1–F4/Home), pause (Pause).

## UI reference (what the original shows, element by element)

Layout: fixed left sidebar (~25% width) + main view window. Top-to-bottom in
the sidebar:

1. **Radar display** — whole-sector overview. Stars = large yellow dots;
   planets = medium green dots; **claimed planets get a colored box** in the
   owner's color; ships = small colored dots (freighters all one neutral
   color); the selected ship gets a white box around it.
2. **Opponent status row** — four colored boxes (blue/red/purple/yellow). A
   box turns **black when that opponent is defeated**. The row **flashes when
   your labs have an upgrade ready** to collect.
3. **Shields bar** — light blue, darkens as shields absorb damage.
4. **Rocket/missile count** — one vertical tick per munition; purple =
   rockets, white = missiles.
5. **Damage bar** — green when healthy, fills red with damage; fully red =
   destroyed.
6. **Velocity bar** — cyan, proportional to current/max speed. **Background
   turns blue when slow enough to land** — this doubles as the landing aid.
7. **Five round dials**, left to right: analog clock (system time); nearest
   star (yellow needle); nearest planet (green needle); nearest enemy (red
   needle, **dial background turns red when an enemy is near** — the threat
   warning); own-fighter orientation (ship glyph).
8. **Ship list** — scrolling list of every friendly unit except the fighter.
   Each entry is three lines: `N. Name` (Space Docks/Sensor Arrays are
   suffixed with their High Port's number), a damage bar (bar length scaled
   to the unit's relative strength, so a Base's bar is longer than a Lab's),
   and a materials bar (raw = magenta for Freighters/Colonies, finished =
   blue for Bases/High Ports).

Selection/view model: the main view always follows one selected ship (default
your fighter). Click a ship in the view, the radar, or the ship list to
select it; selected non-fighter ships draw in white in the view, red text in
the list, white box on radar. Clicking the status indicators re-selects your
fighter. F2–F4 spectate enemy leaders. When an enemy fighter is selected, the
indicator cluster shows *its* stats instead of yours.

## Design implications for Gravitaris (status per IDEAS.md)

- **Landing as a skill mechanic: adopted.** Planets are conquered by landing
  on them (after destroying anything built there); hard landings damage the
  ship. This makes landing physics core to the loop, worth real tuning
  effort.
- **Economy: full freighter logistics adopted** (confirmed 2026-07-19, see
  `gravity-well-mode-plan.md`): the materials flow is physical, like the
  original — colonies produce raw, freighters haul it, bases/high ports
  convert it, construction consumes freighters. IDEAS.md's "auto-upgrade"
  wording was an abstraction over exactly this background activity, not a
  replacement for it. One deliberate difference: **freighters don't land** —
  they enter a kinematic 2-body orbit at their destination and work from
  orbit.

## Sources

- [MobyGames — Gravity Well (1997)](https://www.mobygames.com/game/50327/gravity-well/)
- [Internet Archive — Gravity Well v3.5 (playable/downloadable)](https://archive.org/details/GWELL35X)
- [PLBM Games — the *other* Gravity Well (disambiguation)](http://plbm.com/oldsite/gravwell.htm)
- [Tom's Hardware thread identifying the wireframe look](https://forums.tomshardware.com/threads/old-windows-95-game-w-vector-style-graphics.106958/)
