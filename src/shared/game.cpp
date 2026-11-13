#include "game.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace game {

static const ClassInfo kClasses[] = {
    {"warrior", 150.0f, 6.0f},
    {"mage", 90.0f, 6.5f},
    {"druid", 110.0f, 7.0f},
    {"healer", 100.0f, 6.5f},
};

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

// Moves `pos` along one axis by `delta`, resolving against solid tiles.
// Returns true if blocked.
static bool moveAxis(float& pos, float other, float delta, bool isX) {
    if (delta == 0.0f) return false;
    pos += delta;
    const float r = PLAYER_RADIUS;
    int a0 = static_cast<int>(std::floor((pos - r) / TILE));
    int a1 = static_cast<int>(std::floor((pos + r) / TILE));
    int b0 = static_cast<int>(std::floor((other - r) / TILE));
    int b1 = static_cast<int>(std::floor((other + r) / TILE));
    for (int a = a0; a <= a1; ++a) {
        for (int b = b0; b <= b1; ++b) {
            bool solid = isX ? isSolid(a, b) : isSolid(b, a);
            if (!solid) continue;
            if (delta > 0) pos = a * TILE - r - 1e-4f;
            else pos = (a + 1) * TILE + r + 1e-4f;
            return true;
        }
    }
    return false;
}

void simulate(PlayerState& s, const InputCmd& in, PlayerClass cls, float dt) {
    s.yaw = in.yaw;
    s.pitch = std::clamp(in.pitch, -1.5f, 1.5f);

    float f = static_cast<float>(std::clamp<int>(in.forward, -1, 1));
    float st = static_cast<float>(std::clamp<int>(in.strafe, -1, 1));
    float fx = std::sin(s.yaw), fz = -std::cos(s.yaw);
    float rx = std::cos(s.yaw), rz = std::sin(s.yaw);
    float wx = fx * f + rx * st;
    float wz = fz * f + rz * st;
    float len = std::sqrt(wx * wx + wz * wz);
    if (len > 1e-3f) { wx /= len; wz /= len; }
    float speed = classInfo(cls).moveSpeed;
    wx *= speed;
    wz *= speed;

    if (s.onGround) {
        s.vx = wx;
        s.vz = wz;
        if (in.buttons & BTN_JUMP) {
            s.vy = JUMP_VEL;
            s.onGround = 0;
        }
    } else {
        float k = std::min(1.0f, AIR_CONTROL * dt);
        s.vx += (wx - s.vx) * k;
        s.vz += (wz - s.vz) * k;
    }
    s.vy -= GRAVITY * dt;

    if (moveAxis(s.x, s.z, s.vx * dt, true)) s.vx = 0;
    if (moveAxis(s.z, s.x, s.vz * dt, false)) s.vz = 0;

    s.y += s.vy * dt;
    if (s.y <= 0.0f) {
        s.y = 0.0f;
        s.vy = 0.0f;
        s.onGround = 1;
    } else {
        s.onGround = 0;
    }
    const float headroom = WALL_H - PLAYER_HEIGHT;
    if (s.y > headroom) {
        s.y = headroom;
        if (s.vy > 0) s.vy = 0;
    }
}

}  // namespace game
