# boyzpvp

Retro (Daggerfall-style) first-person class-based PvP. C++17, SDL3, OpenGL 3.3, ENet.

## Build

Dependencies (SDL3, ENet, GLM) are fetched automatically by CMake.

```sh
cmake -S . -B build
cmake --build build -j
```

## Run

```sh
./build/boyz_server                       # UDP port 27960, map "arena"
./build/boyz_server --port 27961 --map arena
./build/boyz_client --name alice --class mage
./build/boyz_client --host 192.168.1.20 --name bob --class warrior
```

Controls: WASD, mouse look, Space to jump, LMB primary ability, RMB secondary ability,
Esc to release the mouse (Esc again to quit), F12 for a screenshot.
Kills and joins are printed in the client terminal; your K/D is in the window title.

| Class   | HP  | LMB                                    | RMB                                  |
|---------|-----|----------------------------------------|--------------------------------------|
| warrior | 150 | sword swing (30, melee cone)           | charge (short dash, 5s cd)           |
| mage    | 90  | fireball (26 + 14 splash)              | frostbolt (14, slows 50% for 2s)     |
| druid   | 110 | thorns (11, very fast fire rate)       | regrowth (heal 50 over 5s, 10s cd)   |
| healer  | 100 | holy bolt (19)                         | heal (40 instant, 8s cd)             |

All numbers live in `src/shared/game.cpp` (`kAbilities`, `kClasses`).

Practice target: `BOYZ_BOT=1 ./build/boyz_client --name bot --class mage` runs a client
that aims at the nearest visible player and holds attack.
To play over the internet, forward UDP 27960 to the machine running the server.

## Maps

Maps are text files in `maps/` (see `maps/arena.map`, which documents the format at the top).
Each grid character is a column type from the legend: a floor height, a ceiling height, optional
`slab`s (balconies, bridges, the floor between storeys), `stairs`, `spawn` points and wall
`torch`es. That's
enough for tall halls, balconies you can walk under, stacked rooms, platforms and staircases.
Edit a map and restart the server; no recompile needed. Clients load the map the server names
and refuse to join if their copy differs.

## Layout

```
src/shared/   game rules + simulation (used by BOTH client prediction and server)
  game.h/.cpp   constants, classes, movement/collision
  map.cpp       map loading + spatial queries (columns, line of sight)
  protocol.h    network messages
src/server/   headless authoritative server (no SDL/GL)
src/client/   SDL3 window, OpenGL renderer, input, prediction, interpolation
maps/         map files
```

## Netcode model

- Server runs a fixed 60 Hz sim and is the only authority on state.
- Clients send an `InputCmd` every tick (with the last 4 repeated to survive packet loss).
- Client predicts its own movement immediately, then reconciles when a snapshot arrives:
  it takes the server state and replays inputs the server hasn't processed yet.
- Other players are rendered ~100 ms in the past, interpolated between 20 Hz snapshots.
- Cooldowns, slows and dashes are part of the shared `PlayerState`, so they're predicted too.
- Combat is resolved only on the server: projectiles (sub-stepped, so they can't tunnel),
  melee cones with line-of-sight, splash, heals, deaths and respawns (furthest spawn from enemies).
  Results go to clients as reliable events (`MSG_EVENTS`) that drive effects and the kill feed.
- Not yet: lag compensation for melee/projectiles, client-side predicted projectiles.

## Look

The world renders into a 400x225 framebuffer that's upscaled with nearest filtering.
The shader adds flat per-face shading, dark distance fog and ordered-dither colour quantisation.
Players are camera-facing billboard sprites, tinted by class.

Dynamic lighting: flickering wall torches, projectiles (with glowing trails), impacts, heals and
spell casts are point lights (the 32 nearest the camera each frame), lit per pixel with surface normals so light doesn't bleed through walls.
Ambient is kept dim so spells light up the dungeon. No shadows: light still reaches floors on the
other side of a thin wall if they're within its radius.
