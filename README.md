# boyzpvp

Retro (Daggerfall-style) first-person class-based PvP. C++17, SDL3, OpenGL 3.3, ENet.

## Build

Dependencies (SDL3, ENet, GLM) are fetched automatically by CMake.

```sh
cmake -S . -B build
cmake --build build -j
```

### Windows

Either toolchain works; both link the runtime statically, so the resulting `.exe` files need no extra DLLs
(only stock Windows 10/11 system libraries).

Visual Studio 2022 / Build Tools:

```sh
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64
cmake --build build-msvc --config Release
# -> build-msvc/Release/boyz_client.exe, boyz_server.exe, maps/
```

MSYS2 UCRT64 (MinGW-w64 GCC), from a shell with `C:\msys64\ucrt64\bin` on `PATH`:

```sh
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-win
# -> build-win/boyz_client.exe, boyz_server.exe, maps/
```

The `maps/` folder is copied next to the executables, so that folder can be zipped and given to
other players as-is. Windows Firewall will ask to allow the server the first time it listens.

## Run

```sh
./build/boyz_server                       # UDP port 27960, map "arena"
./build/boyz_server --port 27961 --map arena
./build/boyz_client --name alice              # pick a class in game
./build/boyz_client --name carol --class mage # or skip the picker
./build/boyz_client --host 192.168.1.20 --name bob --class warrior
```

Controls: WASD, mouse look, Space to jump, LMB primary ability, RMB secondary ability,
Q third ability, Esc to release the mouse (Esc again to quit), F12 for a screenshot.
Hold Tab for the scoreboard (it also opens while you are dead).
Press C to change class (keys 1-6 pick). The switch happens when you die and respawn; if you're
already dead you respawn as the new class. Without `--class` you spectate until you pick one. Kills show in the top-right kill feed.

| Class   | HP  | LMB                                    | RMB                                  | Q                                              |
|---------|-----|----------------------------------------|--------------------------------------|------------------------------------------------|
| warrior | 150 | sword swing (30, melee cone)           | charge (short dash, 5s cd)           | war cry (8 to all within 6 m, slows 2.5s; 12s cd) |
| mage    | 90  | fireball (26 + 14 splash)              | frostbolt (14, slows 50% for 2s)     | blink (8 m forward, 9s cd)                     |
| druid   | 110 | thorns (11, very fast fire rate)       | regrowth (heal 50 over 5s, 10s cd)   | entangle (6, roots 1.75s; 11s cd)              |
| healer  | 100 | holy bolt (19)                         | heal (40 instant, 8s cd)             | holy nova (16 to all within 5 m, heals you 15; 12s cd) |
| rogue   | 95  | dagger (18, x3 backstab from behind)   | stealth (8s, 12s cd)                 | shadowstep (appear behind who you aim at, 14 m; 14s cd) |
| necromancer | 100 | life drain (17, heals you 60% of it) | raise dead (skeleton minion, 16s cd) | bone burst (blow up your skeleton: 36, 3.5 m; 8s cd) |

Stealth: you're invisible to enemies beyond 3.5 m (closer they see a faint shimmer) and move at 90%
speed. Attacking or taking damage breaks it. The server doesn't send stealthed players' positions to
clients that can't see them, so modified clients can't reveal them either.

Raise dead: a skeleton (45 HP, lasts 20 s) chases the nearest enemy it can see and claws for 9.
Otherwise it follows you. You can only have one, and it crumbles when you die. Its kills count as yours.

Roots stop you walking, jumping, charging and blinking. Combos: a fireball that hits a slowed
target (frostbolt, war cry) SHATTERs for x1.6, and a sword hit on a rooted target CRUSHes for x1.5;
both use up the slow/root. Shadowstep doesn't break stealth, and only finds targets you could see.
Shadowstep and bone burst cost nothing if there's no one to step to / no skeleton.

Kill streaks are announced at 3, 5, 7, 10, ... kills without dying. From 3 on you carry a bounty
(a gold light over your head, a * on the scoreboard): whoever kills you gets 1 extra point per
kill of the streak past 2. The scoreboard ranks by points (kills + bounties).

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
Players are camera-facing billboard sprites, tinted by class, with 8 viewing directions (5 drawn,
3 mirrored) and a walk cycle. Dead players collapse and leave a body for a while (skeletons a heap
of bones). Your hits show floating damage numbers; melee hits briefly freeze the swing, and big
hits and explosions shake the camera. Slowed players look icy blue, rooted ones green.

Dynamic lighting: flickering wall torches, projectiles (with glowing trails), impacts, heals and
spell casts are point lights (the 32 nearest the camera each frame), lit per pixel with surface normals so light doesn't bleed through walls.
Ambient is kept dim so spells light up the dungeon. No shadows: light still reaches floors on the
other side of a thin wall if they're within its radius.
