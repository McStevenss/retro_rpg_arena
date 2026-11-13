// Wire format. Structs are memcpy'd directly, which assumes both ends are
// little-endian and built with the same compiler ABI (true for x86/ARM PCs).
// Replace with an explicit serializer before shipping cross-platform.
#pragma once
#include <cstddef>
#include <cstdint>

#include "game.h"

namespace game {

constexpr uint32_t PROTOCOL_VERSION = 4;
constexpr int NUM_CHANNELS = 2;
constexpr int CH_RELIABLE = 0;    // join info, gameplay events
constexpr int CH_UNRELIABLE = 1;  // inputs and snapshots, newest wins
constexpr int INPUT_REDUNDANCY = 4;  // each input packet repeats the last N cmds
constexpr uint8_t NO_PLAYER = 255;

enum MsgType : uint8_t {
    MSG_HELLO = 1,     // C->S
    MSG_WELCOME,       // S->C
    MSG_REJECT,        // S->C
    MSG_INPUT,         // C->S
    MSG_SNAPSHOT,      // S->C
    MSG_PLAYER_INFO,   // S->C  name/class of a player (reliable)
    MSG_EVENTS,        // S->C  batch of gameplay events (reliable)
};

enum RejectReason : uint8_t { REJECT_FULL = 1, REJECT_VERSION = 2 };
constexpr size_t MAP_NAME_LEN = 32;

enum EventType : uint8_t {
    EV_ATTACK = 1,  // a used ability
    EV_IMPACT,      // projectile ended at x,y,z (b = player hit directly or NO_PLAYER)
    EV_DAMAGE,      // a damaged b by amount
    EV_HEAL,        // b healed by amount
    EV_KILL,        // a killed b (a == b or NO_PLAYER for suicide)
    EV_RESPAWN,     // b respawned
};

#pragma pack(push, 1)
struct MsgHello {
    uint8_t type;
    uint32_t version;
    uint8_t cls;
    char name[16];
};

struct MsgWelcome {
    uint8_t type;
    uint8_t playerId;
    uint32_t serverTick;
    char mapName[MAP_NAME_LEN];  // client loads maps/<mapName>.map
    uint32_t mapHash;            // ...and checks it matches the server's file
};

struct MsgReject {
    uint8_t type;
    uint8_t reason;
};

struct MsgPlayerInfo {
    uint8_t type;
    uint8_t id;
    uint8_t cls;
    char name[16];
};

struct MsgInput {
    uint8_t type;
    uint8_t count;                     // cmds actually sent, oldest first
    InputCmd cmds[INPUT_REDUNDANCY];
};

struct NetPlayer {
    uint8_t id;
    uint8_t cls;
    int16_t hp;
    uint16_t kills;
    uint16_t deaths;
    uint16_t ping;          // round-trip time to the server, ms
    uint32_t lastInputSeq;  // last input the server applied for this player
    PlayerState s;
};

struct NetProjectile {
    uint16_t id;
    uint8_t ability;
    uint8_t owner;
    float x, y, z;
    float vx, vy, vz;
};

// Followed by playerCount NetPlayer, then projCount NetProjectile.
struct MsgSnapshotHeader {
    uint8_t type;
    uint32_t serverTick;
    uint8_t playerCount;
    uint8_t projCount;
};

struct NetEvent {
    uint8_t type;
    uint8_t ability;
    uint8_t a;
    uint8_t b;
    int16_t amount;
    float x, y, z;
};

// Followed by count NetEvent.
struct MsgEventsHeader {
    uint8_t type;
    uint8_t count;
};
#pragma pack(pop)

constexpr size_t MSG_INPUT_HEADER = offsetof(MsgInput, cmds);

}  // namespace game
