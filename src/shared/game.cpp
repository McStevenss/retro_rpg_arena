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

// Balance lives here. Indexed by AbilityId.
const AbilityInfo kAbilities[] = {
    melee("sword", 0.6f, 30, 2.6f, 0.55f),
    timed("charge", AbilityKind::Dash, 5.0f, 0, 0.2f),
    projectile("fireball", 0.9f, 26, 20, 0.25f, 2.2f, 14),
    projectile("frostbolt", 2.5f, 14, 28, 0.18f, 0, 0, 2.0f),
    projectile("thorns", 0.35f, 11, 36, 0.1f),
    timed("regrowth", AbilityKind::HealOverTime, 10.0f, 50, 5.0f),
    projectile("holy bolt", 0.7f, 19, 26, 0.18f),
    timed("heal", AbilityKind::SelfHeal, 8.0f, 40, 0),
};
static_assert(sizeof(kAbilities) / sizeof(kAbilities[0]) == static_cast<size_t>(AbilityId::Count));

const ClassInfo kClasses[] = {
    {"warrior", 150.0f, 6.0f, {AbilityId::SwordSwing, AbilityId::Charge}},
    {"mage", 90.0f, 6.5f, {AbilityId::Fireball, AbilityId::FrostBolt}},
    {"druid", 110.0f, 7.0f, {AbilityId::Thorns, AbilityId::Regrowth}},
    {"healer", 100.0f, 6.5f, {AbilityId::HolyBolt, AbilityId::Heal}},
};

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

uint8_t simulate(PlayerState& s, const InputCmd& in, PlayerClass cls, float dt) {
    const ClassInfo& ci = classInfo(cls);
    for (auto& cd : s.cooldown)
        if (cd) --cd;
    if (s.slowTicks) --s.slowTicks;

    s.yaw = in.yaw;
    s.pitch = std::clamp(in.pitch, -1.5f, 1.5f);
    const float fx = std::sin(s.yaw), fz = -std::cos(s.yaw);
    const float rx = std::cos(s.yaw), rz = std::sin(s.yaw);

    uint8_t fired = 0;
    float wx = 0, wz = 0;
    if (s.alive) {
        static const uint8_t kSlotButton[NUM_ABILITY_SLOTS] = {BTN_PRIMARY, BTN_SECONDARY};
        for (int slot = 0; slot < NUM_ABILITY_SLOTS; ++slot) {
            if (!(in.buttons & kSlotButton[slot]) || s.cooldown[slot] != 0) continue;
            const AbilityInfo& ab = abilityInfo(ci.abilities[slot]);
            s.cooldown[slot] = ab.cooldownTicks;
            fired |= static_cast<uint8_t>(1 << slot);
            if (ab.kind == AbilityKind::Dash) s.dashTicks = ab.durationTicks;
        }

        float f = static_cast<float>(std::clamp<int>(in.forward, -1, 1));
        float st = static_cast<float>(std::clamp<int>(in.strafe, -1, 1));
        wx = fx * f + rx * st;
        wz = fz * f + rz * st;
        float len = std::sqrt(wx * wx + wz * wz);
        if (len > 1e-3f) { wx /= len; wz /= len; }
        float speed = ci.moveSpeed * (s.slowTicks ? SLOW_MULT : 1.0f);
        wx *= speed;
        wz *= speed;
    } else {
        s.dashTicks = 0;
    }

    if (s.dashTicks) {
        --s.dashTicks;
        float sp = ci.moveSpeed * DASH_SPEED_MULT;
        s.vx = fx * sp;
        s.vz = fz * sp;
    } else if (s.onGround) {
        s.vx = wx;
        s.vz = wz;
        if (s.alive && (in.buttons & BTN_JUMP)) {
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
    return fired;
}

}  // namespace game
