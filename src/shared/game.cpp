#include "game.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace game {

namespace {

AbilityInfo melee(const char* name, float cdSec, float dmg, float range, float arcCos) {
    AbilityInfo a{};
    a.name = name; a.kind = AbilityKind::Melee;
    a.cooldownTicks = static_cast<uint16_t>(cdSec * TICK_RATE);
    a.amount = dmg; a.range = range; a.arcCos = arcCos;
    return a;
}

AbilityInfo projectile(const char* name, float cdSec, float dmg, float speed, float radius,
                       float splashRadius = 0, float splashDamage = 0, float slowSec = 0) {
    AbilityInfo a{};
    a.name = name; a.kind = AbilityKind::Projectile;
    a.cooldownTicks = static_cast<uint16_t>(cdSec * TICK_RATE);
    a.amount = dmg; a.projSpeed = speed; a.projRadius = radius;
    a.splashRadius = splashRadius; a.splashDamage = splashDamage;
    a.slowTicks = static_cast<uint16_t>(slowSec * TICK_RATE);
    return a;
}

AbilityInfo timed(const char* name, AbilityKind kind, float cdSec, float amount, float durSec) {
    AbilityInfo a{};
    a.name = name; a.kind = kind;
    a.cooldownTicks = static_cast<uint16_t>(cdSec * TICK_RATE);
    a.amount = amount;
    a.durationTicks = static_cast<uint16_t>(durSec * TICK_RATE);
    return a;
}

AbilityInfo withBackstab(AbilityInfo a, float mult) { a.backstabMult = mult; return a; }
AbilityInfo withLifesteal(AbilityInfo a, float frac) { a.lifesteal = frac; return a; }
AbilityInfo withCombo(AbilityInfo a, ComboOn on, float mult, const char* name) {
    a.comboOn = on; a.comboMult = mult; a.comboName = name;
    return a;
}
AbilityInfo withRoot(AbilityInfo a, float sec) { a.rootTicks = static_cast<uint16_t>(sec * TICK_RATE); return a; }
AbilityInfo withSlow(AbilityInfo a, float sec) { a.slowTicks = static_cast<uint16_t>(sec * TICK_RATE); return a; }
AbilityInfo withRange(AbilityInfo a, float r) { a.range = r; return a; }

// Balance lives here. Indexed by AbilityId.
const AbilityInfo kAbilities[] = {
    withCombo(melee("sword", 0.6f, 30, 2.6f, 0.55f), COMBO_ROOTED, 1.5f, "CRUSH"),
    timed("charge", AbilityKind::Dash, 5.0f, 0, 0.2f),
    withCombo(projectile("fireball", 0.9f, 26, 20, 0.25f, 2.2f, 14), COMBO_SLOWED, 1.6f, "SHATTER"),
    projectile("frostbolt", 2.5f, 14, 28, 0.18f, 0, 0, 2.0f),
    projectile("thorns", 0.35f, 11, 36, 0.1f),
    timed("regrowth", AbilityKind::HealOverTime, 10.0f, 50, 5.0f),
    projectile("holy bolt", 0.7f, 19, 26, 0.18f),
    timed("heal", AbilityKind::SelfHeal, 8.0f, 40, 0),
    withBackstab(melee("dagger", 0.45f, 18, 2.3f, 0.6f), 3.0f),
    timed("stealth", AbilityKind::Stealth, 12.0f, 0, 8.0f),
    withLifesteal(projectile("life drain", 0.8f, 17, 22, 0.2f), 0.6f),
    timed("raise dead", AbilityKind::Summon, 16.0f, 0, 0),
    // Q abilities.
    withSlow(melee("war cry", 12.0f, 8, 6.0f, -1.0f), 2.5f),
    withRange(timed("blink", AbilityKind::Blink, 9.0f, 0, 0), 8.0f),
    withRoot(projectile("entangle", 11.0f, 6, 24, 0.3f), 1.75f),
    [] {
        AbilityInfo a = withRange(timed("holy nova", AbilityKind::Nova, 12.0f, 16, 0), 5.0f);
        a.selfHeal = 15;
        return a;
    }(),
    [] {
        AbilityInfo a = withRange(timed("shadowstep", AbilityKind::Shadowstep, 14.0f, 0, 0), 14.0f);
        a.keepsStealth = true;
        return a;
    }(),
    withRange(timed("bone burst", AbilityKind::Detonate, 8.0f, 36, 0), 3.5f),
    melee("skeleton", 1.0f, 9, 1.7f, 0.3f),
};
static_assert(sizeof(kAbilities) / sizeof(kAbilities[0]) == static_cast<size_t>(AbilityId::Count));

const ClassInfo kClasses[] = {
    {"warrior", 150.0f, 6.0f, {AbilityId::SwordSwing, AbilityId::Charge, AbilityId::WarCry}},
    {"mage", 90.0f, 6.5f, {AbilityId::Fireball, AbilityId::FrostBolt, AbilityId::Blink}},
    {"druid", 110.0f, 7.0f, {AbilityId::Thorns, AbilityId::Regrowth, AbilityId::Entangle}},
    {"healer", 100.0f, 6.5f, {AbilityId::HolyBolt, AbilityId::Heal, AbilityId::HolyNova}},
    {"rogue", 95.0f, 7.0f, {AbilityId::Dagger, AbilityId::Stealth, AbilityId::Shadowstep}},
    {"necromancer", 100.0f, 6.3f, {AbilityId::LifeDrain, AbilityId::RaiseDead, AbilityId::BoneBurst}},
};
static_assert(sizeof(kClasses) / sizeof(kClasses[0]) == static_cast<size_t>(PlayerClass::Count));

}  // namespace

const AbilityInfo& abilityInfo(AbilityId id) {
    int i = static_cast<int>(id);
    if (i < 0 || i >= static_cast<int>(AbilityId::Count)) i = 0;
    return kAbilities[i];
}

const ClassInfo& classInfo(PlayerClass c) {
    int i = static_cast<int>(c);
    if (i < 0 || i >= static_cast<int>(PlayerClass::Count)) i = 0;
    return kClasses[i];
}

bool parseClass(const char* s, PlayerClass& out) {
    for (int i = 0; i < static_cast<int>(PlayerClass::Count); ++i) {
        const char* a = kClasses[i].name;
        const char* b = s;
        while (*a && *b && *a == std::tolower(static_cast<unsigned char>(*b))) { ++a; ++b; }
        if (*a == 0 && *b == 0) {
            out = static_cast<PlayerClass>(i);
            return true;
        }
    }
    return false;
}

bool isBehind(float vx, float vz, float victimYaw, float ax, float az) {
    float dx = ax - vx, dz = az - vz;
    float d = std::sqrt(dx * dx + dz * dz);
    if (d < 1e-3f) return false;
    float fx = std::sin(victimYaw), fz = -std::cos(victimYaw);
    return (dx * fx + dz * fz) / d < BACKSTAB_DOT;
}

void viewDir(float yaw, float pitch, float& dx, float& dy, float& dz) {
    dx = std::sin(yaw) * std::cos(pitch);
    dy = std::sin(pitch);
    dz = -std::cos(yaw) * std::cos(pitch);
}

namespace {

constexpr float EPS = 1e-3f;

template <class F>
void forTilesUnder(float x, float z, F&& f) {
    const float r = PLAYER_RADIUS;
    int x0 = static_cast<int>(std::floor((x - r) / TILE)), x1 = static_cast<int>(std::floor((x + r) / TILE));
    int z0 = static_cast<int>(std::floor((z - r) / TILE)), z1 = static_cast<int>(std::floor((z + r) / TILE));
    Span iv[MAX_SPANS + 1];
    for (int tz = z0; tz <= z1; ++tz)
        for (int tx = x0; tx <= x1; ++tx) {
            int n = solidIntervals(tx, tz, x - r, x + r, z - r, z + r, iv);
            for (int i = 0; i < n; ++i) f(iv[i]);
        }
}

// Would a player whose feet are at yLo (head at yHi) overlap anything solid?
bool boxBlocked(float x, float z, float yLo, float yHi) {
    bool hit = false;
    forTilesUnder(x, z, [&](const Span& s) {
        if (s.y0 < yHi - EPS && s.y1 > yLo + EPS) hit = true;
    });
    return hit;
}

// Highest solid surface under the player at or below yMax.
float floorBelow(float x, float z, float yMax) {
    float best = WORLD_BOTTOM;
    forTilesUnder(x, z, [&](const Span& s) {
        if (s.y1 <= yMax + EPS) best = std::max(best, s.y1);
    });
    return best;
}

// Lowest solid underside above the player at or above yMin.
float ceilingAbove(float x, float z, float yMin) {
    float best = WORLD_TOP;
    forTilesUnder(x, z, [&](const Span& s) {
        if (s.y0 >= yMin - EPS) best = std::min(best, s.y0);
    });
    return best;
}

// Moves horizontally, stepping up small ledges/stairs. Returns true if blocked.
bool moveHorizontal(PlayerState& s, float dx, float dz) {
    if (dx == 0.0f && dz == 0.0f) return false;
    float nx = s.x + dx, nz = s.z + dz;
    if (!boxBlocked(nx, nz, s.y, s.y + PLAYER_HEIGHT)) {
        s.x = nx;
        s.z = nz;
        return false;
    }
    float step = s.onGround ? STEP_HEIGHT : AIR_STEP_HEIGHT;
    if (!boxBlocked(nx, nz, s.y + step, s.y + step + PLAYER_HEIGHT)) {
        s.x = nx;
        s.z = nz;
        s.y = std::max(s.y, floorBelow(nx, nz, s.y + step));
        return false;
    }
    // Blocked: move as far as possible (binary search keeps it simple and
    // works for any shape, including stairs).
    float lo = 0.0f, hi = 1.0f;
    for (int i = 0; i < 8; ++i) {
        float mid = (lo + hi) * 0.5f;
        if (boxBlocked(s.x + dx * mid, s.z + dz * mid, s.y, s.y + PLAYER_HEIGHT)) hi = mid;
        else lo = mid;
    }
    s.x += dx * lo;
    s.z += dz * lo;
    return true;
}

}  // namespace

bool spaceFree(float x, float y, float z) { return !boxBlocked(x, z, y, y + PLAYER_HEIGHT); }

uint8_t simulate(PlayerState& s, const InputCmd& in, PlayerClass cls, float dt) {
    const ClassInfo& ci = classInfo(cls);
    for (auto& cd : s.cooldown)
        if (cd) --cd;
    if (s.stealthTicks) --s.stealthTicks;

    uint8_t fired = 0;
    if (s.alive) {
        static const uint8_t kSlotButton[NUM_ABILITY_SLOTS] = {BTN_PRIMARY, BTN_SECONDARY, BTN_TERTIARY};
        for (int slot = 0; slot < NUM_ABILITY_SLOTS; ++slot) {
            if (!(in.buttons & kSlotButton[slot]) || s.cooldown[slot] != 0) continue;
            const AbilityInfo& ab = abilityInfo(ci.abilities[slot]);
            // Stealth can't be recast while already stealthed.
            if (ab.kind == AbilityKind::Stealth && s.stealthTicks) continue;
            // Roots stop every kind of movement ability.
            if (s.rootTicks && (ab.kind == AbilityKind::Dash || ab.kind == AbilityKind::Blink)) continue;
            s.cooldown[slot] = ab.cooldownTicks;
            fired |= static_cast<uint8_t>(1 << slot);
            if (ab.kind == AbilityKind::Dash) s.dashTicks = ab.durationTicks;
            if (ab.kind == AbilityKind::Blink) {
                // Small steps so walls stop it and it can step up ledges.
                const int STEPS = 16;
                const float fx = std::sin(in.yaw), fz = -std::cos(in.yaw);
                for (int i = 0; i < STEPS; ++i)
                    if (moveHorizontal(s, fx * ab.range / STEPS, 0) | moveHorizontal(s, 0, fz * ab.range / STEPS)) break;
                s.vy = std::max(s.vy, 0.0f);
            }
            // Any other action breaks stealth.
            if (ab.kind == AbilityKind::Stealth) s.stealthTicks = ab.durationTicks;
            else if (!ab.keepsStealth) s.stealthTicks = 0;
        }
    } else {
        s.stealthTicks = 0;
    }
    float speed = ci.moveSpeed * (s.stealthTicks ? STEALTH_SPEED_MULT : 1.0f);
    simulateMove(s, in, speed, dt);
    return fired;
}

void simulateMove(PlayerState& s, const InputCmd& in, float speed, float dt) {
    if (s.slowTicks) --s.slowTicks;
    if (s.rootTicks) --s.rootTicks;

    s.yaw = in.yaw;
    s.pitch = std::clamp(in.pitch, -1.5f, 1.5f);
    const float fx = std::sin(s.yaw), fz = -std::cos(s.yaw);
    const float rx = std::cos(s.yaw), rz = std::sin(s.yaw);

    float wx = 0, wz = 0;
    if (s.alive) {
        float f = static_cast<float>(std::clamp<int>(in.forward, -1, 1));
        float st = static_cast<float>(std::clamp<int>(in.strafe, -1, 1));
        wx = fx * f + rx * st;
        wz = fz * f + rz * st;
        float len = std::sqrt(wx * wx + wz * wz);
        if (len > 1e-3f) { wx /= len; wz /= len; }
        float sp = s.rootTicks ? 0.0f : speed * (s.slowTicks ? SLOW_MULT : 1.0f);
        wx *= sp;
        wz *= sp;
    } else {
        s.dashTicks = 0;
    }

    if (s.rootTicks) s.dashTicks = 0;
    if (s.dashTicks) {
        --s.dashTicks;
        float sp = speed * DASH_SPEED_MULT;
        s.vx = fx * sp;
        s.vz = fz * sp;
    } else if (s.onGround) {
        s.vx = wx;
        s.vz = wz;
        if (s.alive && !s.rootTicks && (in.buttons & BTN_JUMP)) {
            s.vy = JUMP_VEL;
            s.onGround = 0;
        }
    } else {
        float k = std::min(1.0f, AIR_CONTROL * dt);
        s.vx += (wx - s.vx) * k;
        s.vz += (wz - s.vz) * k;
    }
    s.vy -= GRAVITY * dt;

    const bool wasOnGround = s.onGround;
    if (moveHorizontal(s, s.vx * dt, 0)) s.vx = 0;
    if (moveHorizontal(s, 0, s.vz * dt)) s.vz = 0;

    // Stick to the ground when walking down stairs/small ledges instead of
    // launching off them.
    if (wasOnGround && s.vy <= 0) {
        float g = floorBelow(s.x, s.z, s.y + EPS);
        if (s.y > g && s.y - g <= STEP_HEIGHT) s.y = g;
    }

    const float yPrev = s.y;
    s.y += s.vy * dt;
    if (s.vy <= 0) {
        float g = floorBelow(s.x, s.z, yPrev + EPS);
        if (s.y <= g) {
            s.y = g;
            s.vy = 0;
            s.onGround = 1;
        } else {
            s.onGround = 0;
        }
    } else {
        float c = ceilingAbove(s.x, s.z, yPrev + PLAYER_HEIGHT - EPS);
        if (s.y + PLAYER_HEIGHT > c) {
            s.y = c - PLAYER_HEIGHT;
            s.vy = 0;
        }
        s.onGround = 0;
    }
}

}  // namespace game
