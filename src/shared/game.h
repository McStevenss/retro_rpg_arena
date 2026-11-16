// Game rules and simulation shared by client (prediction) and server (authority).
// Anything that affects gameplay outcome must live here so both sides agree.
#pragma once
#include <cstdint>
#include <string>

namespace game {

// --- Timing -----------------------------------------------------------------
constexpr int TICK_RATE = 60;                  // simulation steps per second
constexpr float TICK_DT = 1.0f / TICK_RATE;
constexpr int SNAPSHOT_EVERY = 3;              // server sends state at 60/3 = 20 Hz
constexpr uint16_t DEFAULT_PORT = 27960;
constexpr int MAX_PLAYERS = 16;
constexpr int RESPAWN_TICKS = 3 * TICK_RATE;

// --- World scale (1 unit = 1 meter) -----------------------------------------
constexpr float TILE = 3.0f;
constexpr float STEP_HEIGHT = 0.55f;      // walk up ledges/stairs this high
constexpr float AIR_STEP_HEIGHT = 0.3f;   // forgiving ledge-climb while airborne
constexpr float PLAYER_RADIUS = 0.35f;
constexpr float PLAYER_HEIGHT = 1.8f;
constexpr float EYE_HEIGHT = 1.6f;
constexpr float GRAVITY = 20.0f;
constexpr float JUMP_VEL = 6.5f;
constexpr float AIR_CONTROL = 2.0f;
constexpr float DASH_SPEED_MULT = 3.2f;
constexpr float SLOW_MULT = 0.5f;
constexpr float STEALTH_SPEED_MULT = 0.9f;
constexpr float STEALTH_REVEAL_DIST = 3.5f;  // enemies this close see a stealthed player shimmer
constexpr float BACKSTAB_DOT = -0.2f;        // attacker within ~100 deg behind the victim's facing
constexpr int STREAK_BOUNTY_MIN = 3;         // kills in a row before you carry a bounty

// --- Minions (necromancer skeletons) ------------------------------------------
constexpr float MINION_HP = 45.0f;
constexpr float MINION_SPEED = 5.2f;
constexpr float MINION_HEIGHT = 1.6f;
constexpr int MINION_LIFETIME_TICKS = 20 * TICK_RATE;
constexpr float MINION_SIGHT = 22.0f;       // how far it looks for a target
constexpr float MINION_LEASH = 4.0f;        // follows its owner within this distance when idle

// --- Abilities --------------------------------------------------------------
enum class AbilityId : uint8_t {
    SwordSwing, Charge,       // warrior
    Fireball, FrostBolt,      // mage
    Thorns, Regrowth,         // druid
    HolyBolt, Heal,           // healer
    Dagger, Stealth,          // rogue
    LifeDrain, RaiseDead,     // necromancer
    WarCry, Blink, Entangle, HolyNova, Shadowstep, BoneBurst,  // Q abilities, one per class
    SkeletonClaw,             // the necromancer's minion (not a player ability)
    Count
};

enum class AbilityKind : uint8_t {
    Melee, Projectile, Dash, SelfHeal, HealOverTime, Stealth, Summon,
    Blink,       // teleport `range` m forward (stops at walls)
    Nova,        // hits everything within `range` around you, heals you `selfHeal`
    Shadowstep,  // teleport behind the enemy you're aiming at (server picks; refunded if none)
    Detonate,    // blow up your minion (refunded if you have none)
};

// Combos: a hit does `comboMult` x damage if the target is in this state, and
// uses the state up.
enum ComboOn : uint8_t { COMBO_NONE, COMBO_SLOWED, COMBO_ROOTED };

struct AbilityInfo {
    const char* name;
    AbilityKind kind;
    uint16_t cooldownTicks;
    float amount;            // damage (melee/projectile) or total heal
    float range;             // melee reach, m
    float arcCos;            // melee cone, cos(half angle); -1 = all around
    float projSpeed;         // m/s
    float projRadius;        // m
    float splashRadius;      // m, 0 = no splash
    float splashDamage;      // at the centre, falls off to half at the edge
    uint16_t slowTicks;      // slow applied to whoever is hit
    uint16_t durationTicks;  // dash / heal-over-time / stealth length
    float backstabMult;      // melee: damage multiplier when hitting from behind (0 = none)
    float lifesteal;         // fraction of damage dealt that heals the attacker
    uint16_t rootTicks;      // root (can't move, dash or blink) applied to whoever is hit
    float selfHeal;          // nova: heals the caster
    uint8_t comboOn;         // ComboOn
    float comboMult;
    const char* comboName;   // shown on screen, e.g. "SHATTER"
    bool keepsStealth;       // using it doesn't break your stealth
};
const AbilityInfo& abilityInfo(AbilityId id);

// --- Classes ----------------------------------------------------------------
enum class PlayerClass : uint8_t { Warrior, Mage, Druid, Healer, Rogue, Necromancer, Count };

constexpr int NUM_ABILITY_SLOTS = 3;  // 0 = primary (LMB), 1 = secondary (RMB), 2 = Q

struct ClassInfo {
    const char* name;
    float maxHp;
    float moveSpeed;  // m/s
    AbilityId abilities[NUM_ABILITY_SLOTS];
};
const ClassInfo& classInfo(PlayerClass c);
bool parseClass(const char* s, PlayerClass& out);

// --- Input / state ----------------------------------------------------------
enum Buttons : uint8_t { BTN_JUMP = 1, BTN_PRIMARY = 2, BTN_SECONDARY = 4, BTN_TERTIARY = 8 };

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

// Everything here is predicted by the client, so it must only be changed by
// simulate() -- or by the server, which then sends it back in snapshots.
struct PlayerState {
    float x, y, z;     // feet position
    float vx, vy, vz;
    float yaw, pitch;
    uint16_t cooldown[NUM_ABILITY_SLOTS];  // ticks until ability can be used
    uint16_t slowTicks;
    uint16_t dashTicks;
    uint16_t stealthTicks;  // > 0 = stealthed; broken by attacking or taking damage
    uint16_t rootTicks;     // > 0 = rooted in place
    uint8_t onGround;
    uint8_t alive;
};

// Advance one player by one tick. Deterministic: same input -> same output.
// Returns a bitmask of ability slots that were triggered this tick.
uint8_t simulate(PlayerState& s, const InputCmd& in, PlayerClass cls, float dt);
// Movement only (no abilities), at the given ground speed. Used for minions.
void simulateMove(PlayerState& s, const InputCmd& in, float speed, float dt);
// True if `ax,az` stands behind a victim at `vx,vz` facing `victimYaw`.
bool isBehind(float vx, float vz, float victimYaw, float ax, float az);
// Could a player stand with their feet at x,y,z without overlapping walls?
bool spaceFree(float x, float y, float z);

void viewDir(float yaw, float pitch, float& dx, float& dy, float& dz);

// --- Map --------------------------------------------------------------------
// The world is a grid of TILE x TILE columns. Each column is a stack of solid
// vertical spans (floor, slabs, ceiling) plus optionally a staircase, which
// physics treats as a smooth ramp.
constexpr float WORLD_BOTTOM = -100.0f;
constexpr float WORLD_TOP = 100.0f;
constexpr int MAX_SPANS = 6;

struct Span {
    float y0, y1;
};

struct Ramp {
    int8_t dx, dz;  // direction the stairs rise toward
    float h0, h1;   // height at the low and high edge of the tile
};

struct Column {
    Span spans[MAX_SPANS];  // sorted bottom to top, non-overlapping
    uint8_t numSpans;
    uint8_t hasRamp;
    Ramp ramp;
};

bool loadMap(const std::string& path, std::string& err);
// Resolves a map name ("arena") or path to a file, searching ./maps, the
// executable's directory and the source tree. Returns "" if not found.
std::string findMapFile(const std::string& nameOrPath, const char* argv0);
const std::string& mapName();
uint32_t mapHash();  // so clients can verify they have the same map as the server

int mapWidth();
int mapHeight();
const Column& column(int tx, int tz);  // out of bounds = solid
// Solid intervals of a tile as seen by a box covering [x0,x1]x[z0,z1]
// (stairs count up to their highest point under the box). `out` needs
// room for MAX_SPANS + 1 entries. Returns the count.
int solidIntervals(int tx, int tz, float x0, float x1, float z0, float z1, Span* out);
bool pointInWall(float x, float y, float z);
bool lineOfSight(float x0, float y0, float z0, float x1, float y1, float z1);
int spawnCount();
void spawnPoint(int index, float& x, float& y, float& z);

// Wall-mounted torches (visual only: the client draws them and their light).
struct Torch {
    float x, y, z;   // flame position, just off the wall
    int8_t dx, dz;   // direction of the wall it hangs on
};
int torchCount();
const Torch& torchAt(int index);

}  // namespace game
