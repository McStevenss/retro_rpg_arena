// Wire format. Structs are memcpy'd directly, which assumes both ends are
// little-endian and built with the same compiler ABI (true for x86/ARM PCs).
// Replace with an explicit serializer before shipping cross-platform.
#pragma once
#include <cstddef>
#include <cstdint>

#include "game.h"

namespace game {

constexpr uint32_t PROTOCOL_VERSION = 1;
constexpr int NUM_CHANNELS = 2;
constexpr int CH_RELIABLE = 0;    // join/leave, chat, events that must arrive
constexpr int CH_UNRELIABLE = 1;  // inputs and snapshots, newest wins
constexpr int INPUT_REDUNDANCY = 4;  // each input packet repeats the last N cmds

enum MsgType : uint8_t {
    MSG_HELLO = 1,   // C->S
    MSG_WELCOME,     // S->C
    MSG_REJECT,      // S->C
    MSG_INPUT,       // C->S
    MSG_SNAPSHOT,    // S->C
};

enum RejectReason : uint8_t { REJECT_FULL = 1, REJECT_VERSION = 2 };

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
};

struct MsgReject {
    uint8_t type;
    uint8_t reason;
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
    uint32_t lastInputSeq;  // last input the server applied for this player
    PlayerState s;
};

struct MsgSnapshotHeader {
    uint8_t type;
    uint32_t serverTick;
    uint8_t count;  // followed by `count` NetPlayer
};
#pragma pack(pop)

constexpr size_t MSG_INPUT_HEADER = offsetof(MsgInput, cmds);

}  // namespace game
