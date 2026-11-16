// Authoritative dedicated server. Owns all game state; clients only send input.
#include <enet/enet.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "game.h"
#include "protocol.h"

using namespace game;

namespace {

volatile std::sig_atomic_t g_running = 1;
void onSignal(int) { g_running = 0; }

constexpr float MELEE_VERTICAL_REACH = 1.6f;
constexpr int PROJECTILE_SUBSTEPS = 4;
constexpr int PROJECTILE_LIFETIME_TICKS = 3 * TICK_RATE;

struct Client {
    bool active = false;
    ENetPeer* peer = nullptr;
    PlayerClass cls = PlayerClass::Warrior;
    bool picking = false;        // joined without a class: spectating until one is chosen
    uint8_t nextCls = NO_CLASS;  // switch to this class at the next respawn
    std::string name;
    PlayerState st{};
    uint32_t lastSeq = 0;
    float hp = 0;
    uint16_t hotTicks = 0;      // heal-over-time remaining
    float hotPerTick = 0;
    uint32_t respawnTick = 0;
    uint16_t kills = 0, deaths = 0;
    uint16_t score = 0;   // kills plus bounties
    uint8_t streak = 0;   // kills since last death
};

// A necromancer's skeleton. At most one per player, indexed by owner.
struct Minion {
    bool active = false;
    PlayerState st{};
    float hp = 0;
    int ttl = 0;
    uint16_t attackCd = 0;
    uint16_t stuckTicks = 0;
};

struct Projectile {
    uint16_t id;
    AbilityId ability;
    uint8_t owner;
    float x, y, z, vx, vy, vz;
    int ttl;
};

ENetHost* g_host = nullptr;
Client g_clients[MAX_PLAYERS];
Minion g_minions[MAX_PLAYERS];
std::vector<Projectile> g_projectiles;
uint16_t g_nextProjectileId = 1;
std::vector<NetEvent> g_events;
uint32_t g_tick = 0;

void sendTo(ENetPeer* peer, const void* data, size_t len, int channel, bool reliable) {
    ENetPacket* p = enet_packet_create(data, len, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    enet_peer_send(peer, static_cast<enet_uint8>(channel), p);
}

void broadcast(const void* data, size_t len, int channel, bool reliable) {
    ENetPacket* p = enet_packet_create(data, len, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    enet_host_broadcast(g_host, static_cast<enet_uint8>(channel), p);
}

int slotOf(ENetPeer* peer) {
    auto slot = reinterpret_cast<intptr_t>(peer->data);
    if (slot <= 0 || slot > MAX_PLAYERS || !g_clients[slot - 1].active) return -1;
    return static_cast<int>(slot - 1);
}

const char* nameOf(int i) {
    return (i >= 0 && i < MAX_PLAYERS && g_clients[i].active) ? g_clients[i].name.c_str() : "?";
}

float centerY(const Client& c) { return c.st.y + PLAYER_HEIGHT * 0.5f; }

void pushEvent(EventType type, AbilityId ab, int a, int b, float amount, float x, float y, float z) {
    NetEvent e{};
    e.type = type;
    e.ability = static_cast<uint8_t>(ab);
    e.a = a < 0 ? NO_PLAYER : static_cast<uint8_t>(a);
    e.b = b < 0 ? NO_PLAYER : static_cast<uint8_t>(b);
    e.amount = static_cast<int16_t>(std::lround(amount));
    e.x = x; e.y = y; e.z = z;
    g_events.push_back(e);
}

void flushEvents() {
    size_t i = 0;
    while (i < g_events.size()) {
        size_t n = std::min<size_t>(255, g_events.size() - i);
        std::vector<uint8_t> buf(sizeof(MsgEventsHeader) + n * sizeof(NetEvent));
        MsgEventsHeader hdr{MSG_EVENTS, static_cast<uint8_t>(n)};
        std::memcpy(buf.data(), &hdr, sizeof hdr);
        std::memcpy(buf.data() + sizeof hdr, &g_events[i], n * sizeof(NetEvent));
        broadcast(buf.data(), buf.size(), CH_RELIABLE, true);
        i += n;
    }
    g_events.clear();
}

// --- Combat -----------------------------------------------------------------
// Anything that can be hit is a "target": 0..MAX_PLAYERS-1 are players,
// MAX_PLAYERS + i is the minion owned by player i.

constexpr int NUM_TARGETS = 2 * MAX_PLAYERS;
bool isMinion(int t) { return t >= MAX_PLAYERS; }
int minionOwner(int t) { return t - MAX_PLAYERS; }

PlayerState* targetState(int t) {
    if (isMinion(t)) {
        Minion& m = g_minions[minionOwner(t)];
        return m.active ? &m.st : nullptr;
    }
    Client& c = g_clients[t];
    return c.active && c.st.alive ? &c.st : nullptr;
}
float targetHeight(int t) { return isMinion(t) ? MINION_HEIGHT : PLAYER_HEIGHT; }
float targetCenterY(int t) { return targetState(t)->y + targetHeight(t) * 0.5f; }

// Would `attacker` (a player, or -1 for nobody) be hurting itself or its own minion?
bool isFriendly(int attacker, int t) {
    return attacker >= 0 && (t == attacker || (isMinion(t) && minionOwner(t) == attacker));
}

void killMinion(int owner) {
    Minion& m = g_minions[owner];
    if (!m.active) return;
    m.active = false;
    pushEvent(EV_MINION_DIE, AbilityId::RaiseDead, owner, -1, 0, m.st.x, m.st.y + MINION_HEIGHT * 0.5f, m.st.z);
}

// Returns the damage actually dealt.
float applyDamage(int victim, int attacker, AbilityId ab, float amount) {
    if (amount <= 0 || !targetState(victim)) return 0;
    if (isMinion(victim)) {
        Minion& m = g_minions[minionOwner(victim)];
        float dealt = std::min(amount, m.hp);
        m.hp -= amount;
        pushEvent(EV_DAMAGE, ab, attacker, -1, amount, m.st.x, m.st.y + MINION_HEIGHT * 0.5f, m.st.z);
        if (m.hp <= 0) killMinion(minionOwner(victim));
        return dealt;
    }
    Client& v = g_clients[victim];
    float dealt = std::min(amount, v.hp);
    v.hp -= amount;
    v.st.stealthTicks = 0;  // getting hit breaks stealth
    pushEvent(EV_DAMAGE, ab, attacker, victim, amount, v.st.x, centerY(v), v.st.z);
    if (v.hp > 0) return dealt;

    v.hp = 0;
    v.st.alive = 0;
    v.st.dashTicks = 0;
    v.hotTicks = 0;
    v.deaths++;
    v.respawnTick = g_tick + RESPAWN_TICKS;
    pushEvent(EV_KILL, ab, attacker, victim, 0, v.st.x, v.st.y, v.st.z);
    std::printf("[kill] %s -> %s (%s)\n", nameOf(attacker), v.name.c_str(), abilityInfo(ab).name);
    if (attacker >= 0 && attacker != victim && g_clients[attacker].active) {
        Client& k = g_clients[attacker];
        k.kills++;
        k.score++;
        // Ending a streak collects its bounty: one extra point per kill past the threshold.
        if (v.streak >= STREAK_BOUNTY_MIN) {
            k.score = static_cast<uint16_t>(k.score + v.streak - STREAK_BOUNTY_MIN + 1);
            pushEvent(EV_SHUTDOWN, ab, attacker, victim, v.streak, v.st.x, v.st.y, v.st.z);
            std::printf("[bounty] '%s' ended '%s''s streak of %d\n", k.name.c_str(), v.name.c_str(), v.streak);
        }
        if (k.streak < 255) k.streak++;
        if (k.streak == 3 || k.streak == 5 || k.streak == 7 || (k.streak >= 10 && k.streak % 5 == 0)) {
            pushEvent(EV_STREAK, ab, attacker, -1, k.streak, k.st.x, k.st.y, k.st.z);
            std::printf("[streak] '%s' has %d kills in a row\n", k.name.c_str(), k.streak);
        }
    }
    v.streak = 0;
    killMinion(victim);  // the dead don't keep their servants
    return dealt;
}

// Returns the amount actually healed.
float applyHeal(int target, AbilityId ab, float amount, bool announce) {
    Client& c = g_clients[target];
    if (!c.active || !c.st.alive) return 0;
    float before = c.hp;
    c.hp = std::min(classInfo(c.cls).maxHp, c.hp + amount);
    float gained = c.hp - before;
    if (announce) pushEvent(EV_HEAL, ab, target, target, gained, c.st.x, centerY(c), c.st.z);
    return gained;
}

// Bonus damage if the target is in the state this ability combos off (which
// gets used up).
float comboDamage(int attacker, int t, AbilityId id, float dmg) {
    const AbilityInfo& ab = abilityInfo(id);
    PlayerState* v = targetState(t);
    if (!v || ab.comboOn == COMBO_NONE) return dmg;
    uint16_t& state = ab.comboOn == COMBO_SLOWED ? v->slowTicks : v->rootTicks;
    if (!state) return dmg;
    state = 0;
    dmg *= ab.comboMult;
    pushEvent(EV_COMBO, id, attacker, isMinion(t) ? -1 : t, dmg, v->x, targetCenterY(t), v->z);
    return dmg;
}

// Slows and roots from a hit.
void applyStatus(int t, const AbilityInfo& ab) {
    PlayerState* v = targetState(t);
    if (!v) return;
    if (ab.slowTicks) v->slowTicks = std::max(v->slowTicks, ab.slowTicks);
    if (ab.rootTicks) {
        v->rootTicks = std::max(v->rootTicks, ab.rootTicks);
        v->dashTicks = 0;
    }
}

// Can `t` be hit by a melee swing from `a` (cone in front, reach, line of sight)?
bool inMeleeReach(const PlayerState& a, float eyeHeight, const AbilityInfo& ab, int t) {
    const PlayerState* v = targetState(t);
    if (!v) return false;
    const float fx = std::sin(a.yaw), fz = -std::cos(a.yaw);
    float dx = v->x - a.x, dz = v->z - a.z;
    float dist = std::sqrt(dx * dx + dz * dz);
    if (dist > ab.range + PLAYER_RADIUS) return false;
    if (std::fabs(v->y - a.y) > MELEE_VERTICAL_REACH) return false;
    if (dist > 0.3f && (dx * fx + dz * fz) / dist < ab.arcCos) return false;
    return lineOfSight(a.x, a.y + eyeHeight, a.z, v->x, targetCenterY(t), v->z);
}

// Hits everything in a cone in front of the attacker. Uses current
// positions: no lag compensation yet (fine on LAN, noticeable at high ping).
void doMelee(int attacker, AbilityId id) {
    const AbilityInfo& ab = abilityInfo(id);
    const Client& a = g_clients[attacker];
    for (int t = 0; t < NUM_TARGETS; ++t) {
        if (isFriendly(attacker, t) || !inMeleeReach(a.st, EYE_HEIGHT, ab, t)) continue;
        float dmg = ab.amount;
        const PlayerState* v = targetState(t);
        if (ab.backstabMult > 0 && !isMinion(t) && isBehind(v->x, v->z, v->yaw, a.st.x, a.st.z)) {
            dmg *= ab.backstabMult;
            pushEvent(EV_BACKSTAB, id, attacker, t, dmg, v->x, targetCenterY(t), v->z);
        }
        applyDamage(t, attacker, id, comboDamage(attacker, t, id, dmg));
        applyStatus(t, ab);
    }
}

void explode(const Projectile& p, int directVictim) {
    const AbilityInfo& ab = abilityInfo(p.ability);
    pushEvent(EV_IMPACT, p.ability, p.owner, directVictim >= 0 && !isMinion(directVictim) ? directVictim : -1, 0,
              p.x, p.y, p.z);
    if (directVictim >= 0) {
        float dmg = comboDamage(p.owner, directVictim, p.ability, ab.amount);
        float dealt = applyDamage(directVictim, p.owner, p.ability, dmg);
        applyStatus(directVictim, ab);
        if (ab.lifesteal > 0 && dealt > 0) applyHeal(p.owner, p.ability, dealt * ab.lifesteal, true);
    }
    if (ab.splashRadius <= 0) return;
    for (int t = 0; t < NUM_TARGETS; ++t) {
        const PlayerState* v = targetState(t);
        if (!v || t == directVictim || isFriendly(p.owner, t)) continue;
        float cy = targetCenterY(t);
        float dx = v->x - p.x, dy = cy - p.y, dz = v->z - p.z;
        float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        float reach = ab.splashRadius + PLAYER_RADIUS;
        if (d > reach || !lineOfSight(p.x, p.y, p.z, v->x, cy, v->z)) continue;
        applyDamage(t, p.owner, p.ability, ab.splashDamage * (1.0f - 0.5f * d / reach));
    }
}

void spawnProjectile(int owner, AbilityId id) {
    const AbilityInfo& ab = abilityInfo(id);
    const Client& c = g_clients[owner];
    float dx, dy, dz;
    viewDir(c.st.yaw, c.st.pitch, dx, dy, dz);
    Projectile p{};
    p.id = g_nextProjectileId++;
    p.ability = id;
    p.owner = static_cast<uint8_t>(owner);
    p.x = c.st.x + dx * 0.4f;
    p.y = c.st.y + EYE_HEIGHT - 0.15f + dy * 0.4f;
    p.z = c.st.z + dz * 0.4f;
    p.vx = dx * ab.projSpeed;
    p.vy = dy * ab.projSpeed;
    p.vz = dz * ab.projSpeed;
    p.ttl = PROJECTILE_LIFETIME_TICKS;
    if (pointInWall(p.x, p.y, p.z)) {  // fired point-blank into a wall
        explode(p, -1);
        return;
    }
    g_projectiles.push_back(p);
}

// Returns the target the projectile overlaps, or -1.
int projectileHitsTarget(const Projectile& p, float radius) {
    for (int t = 0; t < NUM_TARGETS; ++t) {
        const PlayerState* v = targetState(t);
        if (!v || isFriendly(p.owner, t)) continue;
        float dx = v->x - p.x, dz = v->z - p.z;
        float rr = PLAYER_RADIUS + radius;
        if (dx * dx + dz * dz > rr * rr) continue;
        if (p.y < v->y - radius || p.y > v->y + targetHeight(t) + radius) continue;
        return t;
    }
    return -1;
}

void updateProjectiles() {
    const float sdt = TICK_DT / PROJECTILE_SUBSTEPS;
    for (size_t i = 0; i < g_projectiles.size();) {
        Projectile& p = g_projectiles[i];
        const float radius = abilityInfo(p.ability).projRadius;
        bool done = false;
        for (int s = 0; s < PROJECTILE_SUBSTEPS && !done; ++s) {
            p.x += p.vx * sdt;
            p.y += p.vy * sdt;
            p.z += p.vz * sdt;
            if (pointInWall(p.x, p.y, p.z)) {
                // Back up so the impact effect isn't drawn inside the wall.
                p.x -= p.vx * sdt; p.y -= p.vy * sdt; p.z -= p.vz * sdt;
                explode(p, -1);
                done = true;
            } else if (int hit = projectileHitsTarget(p, radius); hit >= 0) {
                explode(p, hit);
                done = true;
            }
        }
        if (!done && --p.ttl <= 0) {
            explode(p, -1);
            done = true;
        }
        if (done) {
            g_projectiles[i] = g_projectiles.back();
            g_projectiles.pop_back();
        } else {
            ++i;
        }
    }
}

// --- Minions -----------------------------------------------------------------

void summonMinion(int owner) {
    killMinion(owner);  // only one at a time: a new one replaces the old
    const Client& c = g_clients[owner];
    Minion& m = g_minions[owner];
    m = Minion{};
    m.st.x = c.st.x;
    m.st.y = c.st.y;
    m.st.z = c.st.z;
    // Rise from the ground a step in front of the caster, if there's room.
    float fx = std::sin(c.st.yaw), fz = -std::cos(c.st.yaw);
    for (float d : {1.4f, 0.8f}) {
        float x = c.st.x + fx * d, z = c.st.z + fz * d;
        if (!pointInWall(x, c.st.y + 0.3f, z) && !pointInWall(x, c.st.y + MINION_HEIGHT, z) &&
            lineOfSight(c.st.x, c.st.y + 0.3f, c.st.z, x, c.st.y + 0.3f, z)) {
            m.st.x = x;
            m.st.z = z;
            break;
        }
    }
    m.st.y += 0.05f;
    m.st.yaw = c.st.yaw;
    m.st.alive = 1;
    m.active = true;
    m.hp = MINION_HP;
    m.ttl = MINION_LIFETIME_TICKS;
}

// Can someone at `from` see player `i`? Stealthed players are invisible
// unless very close.
bool canSee(const PlayerState& from, float eyeHeight, int i) {
    const Client& v = g_clients[i];
    if (!v.active || !v.st.alive) return false;
    float dx = v.st.x - from.x, dy = v.st.y - from.y, dz = v.st.z - from.z;
    float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (v.st.stealthTicks && d > STEALTH_REVEAL_DIST) return false;
    return lineOfSight(from.x, from.y + eyeHeight, from.z, v.st.x, centerY(v), v.st.z);
}

void updateMinion(int owner) {
    Minion& m = g_minions[owner];
    if (!m.active) return;
    if (--m.ttl <= 0 || m.st.y < WORLD_BOTTOM + 1.0f) {
        killMinion(owner);
        return;
    }
    if (m.attackCd) --m.attackCd;

    const AbilityInfo& claw = abilityInfo(AbilityId::SkeletonClaw);
    const float eye = MINION_HEIGHT - 0.2f;
    // Chase the nearest enemy it can see; otherwise trail behind its master.
    int target = -1;
    float best = MINION_SIGHT;
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (i == owner || !canSee(m.st, eye, i)) continue;
        const PlayerState& v = g_clients[i].st;
        float d = std::hypot(v.x - m.st.x, v.z - m.st.z);
        if (d < best) { best = d; target = i; }
    }
    InputCmd cmd{};
    cmd.yaw = m.st.yaw;
    float goalX = m.st.x, goalZ = m.st.z, goalDist = 0;
    if (target >= 0) {
        goalX = g_clients[target].st.x;
        goalZ = g_clients[target].st.z;
        goalDist = 1.2f;
    } else if (g_clients[owner].active && g_clients[owner].st.alive) {
        goalX = g_clients[owner].st.x;
        goalZ = g_clients[owner].st.z;
        goalDist = MINION_LEASH;
    }
    float dx = goalX - m.st.x, dz = goalZ - m.st.z;
    float dist = std::hypot(dx, dz);
    if (dist > 0.05f) cmd.yaw = std::atan2(dx, -dz);
    if (dist > goalDist) cmd.forward = 1;
    // Hop up ledges it walks into.
    if (cmd.forward && m.stuckTicks > 6 && m.st.onGround) cmd.buttons |= BTN_JUMP;

    float px = m.st.x, pz = m.st.z;
    simulateMove(m.st, cmd, MINION_SPEED, TICK_DT);
    bool moved = std::hypot(m.st.x - px, m.st.z - pz) > MINION_SPEED * TICK_DT * 0.3f;
    m.stuckTicks = cmd.forward && !moved ? static_cast<uint16_t>(m.stuckTicks + 1) : 0;

    if (target >= 0 && m.attackCd == 0 && inMeleeReach(m.st, eye, claw, target)) {
        m.attackCd = claw.cooldownTicks;
        pushEvent(EV_ATTACK, AbilityId::SkeletonClaw, -1, -1, 0, m.st.x, m.st.y, m.st.z);
        applyDamage(target, owner, AbilityId::SkeletonClaw, claw.amount);
    }
}

// Damages every non-friendly target within `radius` of a point (with line of
// sight), falling off to `edgeFrac` of `amount` at the edge.
void blast(int attacker, AbilityId id, float x, float y, float z, float radius, float amount, float edgeFrac) {
    for (int t = 0; t < NUM_TARGETS; ++t) {
        const PlayerState* v = targetState(t);
        if (!v || isFriendly(attacker, t)) continue;
        float cy = targetCenterY(t);
        float dx = v->x - x, dy = cy - y, dz = v->z - z;
        float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        float reach = radius + PLAYER_RADIUS;
        if (d > reach || !lineOfSight(x, y, z, v->x, cy, v->z)) continue;
        applyDamage(t, attacker, id, amount * (1.0f - (1.0f - edgeFrac) * d / reach));
    }
}

bool canSee(const PlayerState& from, float eyeHeight, int i);

// Rogue: appear behind the enemy you're aiming at. Returns false if there's
// nobody to step to (or no room behind them).
bool shadowstep(int idx) {
    Client& c = g_clients[idx];
    const AbilityInfo& ab = abilityInfo(AbilityId::Shadowstep);
    float dx, dy, dz;
    viewDir(c.st.yaw, c.st.pitch, dx, dy, dz);
    int best = -1;
    float bestDot = 0.93f;  // within ~20 degrees of the crosshair
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (i == idx || !canSee(c.st, EYE_HEIGHT, i)) continue;
        const PlayerState& v = g_clients[i].st;
        float tx = v.x - c.st.x, ty = v.y + PLAYER_HEIGHT * 0.5f - (c.st.y + EYE_HEIGHT), tz = v.z - c.st.z;
        float d = std::sqrt(tx * tx + ty * ty + tz * tz);
        if (d > ab.range || d < 0.5f) continue;
        float dot = (tx * dx + ty * dy + tz * dz) / d;
        if (dot > bestDot) { bestDot = dot; best = i; }
    }
    if (best < 0) return false;
    const PlayerState& v = g_clients[best].st;
    const float fx = std::sin(v.yaw), fz = -std::cos(v.yaw);  // victim's facing
    const float rx = -fz, rz = fx;
    // Straight behind, then behind to either side, then beside them.
    const float offs[][2] = {{-1.1f, 0}, {-0.8f, 0.8f}, {-0.8f, -0.8f}, {0, 1.1f}, {0, -1.1f}};
    for (const auto& o : offs) {
        float x = v.x + fx * o[0] + rx * o[1], z = v.z + fz * o[0] + rz * o[1];
        if (!spaceFree(x, v.y + 0.02f, z) ||
            !lineOfSight(v.x, v.y + PLAYER_HEIGHT * 0.5f, v.z, x, v.y + PLAYER_HEIGHT * 0.5f, z))
            continue;
        const bool stealthed = c.st.stealthTicks > 0;
        // A stealthed rogue's whereabouts aren't announced.
        if (!stealthed) pushEvent(EV_ATTACK, AbilityId::Shadowstep, idx, -1, 0, c.st.x, c.st.y, c.st.z);
        c.st.x = x;
        c.st.y = v.y + 0.02f;
        c.st.z = z;
        c.st.vx = c.st.vy = c.st.vz = 0;
        c.st.dashTicks = 0;
        c.st.yaw = std::atan2(v.x - x, -(v.z - z));
        if (stealthed) pushEvent(EV_TELEPORT, AbilityId::Shadowstep, idx, -1, c.st.yaw * 1000.0f, 0, 0, 0);
        else pushEvent(EV_TELEPORT, AbilityId::Shadowstep, idx, best, c.st.yaw * 1000.0f, x, c.st.y, z);
        return true;
    }
    return false;
}

// Necromancer: blow up your skeleton. Returns false if you don't have one.
bool boneBurst(int idx) {
    Minion& m = g_minions[idx];
    if (!m.active) return false;
    const Client& c = g_clients[idx];
    const AbilityInfo& ab = abilityInfo(AbilityId::BoneBurst);
    float x = m.st.x, y = m.st.y + MINION_HEIGHT * 0.5f, z = m.st.z;
    pushEvent(EV_ATTACK, AbilityId::BoneBurst, idx, -1, 0, c.st.x, c.st.y, c.st.z);
    pushEvent(EV_IMPACT, AbilityId::BoneBurst, idx, -1, 0, x, y, z);
    killMinion(idx);
    blast(idx, AbilityId::BoneBurst, x, y, z, ab.range, ab.amount, 0.5f);
    return true;
}

void useAbility(int idx, int slot) {
    Client& c = g_clients[idx];
    AbilityId id = classInfo(c.cls).abilities[slot];
    const AbilityInfo& ab = abilityInfo(id);
    if (ab.kind == AbilityKind::Shadowstep || ab.kind == AbilityKind::Detonate) {
        bool ok = ab.kind == AbilityKind::Shadowstep ? shadowstep(idx) : boneBurst(idx);
        if (!ok) c.st.cooldown[slot] = 0;  // nothing happened: give the cooldown back
        return;
    }
    pushEvent(EV_ATTACK, id, idx, -1, 0, c.st.x, c.st.y, c.st.z);
    switch (ab.kind) {
        case AbilityKind::Melee: doMelee(idx, id); break;
        case AbilityKind::Projectile: spawnProjectile(idx, id); break;
        case AbilityKind::Dash: break;  // movement handled in simulate()
        case AbilityKind::SelfHeal: applyHeal(idx, id, ab.amount, true); break;
        case AbilityKind::HealOverTime:
            c.hotTicks = ab.durationTicks;
            c.hotPerTick = ab.amount / ab.durationTicks;
            pushEvent(EV_HEAL, id, idx, idx, 0, c.st.x, centerY(c), c.st.z);
            break;
        case AbilityKind::Stealth: break;  // simulate() sets stealthTicks
        case AbilityKind::Summon: summonMinion(idx); break;
        case AbilityKind::Blink: break;  // movement handled in simulate()
        case AbilityKind::Nova:
            blast(idx, id, c.st.x, centerY(c), c.st.z, ab.range, ab.amount, 1.0f);
            applyHeal(idx, id, ab.selfHeal, true);
            break;
        case AbilityKind::Shadowstep:
        case AbilityKind::Detonate: break;  // handled above
    }
}

// --- Spawning ---------------------------------------------------------------

// Picks the spawn point furthest from every living player.
void placeAtSpawn(int idx) {
    Client& c = g_clients[idx];
    int best = 0;
    float bestDist = -1;
    for (int s = 0; s < spawnCount(); ++s) {
        float sx, sy, sz;
        spawnPoint(s, sx, sy, sz);
        float nearest = 1e9f;
        for (int i = 0; i < MAX_PLAYERS; ++i) {
            const Client& o = g_clients[i];
            if (i == idx || !o.active || !o.st.alive) continue;
            float dx = o.st.x - sx, dy = o.st.y - sy, dz = o.st.z - sz;
            nearest = std::min(nearest, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        // Small random jitter so ties don't always resolve to the same spawn.
        nearest += static_cast<float>(std::rand() % 100) * 0.01f;
        if (nearest > bestDist) { bestDist = nearest; best = s; }
    }
    c.st = PlayerState{};
    spawnPoint(best, c.st.x, c.st.y, c.st.z);
    c.st.onGround = 1;
    c.st.alive = 1;
    // Face the middle of the map so nobody spawns staring at a wall.
    float dx = mapWidth() * TILE * 0.5f - c.st.x, dz = mapHeight() * TILE * 0.5f - c.st.z;
    c.st.yaw = std::atan2(dx, -dz);
    c.hp = classInfo(c.cls).maxHp;
    c.hotTicks = 0;
}

// --- Messages ---------------------------------------------------------------

MsgPlayerInfo playerInfo(int i) {
    MsgPlayerInfo m{};
    m.type = MSG_PLAYER_INFO;
    m.id = static_cast<uint8_t>(i);
    m.cls = g_clients[i].picking ? NO_CLASS : static_cast<uint8_t>(g_clients[i].cls);
    std::strncpy(m.name, g_clients[i].name.c_str(), sizeof(m.name) - 1);
    return m;
}

void handleHello(ENetPeer* peer, const uint8_t* data, size_t len) {
    if (slotOf(peer) >= 0 || len < sizeof(MsgHello)) return;
    MsgHello hello;
    std::memcpy(&hello, data, sizeof hello);

    MsgReject reject{MSG_REJECT, 0};
    if (hello.version != PROTOCOL_VERSION) reject.reason = REJECT_VERSION;

    int slot = -1;
    for (int i = 0; i < MAX_PLAYERS && reject.reason == 0; ++i)
        if (!g_clients[i].active) { slot = i; break; }
    if (slot < 0 && reject.reason == 0) reject.reason = REJECT_FULL;

    if (reject.reason) {
        sendTo(peer, &reject, sizeof reject, CH_RELIABLE, true);
        enet_peer_disconnect_later(peer, 0);
        return;
    }

    Client& c = g_clients[slot];
    c = Client{};
    c.active = true;
    c.peer = peer;
    c.picking = hello.cls >= static_cast<uint8_t>(PlayerClass::Count);
    c.cls = c.picking ? PlayerClass::Warrior : static_cast<PlayerClass>(hello.cls);
    hello.name[sizeof(hello.name) - 1] = 0;
    c.name = hello.name;
    placeAtSpawn(slot);
    if (c.picking) {  // watch from a spawn point until a class is chosen
        c.st.alive = 0;
        c.respawnTick = UINT32_MAX;
    }
    peer->data = reinterpret_cast<void*>(static_cast<intptr_t>(slot + 1));

    // Welcome must go first: the client treats it as "joined".
    MsgWelcome w{};
    w.type = MSG_WELCOME;
    w.playerId = static_cast<uint8_t>(slot);
    w.serverTick = g_tick;
    std::strncpy(w.mapName, mapName().c_str(), sizeof(w.mapName) - 1);
    w.mapHash = mapHash();
    sendTo(peer, &w, sizeof w, CH_RELIABLE, true);
    MsgPlayerInfo mine = playerInfo(slot);
    broadcast(&mine, sizeof mine, CH_RELIABLE, true);
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (i == slot || !g_clients[i].active) continue;
        MsgPlayerInfo other = playerInfo(i);
        sendTo(peer, &other, sizeof other, CH_RELIABLE, true);
    }
    std::printf("[join] #%d '%s' as %s\n", slot, c.name.c_str(), c.picking ? "(choosing)" : classInfo(c.cls).name);
}

void setClass(int idx, PlayerClass cls) {
    Client& c = g_clients[idx];
    c.cls = cls;
    c.nextCls = NO_CLASS;
    MsgPlayerInfo info = playerInfo(idx);
    broadcast(&info, sizeof info, CH_RELIABLE, true);
    std::printf("[class] '%s' is now %s\n", c.name.c_str(), classInfo(cls).name);
}

void handleChangeClass(ENetPeer* peer, const uint8_t* data, size_t len) {
    int idx = slotOf(peer);
    if (idx < 0 || len < sizeof(MsgChangeClass)) return;
    MsgChangeClass m;
    std::memcpy(&m, data, sizeof m);
    if (m.cls >= static_cast<uint8_t>(PlayerClass::Count)) return;
    Client& c = g_clients[idx];
    PlayerClass cls = static_cast<PlayerClass>(m.cls);
    if (c.picking) {
        // First pick: spawn right away.
        c.picking = false;
        setClass(idx, cls);
        placeAtSpawn(idx);
        pushEvent(EV_RESPAWN, AbilityId::Count, -1, idx, 0, c.st.x, c.st.y, c.st.z);
    } else if (!c.st.alive) {
        setClass(idx, cls);  // dead: you'll respawn as it
    } else {
        c.nextCls = cls == c.cls ? NO_CLASS : m.cls;  // alive: wait for the next respawn
    }
}

void handleInput(ENetPeer* peer, const uint8_t* data, size_t len) {
    int idx = slotOf(peer);
    if (idx < 0 || len < MSG_INPUT_HEADER) return;
    Client& c = g_clients[idx];
    uint8_t count = data[1];
    if (count > INPUT_REDUNDANCY || len < MSG_INPUT_HEADER + count * sizeof(InputCmd)) return;

    for (int i = 0; i < count; ++i) {
        InputCmd cmd;
        std::memcpy(&cmd, data + MSG_INPUT_HEADER + i * sizeof(InputCmd), sizeof cmd);
        if (cmd.seq <= c.lastSeq) continue;  // already applied (redundant copy)
        if (!std::isfinite(cmd.yaw) || !std::isfinite(cmd.pitch)) continue;
        // TODO: anti-speedhack. Right now a client can send more cmds than real
        // time allows (which also speeds up its cooldowns). Fix by buffering
        // cmds and consuming at most one per tick (with a small catch-up budget).
        uint8_t fired = simulate(c.st, cmd, c.cls, TICK_DT);
        c.lastSeq = cmd.seq;
        for (int slot = 0; slot < NUM_ABILITY_SLOTS; ++slot)
            if (fired & (1 << slot)) useAbility(idx, slot);
    }
}

// Each client gets its own snapshot so stealthed players' positions are only
// sent to those allowed to see them (otherwise a modified client could).
void sendSnapshot(int recipient) {
    const Client& me = g_clients[recipient];
    std::vector<uint8_t> buf(sizeof(MsgSnapshotHeader));
    MsgSnapshotHeader hdr{MSG_SNAPSHOT, g_tick, 0, 0, 0};
    auto append = [&](const void* src, size_t n) {
        size_t off = buf.size();
        buf.resize(off + n);
        std::memcpy(buf.data() + off, src, n);
    };
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        const Client& c = g_clients[i];
        if (!c.active) continue;
        NetPlayer np{};
        np.id = static_cast<uint8_t>(i);
        np.cls = c.picking ? NO_CLASS : static_cast<uint8_t>(c.cls);
        np.nextCls = c.nextCls;
        np.hp = static_cast<int16_t>(std::ceil(c.hp));
        np.kills = c.kills;
        np.deaths = c.deaths;
        np.score = c.score;
        np.streak = c.streak;
        np.ping = static_cast<uint16_t>(std::min<enet_uint32>(c.peer->roundTripTime, 65535));
        np.lastInputSeq = c.lastSeq;
        np.s = c.st;
        if (i != recipient && c.st.alive && c.st.stealthTicks) {
            float dx = c.st.x - me.st.x, dy = c.st.y - me.st.y, dz = c.st.z - me.st.z;
            if (!me.st.alive || std::sqrt(dx * dx + dy * dy + dz * dz) > STEALTH_REVEAL_DIST) {
                np.hidden = 1;
                np.s = PlayerState{};
                np.s.alive = 1;
                np.s.stealthTicks = 1;
            }
        }
        append(&np, sizeof np);
        hdr.playerCount++;
    }
    for (const Projectile& p : g_projectiles) {
        if (hdr.projCount == 255) break;
        NetProjectile np{p.id, static_cast<uint8_t>(p.ability), p.owner, p.x, p.y, p.z, p.vx, p.vy, p.vz};
        append(&np, sizeof np);
        hdr.projCount++;
    }
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        const Minion& m = g_minions[i];
        if (!m.active) continue;
        NetMinion nm{};
        nm.owner = static_cast<uint8_t>(i);
        nm.hpPct = static_cast<uint8_t>(std::clamp(std::ceil(m.hp / MINION_HP * 100.0f), 0.0f, 100.0f));
        nm.x = m.st.x; nm.y = m.st.y; nm.z = m.st.z;
        nm.yaw = m.st.yaw;
        append(&nm, sizeof nm);
        hdr.minionCount++;
    }
    std::memcpy(buf.data(), &hdr, sizeof hdr);
    sendTo(me.peer, buf.data(), buf.size(), CH_UNRELIABLE, false);
}

void broadcastSnapshot() {
    for (int i = 0; i < MAX_PLAYERS; ++i)
        if (g_clients[i].active) sendSnapshot(i);
}

void tick() {
    ++g_tick;
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        Client& c = g_clients[i];
        if (!c.active) continue;
        if (!c.st.alive && g_tick >= c.respawnTick) {
            if (c.nextCls != NO_CLASS) setClass(i, static_cast<PlayerClass>(c.nextCls));
            placeAtSpawn(i);
            pushEvent(EV_RESPAWN, AbilityId::Count, -1, i, 0, c.st.x, c.st.y, c.st.z);
        }
        if (c.st.alive && c.hotTicks) {
            applyHeal(i, AbilityId::Regrowth, c.hotPerTick, false);
            --c.hotTicks;
        }
    }
    for (int i = 0; i < MAX_PLAYERS; ++i) updateMinion(i);
    updateProjectiles();
    if (g_tick % SNAPSHOT_EVERY == 0) {
        broadcastSnapshot();
        flushEvents();
    }
}

void handleEvent(const ENetEvent& ev) {
    switch (ev.type) {
        case ENET_EVENT_TYPE_CONNECT: {
            char ip[64];
            enet_address_get_host_ip(&ev.peer->address, ip, sizeof ip);
            std::printf("[connect] %s:%u\n", ip, ev.peer->address.port);
            ev.peer->data = nullptr;
            break;
        }
        case ENET_EVENT_TYPE_RECEIVE: {
            const uint8_t* d = ev.packet->data;
            size_t n = ev.packet->dataLength;
            if (n > 0) {
                switch (d[0]) {
                    case MSG_HELLO: handleHello(ev.peer, d, n); break;
                    case MSG_INPUT: handleInput(ev.peer, d, n); break;
                    case MSG_CHANGE_CLASS: handleChangeClass(ev.peer, d, n); break;
                    default: break;
                }
            }
            enet_packet_destroy(ev.packet);
            break;
        }
        case ENET_EVENT_TYPE_DISCONNECT: {
            int idx = slotOf(ev.peer);
            if (idx >= 0) {
                std::printf("[leave] '%s'\n", g_clients[idx].name.c_str());
                killMinion(idx);
                g_clients[idx].active = false;
            }
            ev.peer->data = nullptr;
            break;
        }
        default: break;
    }
}

}  // namespace

int main(int argc, char** argv) {
    uint16_t port = DEFAULT_PORT;
    std::string mapArg = "arena";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--map" && i + 1 < argc) mapArg = argv[++i];
        else {
            std::fprintf(stderr, "usage: boyz_server [--port P] [--map NAME|PATH]\n");
            return 1;
        }
    }
    std::string mapPath = findMapFile(mapArg, argv[0]);
    std::string mapErr;
    if (mapPath.empty()) {
        std::fprintf(stderr, "map '%s' not found (looked in ./maps and next to the executable)\n", mapArg.c_str());
        return 1;
    }
    if (!loadMap(mapPath, mapErr)) {
        std::fprintf(stderr, "failed to load map: %s\n", mapErr.c_str());
        return 1;
    }
    if (mapName().size() >= MAP_NAME_LEN) {
        std::fprintf(stderr, "map name too long (max %zu chars)\n", MAP_NAME_LEN - 1);
        return 1;
    }

    if (enet_initialize() != 0) {
        std::fprintf(stderr, "enet_initialize failed\n");
        return 1;
    }
    ENetAddress addr{};
    addr.host = ENET_HOST_ANY;
    addr.port = port;
    g_host = enet_host_create(&addr, MAX_PLAYERS + 4, NUM_CHANNELS, 0, 0);
    if (!g_host) {
        std::fprintf(stderr, "could not bind UDP port %u\n", port);
        return 1;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::srand(static_cast<unsigned>(std::time(nullptr)));
    std::printf("boyz_server listening on UDP %u (map '%s' %dx%d, %d spawns, %d Hz)\n", port, mapName().c_str(),
                mapWidth(), mapHeight(), spawnCount(), TICK_RATE);
    std::fflush(stdout);

    using clock = std::chrono::steady_clock;
    const auto tickDur = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(TICK_DT));
    auto next = clock::now() + tickDur;

    while (g_running) {
        auto now = clock::now();
        enet_uint32 waitMs = 0;
        if (next > now)
            waitMs = static_cast<enet_uint32>(std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count());

        ENetEvent ev;
        if (enet_host_service(g_host, &ev, waitMs) > 0) {
            do { handleEvent(ev); } while (enet_host_check_events(g_host, &ev) > 0);
        }

        while (clock::now() >= next) {
            tick();
            next += tickDur;
        }
        std::fflush(stdout);
    }

    std::printf("shutting down\n");
    for (auto& c : g_clients)
        if (c.active) enet_peer_disconnect_now(c.peer, 0);
    enet_host_destroy(g_host);
    enet_deinitialize();
    return 0;
}
