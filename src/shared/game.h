// Game rules and simulation shared by client (prediction) and server (authority).
// Anything that affects gameplay outcome must live here so both sides agree.
#pragma once
#include <cstdint>

namespace game {

// --- Timing -----------------------------------------------------------------
constexpr int TICK_RATE = 60;                  // simulation steps per second
constexpr float TICK_DT = 1.0f / TICK_RATE;
constexpr int SNAPSHOT_EVERY = 3;              // server sends state at 60/3 = 20 Hz
constexpr uint16_t DEFAULT_PORT = 27960;
constexpr int MAX_PLAYERS = 16;

// --- World scale (1 unit = 1 meter) -----------------------------------------
constexpr float TILE = 3.0f;
constexpr float WALL_H = 3.0f;
constexpr float PLAYER_RADIUS = 0.35f;
constexpr float PLAYER_HEIGHT = 1.8f;
constexpr float EYE_HEIGHT = 1.6f;
constexpr float GRAVITY = 20.0f;
constexpr float JUMP_VEL = 6.5f;
constexpr float AIR_CONTROL = 2.0f;

// --- Classes ----------------------------------------------------------------
enum class PlayerClass : uint8_t { Warrior, Mage, Druid, Healer, Count };

struct ClassInfo {
    const char* name;
    float maxHp;
    float moveSpeed;  // m/s
};
const ClassInfo& classInfo(PlayerClass c);
bool parseClass(const char* s, PlayerClass& out);

// --- Input / state ----------------------------------------------------------
enum Buttons : uint8_t { BTN_JUMP = 1, BTN_ATTACK = 2, BTN_ABILITY = 4 };

// One simulation tick of player intent. Sent client -> server.
struct InputCmd {
    uint32_t seq;      // increments by 1 every tick
    int8_t forward;    // -1, 0, 1
    int8_t strafe;     // -1, 0, 1
    uint8_t buttons;   // Buttons bitmask
    uint8_t pad;
    float yaw;         // radians, 0 = looking toward -Z
    float pitch;       // radians, + = up
};

struct PlayerState {
    float x, y, z;     // feet position
    float vx, vy, vz;
    float yaw, pitch;
    uint8_t onGround;
};

// Advance one player by one tick. Deterministic: same input -> same output.
void simulate(PlayerState& s, const InputCmd& in, PlayerClass cls, float dt);

// --- Map --------------------------------------------------------------------
int mapWidth();
int mapHeight();
bool isSolid(int tx, int tz);  // out of bounds counts as solid
int spawnCount();
void spawnPoint(int index, float& x, float& z);

}  // namespace game
