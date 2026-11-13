// Authoritative dedicated server. Owns all game state; clients only send input.
#include <enet/enet.h>

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "game.h"
#include "protocol.h"

using namespace game;

namespace {

volatile std::sig_atomic_t g_running = 1;
void onSignal(int) { g_running = 0; }

struct Client {
    bool active = false;
    ENetPeer* peer = nullptr;
    PlayerClass cls = PlayerClass::Warrior;
    std::string name;
    PlayerState st{};
    uint32_t lastSeq = 0;
    float hp = 0;
};

Client g_clients[MAX_PLAYERS];
int g_spawnCounter = 0;

void sendTo(ENetPeer* peer, const void* data, size_t len, int channel, bool reliable) {
    ENetPacket* p = enet_packet_create(data, len, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    enet_peer_send(peer, static_cast<enet_uint8>(channel), p);
}

Client* clientOf(ENetPeer* peer) {
    auto slot = reinterpret_cast<intptr_t>(peer->data);
    if (slot <= 0 || slot > MAX_PLAYERS) return nullptr;
    Client* c = &g_clients[slot - 1];
    return c->active ? c : nullptr;
}

void handleHello(ENetPeer* peer, const uint8_t* data, size_t len, uint32_t tick) {
    if (clientOf(peer) || len < sizeof(MsgHello)) return;
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
    spawnPoint(g_spawnCounter++, c.st.x, c.st.z);
    c.st.onGround = 1;
    // Face the middle of the map so nobody spawns staring at a wall.
    float dx = mapWidth() * TILE * 0.5f - c.st.x, dz = mapHeight() * TILE * 0.5f - c.st.z;
    c.st.yaw = std::atan2(dx, -dz);
    c.hp = classInfo(c.cls).maxHp;
    peer->data = reinterpret_cast<void*>(static_cast<intptr_t>(slot + 1));

    MsgWelcome w{MSG_WELCOME, static_cast<uint8_t>(slot), tick};
    sendTo(peer, &w, sizeof w, CH_RELIABLE, true);
    std::printf("[join] #%d '%s' as %s\n", slot, c.name.c_str(), classInfo(c.cls).name);
}

void handleInput(ENetPeer* peer, const uint8_t* data, size_t len) {
    Client* c = clientOf(peer);
    if (!c || len < MSG_INPUT_HEADER) return;
    uint8_t count = data[1];
    if (count > INPUT_REDUNDANCY || len < MSG_INPUT_HEADER + count * sizeof(InputCmd)) return;

    for (int i = 0; i < count; ++i) {
        InputCmd cmd;
        std::memcpy(&cmd, data + MSG_INPUT_HEADER + i * sizeof(InputCmd), sizeof cmd);
        if (cmd.seq <= c->lastSeq) continue;  // already applied (redundant copy)
        if (!std::isfinite(cmd.yaw) || !std::isfinite(cmd.pitch)) continue;
        // TODO: anti-speedhack. Right now a client can send more cmds than real
        // time allows. Fix by buffering cmds and consuming at most one per tick
        // (with a small catch-up budget).
        simulate(c->st, cmd, c->cls, TICK_DT);
        c->lastSeq = cmd.seq;
    }
}

void broadcastSnapshot(ENetHost* host, uint32_t tick) {
    std::vector<uint8_t> buf(sizeof(MsgSnapshotHeader));
    MsgSnapshotHeader hdr{MSG_SNAPSHOT, tick, 0};
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        const Client& c = g_clients[i];
        if (!c.active) continue;
        NetPlayer np{};
        np.id = static_cast<uint8_t>(i);
        np.cls = static_cast<uint8_t>(c.cls);
        np.hp = static_cast<int16_t>(c.hp);
        np.lastInputSeq = c.lastSeq;
        np.s = c.st;
        size_t off = buf.size();
        buf.resize(off + sizeof np);
        std::memcpy(buf.data() + off, &np, sizeof np);
        hdr.count++;
    }
    std::memcpy(buf.data(), &hdr, sizeof hdr);
    ENetPacket* p = enet_packet_create(buf.data(), buf.size(), 0);
    enet_host_broadcast(host, CH_UNRELIABLE, p);
}

void handleEvent(const ENetEvent& ev, uint32_t tick) {
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
                    case MSG_HELLO: handleHello(ev.peer, d, n, tick); break;
                    case MSG_INPUT: handleInput(ev.peer, d, n); break;
                    default: break;
                }
            }
            enet_packet_destroy(ev.packet);
            break;
        }
        case ENET_EVENT_TYPE_DISCONNECT: {
            if (Client* c = clientOf(ev.peer)) {
                std::printf("[leave] '%s'\n", c->name.c_str());
                c->active = false;
            }
            ev.peer->data = nullptr;
            break;
        }
        default: break;
    }
}

}  // namespace

int main(int argc, char** argv) {
    uint16_t port = argc > 1 ? static_cast<uint16_t>(std::atoi(argv[1])) : DEFAULT_PORT;

    if (enet_initialize() != 0) {
        std::fprintf(stderr, "enet_initialize failed\n");
        return 1;
    }
    ENetAddress addr{};
    addr.host = ENET_HOST_ANY;
    addr.port = port;
    ENetHost* host = enet_host_create(&addr, MAX_PLAYERS + 4, NUM_CHANNELS, 0, 0);
    if (!host) {
        std::fprintf(stderr, "could not bind UDP port %u\n", port);
        return 1;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::printf("boyz_server listening on UDP %u (map %dx%d, %d spawns, %d Hz)\n", port, mapWidth(),
                mapHeight(), spawnCount(), TICK_RATE);
    std::fflush(stdout);

    using clock = std::chrono::steady_clock;
    const auto tickDur = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(TICK_DT));
    auto next = clock::now() + tickDur;
    uint32_t tick = 0;

    while (g_running) {
        auto now = clock::now();
        enet_uint32 waitMs = 0;
        if (next > now)
            waitMs = static_cast<enet_uint32>(std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count());

        ENetEvent ev;
        if (enet_host_service(host, &ev, waitMs) > 0) {
            do { handleEvent(ev, tick); } while (enet_host_check_events(host, &ev) > 0);
        }

        while (clock::now() >= next) {
            ++tick;
            // TODO: per-tick game logic goes here (projectiles, DoTs, cooldowns, respawns).
            if (tick % SNAPSHOT_EVERY == 0) broadcastSnapshot(host, tick);
            next += tickDur;
        }
        std::fflush(stdout);
    }

    std::printf("shutting down\n");
    for (auto& c : g_clients)
        if (c.active) enet_peer_disconnect_now(c.peer, 0);
    enet_host_destroy(host);
    enet_deinitialize();
    return 0;
}
