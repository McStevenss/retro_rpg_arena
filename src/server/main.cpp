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
    std::string name;
    PlayerState st{};
    uint32_t lastSeq = 0;
    float hp = 0;
    uint16_t hotTicks = 0;      // heal-over-time remaining
    float hotPerTick = 0;
    uint32_t respawnTick = 0;
    uint16_t kills = 0, deaths = 0;
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

void applyDamage(int victim, int attacker, AbilityId ab, float amount) {
    Client& v = g_clients[victim];
    if (!v.active || !v.st.alive || amount <= 0) return;
    v.hp -= amount;
    pushEvent(EV_DAMAGE, ab, attacker, victim, amount, v.st.x, centerY(v), v.st.z);
    if (v.hp > 0) return;

    v.hp = 0;
    v.st.alive = 0;
    v.st.dashTicks = 0;
    v.hotTicks = 0;
    v.deaths++;
    v.respawnTick = g_tick + RESPAWN_TICKS;
    if (attacker >= 0 && attacker != victim && g_clients[attacker].active) g_clients[attacker].kills++;
    pushEvent(EV_KILL, ab, attacker, victim, 0, v.st.x, v.st.y, v.st.z);
    std::printf("[kill] %s -> %s (%s)\n", nameOf(attacker), v.name.c_str(), abilityInfo(ab).name);
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

// Hits everyone alive in a cone in front of the attacker. Uses current
// positions: no lag compensation yet (fine on LAN, noticeable at high ping).
void doMelee(int attacker, AbilityId id) {
    const AbilityInfo& ab = abilityInfo(id);
    const Client& a = g_clients[attacker];
    const float fx = std::sin(a.st.yaw), fz = -std::cos(a.st.yaw);
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        const Client& v = g_clients[i];
        if (i == attacker || !v.active || !v.st.alive) continue;
        float dx = v.st.x - a.st.x, dz = v.st.z - a.st.z;
        float dist = std::sqrt(dx * dx + dz * dz);
        if (dist > ab.range + PLAYER_RADIUS) continue;
        if (std::fabs(v.st.y - a.st.y) > MELEE_VERTICAL_REACH) continue;
        if (dist > 0.3f && (dx * fx + dz * fz) / dist < ab.arcCos) continue;
        if (!lineOfSight(a.st.x, a.st.y + EYE_HEIGHT, a.st.z, v.st.x, centerY(v), v.st.z)) continue;
        applyDamage(i, attacker, id, ab.amount);
    }
}

void explode(const Projectile& p, int directVictim) {
    const AbilityInfo& ab = abilityInfo(p.ability);
    pushEvent(EV_IMPACT, p.ability, p.owner, directVictim, 0, p.x, p.y, p.z);
    if (directVictim >= 0) {
        applyDamage(directVictim, p.owner, p.ability, ab.amount);
        Client& v = g_clients[directVictim];
        if (ab.slowTicks && v.st.alive) v.st.slowTicks = std::max(v.st.slowTicks, ab.slowTicks);
    }
    if (ab.splashRadius <= 0) return;
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        const Client& v = g_clients[i];
        if (i == p.owner || i == directVictim || !v.active || !v.st.alive) continue;
        float dx = v.st.x - p.x, dy = centerY(v) - p.y, dz = v.st.z - p.z;
        float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        float reach = ab.splashRadius + PLAYER_RADIUS;
        if (d > reach || !lineOfSight(p.x, p.y, p.z, v.st.x, centerY(v), v.st.z)) continue;
        applyDamage(i, p.owner, p.ability, ab.splashDamage * (1.0f - 0.5f * d / reach));
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

// Returns index of the player the projectile overlaps, or -1.
int projectileHitsPlayer(const Projectile& p, float radius) {
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        const Client& v = g_clients[i];
        if (i == p.owner || !v.active || !v.st.alive) continue;
        float dx = v.st.x - p.x, dz = v.st.z - p.z;
        float rr = PLAYER_RADIUS + radius;
        if (dx * dx + dz * dz > rr * rr) continue;
        if (p.y < v.st.y - radius || p.y > v.st.y + PLAYER_HEIGHT + radius) continue;
        return i;
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
            } else if (int hit = projectileHitsPlayer(p, radius); hit >= 0) {
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

void useAbility(int idx, int slot) {
    Client& c = g_clients[idx];
    AbilityId id = classInfo(c.cls).abilities[slot];
    const AbilityInfo& ab = abilityInfo(id);
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
    m.cls = static_cast<uint8_t>(g_clients[i].cls);
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
    c.cls = hello.cls < static_cast<uint8_t>(PlayerClass::Count) ? static_cast<PlayerClass>(hello.cls)
                                                                 : PlayerClass::Warrior;
    hello.name[sizeof(hello.name) - 1] = 0;
    c.name = hello.name;
    placeAtSpawn(slot);
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
    std::printf("[join] #%d '%s' as %s\n", slot, c.name.c_str(), classInfo(c.cls).name);
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

void broadcastSnapshot() {
    std::vector<uint8_t> buf(sizeof(MsgSnapshotHeader));
    MsgSnapshotHeader hdr{MSG_SNAPSHOT, g_tick, 0, 0};
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
        np.cls = static_cast<uint8_t>(c.cls);
        np.hp = static_cast<int16_t>(std::ceil(c.hp));
        np.kills = c.kills;
        np.deaths = c.deaths;
        np.lastInputSeq = c.lastSeq;
        np.s = c.st;
        append(&np, sizeof np);
        hdr.playerCount++;
    }
    for (const Projectile& p : g_projectiles) {
        if (hdr.projCount == 255) break;
        NetProjectile np{p.id, static_cast<uint8_t>(p.ability), p.owner, p.x, p.y, p.z, p.vx, p.vy, p.vz};
        append(&np, sizeof np);
        hdr.projCount++;
    }
    std::memcpy(buf.data(), &hdr, sizeof hdr);
    broadcast(buf.data(), buf.size(), CH_UNRELIABLE, false);
}

void tick() {
    ++g_tick;
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        Client& c = g_clients[i];
        if (!c.active) continue;
        if (!c.st.alive && g_tick >= c.respawnTick) {
            placeAtSpawn(i);
            pushEvent(EV_RESPAWN, AbilityId::Count, -1, i, 0, c.st.x, c.st.y, c.st.z);
        }
        if (c.st.alive && c.hotTicks) {
            applyHeal(i, AbilityId::Regrowth, c.hotPerTick, false);
            --c.hotTicks;
        }
    }
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
