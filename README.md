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
./build/boyz_server            # UDP port 27960 by default
./build/boyz_client --name alice --class mage
./build/boyz_client --host 192.168.1.20 --name bob --class warrior
```

Classes: `warrior`, `mage`, `druid`, `healer`.
Controls: WASD, mouse look, Space to jump, Esc to release the mouse (Esc again to quit), F12 for a screenshot.
To play over the internet, forward UDP 27960 to the machine running the server.

## Layout

```
src/shared/   game rules + simulation (used by BOTH client prediction and server)
  game.h/.cpp   constants, classes, movement/collision
  map.cpp       test arena (ASCII grid)
  protocol.h    network messages
src/server/   headless authoritative server (no SDL/GL)
src/client/   SDL3 window, OpenGL renderer, input, prediction, interpolation
```

## Netcode model

- Server runs a fixed 60 Hz sim and is the only authority on state.
- Clients send an `InputCmd` every tick (with the last 4 repeated to survive packet loss).
- Client predicts its own movement immediately, then reconciles when a snapshot arrives:
  it takes the server state and replays inputs the server hasn't processed yet.
- Other players are rendered ~100 ms in the past, interpolated between 20 Hz snapshots.

## Look

The world renders into a 400x225 framebuffer that's upscaled with nearest filtering.
The shader adds flat per-face shading, dark distance fog and ordered-dither colour quantisation.
Players are camera-facing billboard sprites, tinted by class.
