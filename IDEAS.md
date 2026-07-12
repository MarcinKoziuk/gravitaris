# TODO

## Complete first steps

- Make sure game visual style looks good
- Switch from entt to flecs for ECS
    - with simple standalone header lib for signals/observable for ResourceLoader
- Configuration
  - Replace Yaml
  - Create a persistent game settings file
- Add a settings RmlUi for visual settings, that persists to configuration
- Introduce Dear ImGui for a dev/debug console
- Introduce GUI for performance monitoring (both rendering, physics, game loop)
- Design new SVG ships
- Start with actual gameplay design and ECS data structures
- Start actual work on UI
- Make some hot-reload functionality for resources
  - and maybe gameplay code if practical (less essential)
- Support WebAssembly as a build target, in line with the goal of being as
  portable/cross-platform as possible (Windows, macOS, Linux, and web)

## Gameplay ideas
The gameplay loop tactile and visual feel should be inspired by the
1990s game Gravity Well by Software Engineering Inc.

### Gameplay modes
- Simple round-based gameplay inspired by Gravity Well (most important one to prototype first!)
- Same as first, but a multiplayer variant
- The final goal is a kind of long-standing campaign game with hexagons tiles similar to Star Trek
  Starfleet Command III
  - Final final goal: this but with multiplayer 

#### Simple game mode
In the simple base game mode, you play in a solar system; with a sun and planet. You can build a base around a planet.
Goal is to build upgrades and conquer other planets while fighting and defending
other players (AI's but later players)

In the game mode there are 2 or more players. One is controlled by player,
others are AI (or later multiplayer players).

Bases: can be on the planet but also orbit it (see Gravity Well)
May wanna consider: static guns, radars. At least start with what Gravity Well has.

Upgrade economy TBD yet. Simplest way is auto-upgrades like gravity well,
and faster upgrades the more bases you have. But a tree or more complex
economy to consider later or other game mode.

Planet is conquered by landing on it. Unless it has stuff built,
then that needs to be destroyed first.

You respawn when your ship is destroyed.
This may have a cost (e.g. lost ammo or upgrades)

Round is won when all enemies planets are conquered or lost when player's last planet is conquered by enemy.
When a round is over, it's over.
Nothing persists in base gamemode.
This gamemode is my "vertical slice".
(may want to think of roguelike aspects later, but this should be optional)

#### Persistent game mode
Details TBD yet. The world map is divided in hexagons and you (or enemy factions)
can conquer them. Supply lines. Grand battles. Allied factions. Diplomacy. Economy.
Maybe a bit of warband inspired.

You'll also have friendly AI's battle in formation.

You can travel to the next tile if you're at the edge,
and other 1 or 2 tiles at the edge are preloaded
but it is fine to have some limitations on objects near edges.

### Gravity & Universe
Planets/suns have gravity, these affects ships.
Not sure about guns yet, I think for simplicity not.
They should affect missiles.

Optimization/stability: bases orbiting should have a fixed path (kinematic bodies?)

There will also be asteroid belts, maybe other phenomena.
Asteroids will be shootable and act a bit like asteroids^^

### Movement
Not truly newtonian. Inspired by gravity well.
So angular momentum is fixed (does not drift) and speed depends on ship type.
Velocity is semi-newtonian; is affected by gravity but there is a limit how
fast you can accelerate (unless you slingshot)
Speed limit * acceleration is also affected by ship class (again, unless slingshot)

Landing is supposed to be tricky (you can be damaged if u land too hard)
You may not go too close or land on suns.

May consider having docking later. Not relavant currently yet.

### AI navigation gravity wells
This might be hard. TBD how to handle it.
Since we have 3-body problem, I was thinking of approximating the
path the AI's ship will take (multithreading) and then compensating
the flight path with this in mind.

### Energy system
Ships have unlimited energy (fusion?) in a sense that it's not a battery.
But limited in a sense that you can't max power everything, and you can tweak
how you distribute this power.
There is a concept of back-up energy, which could be limited (TBD yet)
We don't have a concept of "fuel" probably

### Damage system
It is possible to have damage local to a certain aspect (superficual hull-only, shield system, engines, weapons).
Should be possible to repair it over time, this might need resources/spare parts (TBD)
Probably no concept of "crew"
Something like life support: not sure

### Simulation and Multiplayer
Not fully decided yet.
I was thinking to make the server authoritative regarding physics; client
may also simulate it but is could be corrected if needed.
Some kind of lag compensation should be in place.

For the persistent world game mode: there would be a lightweight server
process/thread per hexagon tile.

### Ships types
Some of the initial important ones
- Fighter: versatile with guns as main weapon and missiles (limited capacity),
  medium armoured but very maneuverable (good engine).
- Interceptor: sleek design, light armour, main weapon is missiles
  is the fastest ship but less manouverable than fighter. Secondary weapon is lasers.
  Intended for a long-range defensive intercept role.
  Has good firepower but limited ammunition.
- Shuttle: jack of all trades, is very customizable and can be equiped with anything
  is a good transport but can also be used in a support combat role
  (e.g. can carry lots of missiles). However its quite an easy target.
  Appears a bit like star trek shuttle (but TBD), but is actually similar size to fighter.
- Artillery ship. Hard to move and is either used for defense or behind the "real"
  frontline because they are easy targets. Needs to be supported by other ships
  to be effective in offensive combat.
- Capital ship: think of a protoss carrier. Huge and expensive.
  Can launch smaller fighter-shuttles and can also port a limited amount of normal ships.

An idea is that it's possible to make "flagship" variants of these vessles.
These are intended to be more expensive and limited, but to be piloted mostly
by players (not AI). These can receive more expensive upgrades and part prototypes.

For the first vertical slice we'll just have a fighter.
The other ones are only applicable when a player has multiple ships to control (with friendly AIs)

### Upgrades
Didn't decide yet, I had two types in mind (maybe we want both in different modes)
- Upgrades where you pick the upgrade path yourself in a tree or like starcraft
  - Could also have some rarer ways to unlock them (e.g. steal/spy or randomly found) 
- Random upgrades like vampire survivors (or a bit of a mix between first and this)

I lean more towards the first mode, the other mode can be an separate game type.

### Ship parts, hardpoints, upgrades
A ship can have customizable slots, e.g. for weapons

A slot can have specific parts, depending on the type of ship or ship design,
some slots might be more restrictive than others (e.g. only support one kind of weapon
on an interceptor, but almost any kinds of weapon on a shuttle)

There's also slots for engines, shields, etc.

The weapon parts will be upgradable with a tech tree; earlier weapons/shield less powerful than later variants,
and later versions also look different/cooler.

### Weapon types
- Laser/phaser: cheap to install. Does great localized damage for non-armoured targets.
  But is less effective than other weapons (but high tier variants do close the gap)
  Travels instantly. Unlimited ammo but limited by ship energy.
- Guns/cannons: think of A-10 gatling gun. Travels less instantly but is more powerful against
  armoured targets. Limited but usually plenty ammo for 1-2 fight encounters.
  Can research multiple variants of guns (AP, explosive, ...)
- Artillery: basically higher-yield slower firing cannons, usually from specialized artillery ships
  or capital ships
- Missiles: automatically lock on, with custom detonation settings.
  Expensive but most powerful. Can be outrun
- Nukes: more expensive and more powerful missiles. But super costly.

Lasers and guns can also be installed in a PDC/PDL variant (point defense cannon/laser)
for defense against missiles.

### Shields and armour

*Shields*
- Bubble forcefield; cheap and easy to install. Absorbs damage the best because of the distance. 
  But it does mean your hitbox is bigger.
  Least "efficient" shield but should be competitive until late game
  A hit affects the entire bubble
- Forcefield plating; close to the hull and thicker and more efficient.
  No penalty to hitbox. Costlier because it has to be shaped to the hull and
  the hull must be developed to support it.
  A bit of the blast/splash damage doesn't get absorbed, but overall it's a stronger
  shield (can shield bigger blasts and doesn't drain as quickly)
  A hit affects the specific targeted plate, but gets equalized over time

*Armour*
- Hull plating: the standard type of common, cheap, hull plating
- Ablative armour; specialized regenerative ablative armour. Fully absorbs damage,
  but a hit affects the specific targeted armour plate (which can take a few hits and regenerates)
  Works extremely wel together with forcefield plating, but expensive.
  Mostly only used in fighters because expense

### Camera
Same as gravity well, so it follows your ship (but I guess you could move it manually too)
There is also a mini-map where you can see bodies and ships.
Fog of war TBD yet. In the simplest version maybe no.

### Audio
Simple, a bit like gravity well
Let's fine some free or AI music at some point