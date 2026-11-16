// Client: renders, reads input, predicts own movement, interpolates others.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <enet/enet.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "font.h"
#include "game.h"
#include "gl.h"
#include "protocol.h"
#include "render.h"
#include "textures.h"

using namespace game;

namespace {

constexpr int RETRO_W = 400;  // internal render resolution (16:9 take on 320x200)
constexpr int RETRO_H = 225;
constexpr double INTERP_DELAY = 0.1;  // render remote players 100 ms in the past
constexpr float MOUSE_SENS = 0.0025f;

struct Options {
    std::string host = "127.0.0.1";
    uint16_t port = DEFAULT_PORT;
    std::string name = "player";
    PlayerClass cls = PlayerClass::Warrior;
    bool clsGiven = false;  // no --class: pick one in game
};

glm::vec3 classColor(PlayerClass c) {
    switch (c) {
        case PlayerClass::Warrior: return {0.85f, 0.3f, 0.25f};
        case PlayerClass::Mage: return {0.35f, 0.45f, 1.0f};
        case PlayerClass::Druid: return {0.4f, 0.8f, 0.3f};
        case PlayerClass::Healer: return {1.0f, 0.95f, 0.75f};
        case PlayerClass::Rogue: return {0.95f, 0.8f, 0.2f};
        case PlayerClass::Necromancer: return {0.65f, 0.3f, 0.9f};
        default: return {1, 1, 1};
    }
}

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        bool hasVal = i + 1 < argc;
        if (a == "--host" && hasVal) o.host = argv[++i];
        else if (a == "--port" && hasVal) o.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--name" && hasVal) o.name = argv[++i];
        else if (a == "--class" && hasVal) {
            o.clsGiven = true;
            if (!parseClass(argv[++i], o.cls)) {
                std::fprintf(stderr, "unknown class '%s' (warrior, mage, druid, healer, rogue, necromancer)\n", argv[i]);
                return false;
            }
        } else {
            std::fprintf(stderr,
                         "usage: boyz_client [--host H] [--port P] [--name N] "
                         "[--class warrior|mage|druid|healer|rogue|necromancer]\n");
            return false;
        }
    }
    return true;
}

void sendTo(ENetPeer* peer, const void* data, size_t len, int channel, bool reliable) {
    ENetPacket* p = enet_packet_create(data, len, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    enet_peer_send(peer, static_cast<enet_uint8>(channel), p);
}

bool connectToServer(const Options& o, ENetHost*& host, ENetPeer*& peer, MsgWelcome& welcome) {
    host = enet_host_create(nullptr, 1, NUM_CHANNELS, 0, 0);
    if (!host) { std::fprintf(stderr, "could not create ENet host\n"); return false; }
    ENetAddress addr{};
    if (enet_address_set_host(&addr, o.host.c_str()) != 0) {
        std::fprintf(stderr, "could not resolve '%s'\n", o.host.c_str());
        return false;
    }
    addr.port = o.port;
    peer = enet_host_connect(host, &addr, NUM_CHANNELS, 0);
    std::printf("connecting to %s:%u ...\n", o.host.c_str(), o.port);

    ENetEvent ev;
    if (enet_host_service(host, &ev, 5000) <= 0 || ev.type != ENET_EVENT_TYPE_CONNECT) {
        enet_peer_reset(peer);
        std::fprintf(stderr, "connection failed (is boyz_server running?)\n");
        return false;
    }

    MsgHello hello{};
    hello.type = MSG_HELLO;
    hello.version = PROTOCOL_VERSION;
    hello.cls = o.clsGiven ? static_cast<uint8_t>(o.cls) : NO_CLASS;
    std::strncpy(hello.name, o.name.c_str(), sizeof(hello.name) - 1);
    sendTo(peer, &hello, sizeof hello, CH_RELIABLE, true);
    enet_host_flush(host);

    enet_uint32 deadline = enet_time_get() + 5000;
    while (enet_time_get() < deadline) {
        if (enet_host_service(host, &ev, 100) <= 0) continue;
        if (ev.type == ENET_EVENT_TYPE_DISCONNECT) break;
        if (ev.type != ENET_EVENT_TYPE_RECEIVE) continue;
        const uint8_t* d = ev.packet->data;
        size_t n = ev.packet->dataLength;
        bool done = false, ok = false;
        if (n >= sizeof(MsgWelcome) && d[0] == MSG_WELCOME) {
            std::memcpy(&welcome, d, sizeof welcome);
            welcome.mapName[sizeof(welcome.mapName) - 1] = 0;
            done = ok = true;
        } else if (n >= sizeof(MsgReject) && d[0] == MSG_REJECT) {
            std::fprintf(stderr, "server rejected us: %s\n", d[1] == REJECT_FULL ? "server full" : "version mismatch");
            done = true;
        }
        enet_packet_destroy(ev.packet);
        if (done) return ok;
    }
    std::fprintf(stderr, "no welcome from server\n");
    return false;
}

struct RemoteSample {
    double t;
    NetPlayer p;
};

struct Remote {
    std::deque<RemoteSample> samples;
    // Latest snapshot says they're stealthed out of our sight (position unknown).
    bool hidden() const { return samples.empty() || samples.back().p.hidden; }
};

struct MinionTrack {
    std::deque<std::pair<double, glm::vec3>> samples;
    uint8_t hpPct = 100;
    float yaw = 0;
};

glm::vec3 sampleTrack(const std::deque<std::pair<double, glm::vec3>>& s, double t) {
    if (t <= s.front().first) return s.front().second;
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        if (t >= s[i].first && t <= s[i + 1].first) {
            float a = static_cast<float>((t - s[i].first) / (s[i + 1].first - s[i].first));
            return glm::mix(s[i].second, s[i + 1].second, a);
        }
    }
    return s.back().second;
}

// Position of a remote player at time `t` (in the past), interpolated between
// snapshots. `yaw` gets the way they faced then (nearest snapshot).
glm::vec3 sampleRemote(const Remote& r, double t, float& yaw) {
    const auto& s = r.samples;
    auto pos = [](const NetPlayer& p) { return glm::vec3(p.s.x, p.s.y, p.s.z); };
    yaw = s.back().p.s.yaw;
    if (t <= s.front().t) { yaw = s.front().p.s.yaw; return pos(s.front().p); }
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        if (t >= s[i].t && t <= s[i + 1].t) {
            float a = static_cast<float>((t - s[i].t) / (s[i + 1].t - s[i].t));
            yaw = (a < 0.5f ? s[i] : s[i + 1]).p.s.yaw;
            return glm::mix(pos(s[i].p), pos(s[i + 1].p), a);
        }
    }
    return pos(s.back().p);
}

// Visual-only data per ability.
glm::vec3 abilityColor(AbilityId id) {
    switch (id) {
        case AbilityId::Fireball: return {1.0f, 0.38f, 0.04f};
        case AbilityId::FrostBolt: return {0.25f, 0.7f, 1.0f};
        case AbilityId::Thorns: return {0.45f, 1.0f, 0.12f};
        case AbilityId::Regrowth: return {0.2f, 1.0f, 0.3f};
        case AbilityId::HolyBolt: return {1.0f, 0.85f, 0.25f};
        case AbilityId::Heal: return {1.0f, 0.9f, 0.35f};
        case AbilityId::Stealth: return {0.45f, 0.4f, 0.5f};
        case AbilityId::LifeDrain: return {0.75f, 0.1f, 1.0f};
        case AbilityId::RaiseDead: return {0.35f, 1.0f, 0.55f};
        case AbilityId::WarCry: return {1.0f, 0.45f, 0.15f};
        case AbilityId::Blink: return {0.65f, 0.4f, 1.0f};
        case AbilityId::Entangle: return {0.3f, 0.9f, 0.15f};
        case AbilityId::HolyNova: return {1.0f, 0.88f, 0.4f};
        case AbilityId::Shadowstep: return {0.45f, 0.4f, 0.5f};
        case AbilityId::BoneBurst: return {0.5f, 1.0f, 0.6f};
        default: return {1, 1, 1};
    }
}

struct LightStyle {
    float radius;     // metres
    float intensity;  // multiplies the ability colour
};

LightStyle abilityLight(AbilityId id) {
    switch (id) {
        case AbilityId::Fireball: return {8.5f, 2.2f};
        case AbilityId::FrostBolt: return {6.5f, 1.7f};
        case AbilityId::Thorns: return {4.0f, 1.1f};
        case AbilityId::HolyBolt: return {7.0f, 1.9f};
        case AbilityId::Regrowth: return {6.0f, 1.6f};
        case AbilityId::Heal: return {7.0f, 2.0f};
        case AbilityId::LifeDrain: return {6.5f, 1.9f};
        case AbilityId::RaiseDead: return {7.5f, 2.0f};
        case AbilityId::WarCry: return {7.0f, 1.6f};
        case AbilityId::Blink: return {6.0f, 1.8f};
        case AbilityId::Entangle: return {5.0f, 1.4f};
        case AbilityId::HolyNova: return {9.0f, 2.2f};
        case AbilityId::BoneBurst: return {8.5f, 2.2f};
        default: return {4.0f, 1.0f};
    }
}

bool castsLight(AbilityId id) {
    AbilityKind k = abilityInfo(id).kind;
    return k == AbilityKind::Projectile || k == AbilityKind::SelfHeal || k == AbilityKind::HealOverTime ||
           k == AbilityKind::Summon || k == AbilityKind::Blink || k == AbilityKind::Nova;
}

// Kill streak names, announced to everyone.
const char* streakName(int n) {
    if (n >= 10) return "GODLIKE";
    if (n >= 7) return "UNSTOPPABLE";
    if (n >= 5) return "RAMPAGE";
    return "KILLING SPREE";
}

// A body left where someone died: briefly the collapsing frame, then a corpse.
struct Corpse {
    glm::vec3 pos;
    glm::vec3 tint;
    float yaw;
    double t0;
    bool skeleton;
};
constexpr double CORPSE_COLLAPSE_TIME = 0.2;
constexpr double CORPSE_LIFETIME = 15.0;

// Rising number over whatever you hit.
struct DamageNumber {
    glm::vec3 pos;
    std::string text;
    glm::vec3 color;
    double t0;
};
constexpr double DAMAGE_NUMBER_TIME = 0.9;

// Walking animation for someone we only see through snapshots: strides are
// timed by how far they've actually moved.
struct WalkAnim {
    glm::vec3 last{0};
    bool hasLast = false;
    float dist = 0;
    double lastMoveT = -1;
    int frame(double t) const {
        if (t - lastMoveT > 0.12) return FRAME_STAND;
        static const int kCycle[4] = {FRAME_STRIDE_A, FRAME_STAND, FRAME_STRIDE_B, FRAME_STAND};
        return kCycle[static_cast<int>(dist / 0.45f) % 4];
    }
    void update(glm::vec3 pos, double t) {
        if (hasLast) {
            float d = glm::length(glm::vec2(pos.x - last.x, pos.z - last.z));
            if (d < 2.0f) {  // bigger = a teleport, not a step
                dist += d;
                if (d > 1e-3f) lastMoveT = t;
            }
        }
        last = pos;
        hasLast = true;
    }
};

struct Burst {  // short-lived expanding glow (impacts, hits, heals)
    glm::vec3 pos;
    glm::vec3 color;
    double t0;
    double duration;
    float size;
    float lightRadius = 0;  // 0 = doesn't light the surroundings
};

struct Flash {  // light only, no sprite: spell casting at a staff
    glm::vec3 pos;
    glm::vec3 color;
    double t0;
    double duration;
    float radius;
};

struct KillFeedEntry {
    double t;
    uint8_t killer, victim;
    AbilityId ability;
};

struct CenterMessage {
    std::string text;
    glm::vec3 color;
    double t0 = -100;
    double duration = 0;
};

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

struct WeaponPose {
    glm::vec2 offset{0};
    float angle = 0;
    float glow = 1;
};

// First-person weapon animation. `since` = seconds since the ability fired.
WeaponPose weaponPose(bool sword, AbilityKind kind, double since, float walkPhase) {
    WeaponPose w;
    const float base = sword ? 0.35f : 0.12f;
    w.angle = base;
    w.offset = {std::cos(walkPhase) * 4.0f, -std::fabs(std::sin(walkPhase)) * 5.0f};
    const float PI = 3.14159265f;
    auto bump = [&](double dur) { return static_cast<float>(std::sin(PI * std::clamp(since / dur, 0.0, 1.0))); };
    if (since < 0) return w;
    switch (kind) {
        case AbilityKind::Melee: {  // wind up right, sweep hard left, recover
            const double dur = 0.32;
            if (since >= dur) break;
            float p = static_cast<float>(since / dur);
            if (p < 0.15f) {
                w.angle = base - 0.5f * (p / 0.15f);
            } else if (p < 0.55f) {
                float k = (p - 0.15f) / 0.4f;
                w.angle = base - 0.5f + 2.0f * k;
                w.offset.x -= 110.0f * k;
                w.offset.y += 20.0f * std::sin(PI * k);
            } else {
                float k = (p - 0.55f) / 0.45f;
                w.angle = glm::mix(base + 1.5f, base, k);
                w.offset.x -= glm::mix(110.0f, 0.0f, k);
            }
            break;
        }
        case AbilityKind::Dash:  // thrust forward
            if (since < 0.3) { float b = bump(0.3); w.offset.y += 30 * b; w.angle = base * (1 - b); }
            break;
        case AbilityKind::Projectile:  // jab the staff forward, orb flares
            if (since < 0.25) {
                float b = bump(0.25);
                w.offset += glm::vec2(-25 * b, 20 * b);
                w.glow = 1.0f + 1.5f * static_cast<float>(1.0 - since / 0.25);
            }
            break;
        case AbilityKind::Blink:  // same jab as a spell
            if (since < 0.25) {
                float b = bump(0.25);
                w.offset += glm::vec2(-25 * b, 20 * b);
                w.glow = 1.0f + 1.5f * static_cast<float>(1.0 - since / 0.25);
            }
            break;
        case AbilityKind::Stealth:  // tuck the blade away
        case AbilityKind::Shadowstep:
            if (since < 0.4) { float b = bump(0.4); w.offset.y -= 50 * b; w.offset.x += 20 * b; }
            break;
        case AbilityKind::SelfHeal:
        case AbilityKind::HealOverTime:
        case AbilityKind::Nova:
        case AbilityKind::Detonate:
        case AbilityKind::Summon:  // raise the staff overhead
            if (since < 0.6) { float b = bump(0.6); w.offset.y += 45 * b; w.glow = 1.0f + 2.0f * b; }
            break;
    }
    return w;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) return 1;

    if (!opt.clsGiven && SDL_getenv("BOYZ_BOT")) opt.clsGiven = true;  // bots don't use menus
    if (enet_initialize() != 0) { std::fprintf(stderr, "enet_initialize failed\n"); return 1; }
    ENetHost* net = nullptr;
    ENetPeer* server = nullptr;
    MsgWelcome welcome{};
    if (!connectToServer(opt, net, server, welcome)) return 1;
    const uint8_t myId = welcome.playerId;
    {
        std::string path = findMapFile(welcome.mapName, argv[0]);
        std::string err;
        if (path.empty()) {
            std::fprintf(stderr, "server is running map '%s', which you don't have\n", welcome.mapName);
            return 1;
        }
        if (!loadMap(path, err)) {
            std::fprintf(stderr, "failed to load map: %s\n", err.c_str());
            return 1;
        }
        if (mapHash() != welcome.mapHash) {
            std::fprintf(stderr, "your copy of map '%s' differs from the server's\n", welcome.mapName);
            return 1;
        }
    }
    std::printf("joined as player #%d (%s) on map '%s'\n", myId, opt.clsGiven ? classInfo(opt.cls).name : "choosing a class",
                welcome.mapName);

    if (!SDL_Init(SDL_INIT_VIDEO)) { SDL_Log("SDL_Init: %s", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window* window = SDL_CreateWindow("boyzpvp", 1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window) { SDL_Log("SDL_CreateWindow: %s", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(window);
    if (!ctx) { SDL_Log("SDL_GL_CreateContext: %s", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(1);
    if (!glfn::load()) return 1;

    Renderer renderer;
    if (!renderer.init(RETRO_W, RETRO_H)) return 1;

    bool grabbed = true;
    SDL_SetWindowRelativeMouseMode(window, true);

    // Local (predicted) player.
    PlayerState pred{}, prevPred{};
    bool haveServerState = false;
    float viewYaw = 0, viewPitch = 0;
    float myHp = classInfo(opt.cls).maxHp;
    // The server decides our class; these follow its snapshots.
    PlayerClass myCls = opt.cls;
    bool picking = !opt.clsGiven;     // spectating until we choose a class
    uint8_t myNextCls = NO_CLASS;     // switching to this at the next respawn
    bool classMenu = picking;
    uint16_t myKills = 0, myDeaths = 0, myScore = 0;
    uint8_t myStreak = 0;
    uint32_t inputSeq = 0;
    std::deque<InputCmd> pending;  // sent but not yet acknowledged by the server

    std::map<uint8_t, Remote> remotes;
    std::map<uint8_t, MinionTrack> minions;  // by owner
    std::map<uint8_t, std::string> names;
    std::map<uint8_t, PlayerClass> classes;
    std::deque<KillFeedEntry> killFeed;
    CenterMessage centerMsg;
    std::string killedByText;
    double deathT = -1;
    uint16_t myPing = 0;
    std::map<uint8_t, double> remoteAttackT, remoteHurtT;
    std::vector<std::pair<NetProjectile, double>> projectiles;  // latest snapshot + receive time
    std::vector<Burst> bursts;
    std::vector<Flash> flashes;
    std::deque<Corpse> corpses;
    std::vector<DamageNumber> damageNumbers;
    std::map<uint8_t, WalkAnim> walkAnims, minionWalkAnims;
    CenterMessage announce;       // kill streaks etc., above the personal centre message
    bool nextDamageBig = false;   // the next damage we deal was a combo / backstab
    float trauma = 0;             // screen shake, 0..1
    auto addShake = [&](float amount) { trauma = std::min(1.0f, trauma + amount); };
    double hitStopCast = -1;      // castT of the swing that already got its hit-stop
    double lastFrameT = -1;

    // Feedback timers (time of last occurrence).
    double hitMarkerT = -1, hurtT = -1, healT = -1, castT = -1;
    AbilityKind castKind = AbilityKind::Melee;
    AbilityId castAbility = AbilityId::Count;
    float walkPhase = 0;

    auto now = [] { return static_cast<double>(SDL_GetTicksNS()) / 1e9; };
    double last = now(), acc = 0;
    bool running = true;
    bool wantShot = false;
    int shotCount = 0;
    // BOYZ_AUTOSHOT=S: save a screenshot after S seconds and quit (for testing).
    const char* autoShotEnv = SDL_getenv("BOYZ_AUTOSHOT");
    double autoShotAt = autoShotEnv ? now() + std::atof(autoShotEnv) : -1.0;
    // BOYZ_VIEW=yaw,pitch (degrees): initial view direction (for screenshots).
    const char* viewEnv = SDL_getenv("BOYZ_VIEW");
    // BOYZ_BOT=1: aim at the nearest visible player and hold primary attack
    // (target practice / testing).
    const bool botMode = SDL_getenv("BOYZ_BOT") != nullptr;

    auto nameOf = [&](uint8_t id) -> std::string {
        auto it = names.find(id);
        return it != names.end() ? it->second : "player" + std::to_string(id);
    };
    auto updateTitle = [&] {
        std::string title = "boyzpvp - " + opt.name + (picking ? std::string(" (choosing a class)") : std::string(" the ") + classInfo(myCls).name) + "   |   kills " +
                            std::to_string(myKills) + "   deaths " + std::to_string(myDeaths);
        SDL_SetWindowTitle(window, title.c_str());
    };
    updateTitle();

    auto onSnapshot = [&](const uint8_t* d, size_t n) {
        MsgSnapshotHeader hdr;
        if (n < sizeof hdr) return;
        std::memcpy(&hdr, d, sizeof hdr);
        if (n < sizeof hdr + hdr.playerCount * sizeof(NetPlayer) + hdr.projCount * sizeof(NetProjectile) +
                    hdr.minionCount * sizeof(NetMinion))
            return;
        double t = now();
        std::set<uint8_t> seen;
        const uint8_t* p = d + sizeof hdr;
        for (int i = 0; i < hdr.playerCount; ++i, p += sizeof(NetPlayer)) {
            NetPlayer np;
            std::memcpy(&np, p, sizeof np);
            seen.insert(np.id);
            if (np.id == myId) {
                // Reconcile: take server's authoritative state, then re-apply
                // every input the server hasn't processed yet.
                myHp = np.hp;
                myPing = np.ping;
                myNextCls = np.nextCls;
                myScore = np.score;
                myStreak = np.streak;
                bool nowPicking = np.cls == NO_CLASS;
                PlayerClass cls = nowPicking ? myCls : static_cast<PlayerClass>(np.cls);
                if (nowPicking != picking || cls != myCls) {
                    picking = nowPicking;
                    myCls = cls;
                    updateTitle();
                }
                if (np.kills != myKills || np.deaths != myDeaths) {
                    myKills = np.kills;
                    myDeaths = np.deaths;
                    updateTitle();
                }
                pred = np.s;
                while (!pending.empty() && pending.front().seq <= np.lastInputSeq) pending.pop_front();
                for (const InputCmd& c : pending) simulate(pred, c, myCls, TICK_DT);  // ignore re-fired abilities
                pred.yaw = viewYaw;
                pred.pitch = viewPitch;
                if (!haveServerState) {
                    // Adopt the server's spawn facing on first contact.
                    viewYaw = pred.yaw = np.s.yaw;
                    float vy, vp;
                    if (viewEnv && std::sscanf(viewEnv, "%f,%f", &vy, &vp) == 2) {
                        viewYaw = pred.yaw = vy * 3.14159265f / 180.0f;
                        viewPitch = pred.pitch = vp * 3.14159265f / 180.0f;
                    }
                    haveServerState = true;
                }
                // Teleport (respawn / big correction): don't smear across the map.
                if (glm::distance(glm::vec3(pred.x, pred.y, pred.z), glm::vec3(prevPred.x, prevPred.y, prevPred.z)) > 2.0f)
                    prevPred = pred;
            } else {
                auto& r = remotes[np.id];
                // Don't interpolate from where they vanished to where they reappear.
                if (!np.hidden && !r.samples.empty() && r.samples.back().p.hidden) r.samples.clear();
                r.samples.push_back({t, np});
                while (r.samples.size() > 32) r.samples.pop_front();
            }
        }
        for (auto it = remotes.begin(); it != remotes.end();)
            it = seen.count(it->first) ? std::next(it) : remotes.erase(it);
        projectiles.clear();
        for (int i = 0; i < hdr.projCount; ++i, p += sizeof(NetProjectile)) {
            NetProjectile np;
            std::memcpy(&np, p, sizeof np);
            projectiles.push_back({np, t});
        }
        std::set<uint8_t> seenMinions;
        for (int i = 0; i < hdr.minionCount; ++i, p += sizeof(NetMinion)) {
            NetMinion nm;
            std::memcpy(&nm, p, sizeof nm);
            seenMinions.insert(nm.owner);
            MinionTrack& m = minions[nm.owner];
            glm::vec3 pos(nm.x, nm.y, nm.z);
            // A freshly raised one replaces the old: don't slide between them.
            if (!m.samples.empty() && glm::distance(m.samples.back().second, pos) > 3.0f) m.samples.clear();
            m.samples.push_back({t, pos});
            while (m.samples.size() > 32) m.samples.pop_front();
            m.hpPct = nm.hpPct;
            m.yaw = nm.yaw;
        }
        for (auto it = minions.begin(); it != minions.end();)
            it = seenMinions.count(it->first) ? std::next(it) : minions.erase(it);
    };

    auto onEvents = [&](const uint8_t* d, size_t n) {
        MsgEventsHeader hdr;
        if (n < sizeof hdr) return;
        std::memcpy(&hdr, d, sizeof hdr);
        if (n < sizeof hdr + hdr.count * sizeof(NetEvent)) return;
        double t = now();
        for (int i = 0; i < hdr.count; ++i) {
            NetEvent e;
            std::memcpy(&e, d + sizeof hdr + i * sizeof(NetEvent), sizeof e);
            AbilityId ab = static_cast<AbilityId>(e.ability);
            glm::vec3 pos(e.x, e.y, e.z);
            switch (e.type) {
                case EV_ATTACK: {
                    const glm::vec3 chest = pos + glm::vec3(0, PLAYER_HEIGHT * 0.5f, 0);
                    if (ab == AbilityId::Stealth || ab == AbilityId::Shadowstep)  // puff of smoke where they vanished
                        bursts.push_back({chest, abilityColor(ab), t, 0.6, 1.8f});
                    if (ab == AbilityId::Blink) bursts.push_back({chest, abilityColor(ab), t, 0.35, 1.4f, 5.0f});
                    if (ab == AbilityId::WarCry || ab == AbilityId::HolyNova) {
                        float radius = abilityInfo(ab).range;
                        bursts.push_back({chest, abilityColor(ab), t, 0.45, radius * 1.6f, abilityLight(ab).radius});
                        if (e.a == myId || glm::distance(pos, glm::vec3(pred.x, pred.y, pred.z)) < radius)
                            addShake(e.a == myId ? 0.25f : 0.35f);
                    }
                    if (e.a != myId) {
                        remoteAttackT[e.a] = t;
                        if (castsLight(ab)) {
                            LightStyle ls = abilityLight(ab);
                            flashes.push_back({pos + glm::vec3(0, EYE_HEIGHT, 0), abilityColor(ab) * ls.intensity * 1.5f,
                                               t, 0.18, ls.radius * 0.8f});
                        }
                    }
                    break;
                }
                case EV_IMPACT: {
                    const AbilityInfo& ai = abilityInfo(ab);
                    float splash = ai.kind == AbilityKind::Detonate ? ai.range : ai.splashRadius;
                    float size = std::max(0.5f, splash * 0.9f);
                    bursts.push_back({pos, abilityColor(ab), t, splash > 3 ? 0.45 : 0.3, size, abilityLight(ab).radius * 1.3f});
                    // Big explosions nearby rattle the screen.
                    float camDist = glm::distance(pos, glm::vec3(pred.x, pred.y + EYE_HEIGHT, pred.z));
                    if (splash > 1.0f && camDist < splash * 2.5f) addShake(0.45f * (1.0f - camDist / (splash * 2.5f)));
                    break;
                }
                case EV_DAMAGE:
                    if (e.b == myId) {
                        hurtT = t;
                        addShake(std::min(0.6f, e.amount / 45.0f));
                    } else {
                        remoteHurtT[e.b] = t;
                    }
                    if (e.a == myId && e.b != myId) {
                        bool claw = ab == AbilityId::SkeletonClaw;
                        if (!claw) hitMarkerT = t;
                        glm::vec3 c = nextDamageBig ? glm::vec3(1.0f, 0.55f, 0.15f)
                                      : claw        ? glm::vec3(0.6f, 0.75f, 0.6f)
                                                    : glm::vec3(1.0f, 0.9f, 0.4f);
                        std::string txt = std::to_string(e.amount) + (nextDamageBig ? "!" : "");
                        // Spread numbers a little so rapid hits don't stack into one blob.
                        float jx = static_cast<float>(damageNumbers.size() % 3) * 0.2f - 0.2f;
                        damageNumbers.push_back({pos + glm::vec3(jx, 0.5f, 0), txt, c, t});
                        nextDamageBig = false;
                        // Melee hits land with a jolt: a short freeze of the swing and a shake.
                        if (abilityInfo(ab).kind == AbilityKind::Melee && !claw) {
                            addShake(0.18f);
                            if (castT >= 0 && t - castT < 0.35 && hitStopCast != castT) {
                                castT += 0.07;
                                hitStopCast = castT;
                            }
                        }
                    }
                    // Your own hits/heals use a screen flash instead of an in-your-face puff.
                    if (e.b != myId) bursts.push_back({pos, {0.8f, 0.08f, 0.05f}, t, 0.2, 0.45f});
                    break;
                case EV_HEAL:
                    if (e.b == myId) {
                        healT = t;
                        flashes.push_back({pos, abilityColor(ab) * 1.5f, t, 0.5, abilityLight(ab).radius});
                    } else {
                        bursts.push_back({pos, abilityColor(ab), t, 0.5, 1.3f, abilityLight(ab).radius});
                    }
                    break;
                case EV_COMBO: {
                    const char* combo = abilityInfo(ab).comboName ? abilityInfo(ab).comboName : "COMBO";
                    if (e.a == myId) {
                        centerMsg = {std::string(combo) + "!", {1.0f, 0.6f, 0.2f}, t, 1.0};
                        nextDamageBig = true;
                        addShake(0.3f);
                    } else if (e.b == myId) {
                        centerMsg = {std::string(combo) + " BY " + upper(nameOf(e.a)), {1.0f, 0.35f, 0.3f}, t, 1.5};
                    }
                    bursts.push_back({pos, abilityColor(ab), t, 0.3, 1.2f, 5.0f});
                    break;
                }
                case EV_STREAK: {
                    std::string name = streakName(e.amount);
                    if (e.a == myId)
                        announce = {name + "! " + std::to_string(e.amount) + " KILLS - BOUNTY ON YOUR HEAD", {1.0f, 0.75f, 0.2f}, t, 3.0};
                    else
                        announce = {upper(nameOf(e.a)) + " IS ON A " + name + " (" + std::to_string(e.amount) + ")", {1.0f, 0.75f, 0.2f}, t, 3.0};
                    break;
                }
                case EV_SHUTDOWN: {
                    int bonus = e.amount - STREAK_BOUNTY_MIN + 1;
                    if (e.a == myId)
                        announce = {"BOUNTY COLLECTED: +" + std::to_string(bonus) + " POINTS", {1.0f, 0.85f, 0.3f}, t, 3.0};
                    else
                        announce = {upper(nameOf(e.a)) + " ENDED " + upper(nameOf(e.b)) + "'S STREAK OF " + std::to_string(e.amount),
                                    {1.0f, 0.75f, 0.2f}, t, 3.0};
                    break;
                }
                case EV_TELEPORT:
                    if (e.a == myId) {  // face the victim
                        viewYaw = pred.yaw = e.amount / 1000.0f;
                        prevPred = pred;
                    }
                    if (e.b != NO_PLAYER)
                        bursts.push_back({pos + glm::vec3(0, PLAYER_HEIGHT * 0.5f, 0), abilityColor(ab), t, 0.5, 1.6f});
                    break;
                case EV_BACKSTAB:
                    if (e.a == myId) {
                        centerMsg = {"BACKSTAB!", {1.0f, 0.8f, 0.2f}, t, 1.0};
                        nextDamageBig = true;
                        addShake(0.35f);
                    }
                    else if (e.b == myId) centerMsg = {"BACKSTABBED BY " + upper(nameOf(e.a)), {1.0f, 0.35f, 0.3f}, t, 1.5};
                    bursts.push_back({pos, {1.0f, 0.75f, 0.2f}, t, 0.25, 0.8f, 4.0f});
                    break;
                case EV_MINION_DIE: {
                    float yaw = 0;
                    if (auto mi = minions.find(e.a); mi != minions.end()) yaw = mi->second.yaw;
                    corpses.push_back({pos - glm::vec3(0, MINION_HEIGHT * 0.5f, 0), glm::vec3(0.3f), yaw, t, true});
                    bursts.push_back({pos, {0.85f, 0.8f, 0.65f}, t, 0.5, 1.4f});
                    bursts.push_back({pos, abilityColor(AbilityId::RaiseDead), t, 0.35, 0.8f, 5.0f});
                    break;
                }
                case EV_KILL:
                    killFeed.push_back({t, e.a, e.b, ab});
                    while (killFeed.size() > 5) killFeed.pop_front();
                    if (e.b != myId) {  // leave a body (ours would be under the camera)
                        glm::vec3 tint = glm::vec3(0.8f);
                        float yaw = 0;
                        if (auto ci = classes.find(e.b); ci != classes.end()) tint = classColor(ci->second);
                        if (auto ri = remotes.find(e.b); ri != remotes.end() && !ri->second.samples.empty())
                            yaw = ri->second.samples.back().p.s.yaw;
                        corpses.push_back({pos, tint, yaw, t, false});
                        bursts.push_back({pos + glm::vec3(0, 0.9f, 0), {0.7f, 0.05f, 0.03f}, t, 0.35, 1.3f});
                    }
                    if (e.b == myId) {
                        deathT = t;
                        killedByText = (e.a == e.b || e.a == NO_PLAYER)
                                           ? "YOU DIED"
                                           : "KILLED BY " + upper(nameOf(e.a)) + " [" + upper(abilityInfo(ab).name) + "]";
                    } else if (e.a == myId) {
                        centerMsg = {"YOU KILLED " + upper(nameOf(e.b)), {1.0f, 0.85f, 0.3f}, t, 2.0};
                    }
                    if (e.a == e.b || e.a == NO_PLAYER)
                        std::printf("  %s died\n", nameOf(e.b).c_str());
                    else
                        std::printf("  %s  [%s]  %s\n", nameOf(e.a).c_str(), abilityInfo(ab).name, nameOf(e.b).c_str());
                    std::fflush(stdout);
                    break;
                default: break;
            }
        }
    };

    auto onPlayerInfo = [&](const uint8_t* d, size_t n) {
        MsgPlayerInfo m;
        if (n < sizeof m) return;
        std::memcpy(&m, d, sizeof m);
        m.name[sizeof(m.name) - 1] = 0;
        bool known = names.count(m.id) > 0;
        names[m.id] = m.name;
        classes[m.id] = static_cast<PlayerClass>(m.cls);
        const char* clsName = m.cls == NO_CLASS ? "spectator" : classInfo(static_cast<PlayerClass>(m.cls)).name;
        if (!known && m.id != myId) std::printf("  %s joined as %s\n", m.name, clsName);
        else if (known && m.cls != NO_CLASS) std::printf("  %s is now a %s\n", m.name, clsName);
        std::fflush(stdout);
    };

    while (running) {
        // --- Window / input events -----------------------------------------
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_EVENT_QUIT: running = false; break;
                case SDL_EVENT_KEY_DOWN: {
                    const SDL_Scancode sc = e.key.scancode;
                    if (sc == SDL_SCANCODE_F12) wantShot = true;
                    const int numClasses = static_cast<int>(PlayerClass::Count);
                    if (sc == SDL_SCANCODE_C && !e.key.repeat && !picking) {
                        classMenu = !classMenu;
                    } else if ((classMenu || picking) && sc >= SDL_SCANCODE_1 && sc < SDL_SCANCODE_1 + numClasses) {
                        MsgChangeClass m{MSG_CHANGE_CLASS, static_cast<uint8_t>(sc - SDL_SCANCODE_1)};
                        sendTo(server, &m, sizeof m, CH_RELIABLE, true);
                        classMenu = false;
                    } else if (sc == SDL_SCANCODE_ESCAPE) {
                        if (classMenu && !picking) classMenu = false;
                        else if (grabbed) { grabbed = false; SDL_SetWindowRelativeMouseMode(window, false); }
                        else running = false;
                    }
                    break;
                }
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (!grabbed) { grabbed = true; SDL_SetWindowRelativeMouseMode(window, true); }
                    break;
                case SDL_EVENT_WINDOW_FOCUS_LOST:
                    grabbed = false;
                    SDL_SetWindowRelativeMouseMode(window, false);
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    if (grabbed) {
                        viewYaw += e.motion.xrel * MOUSE_SENS;
                        viewPitch = std::clamp(viewPitch - e.motion.yrel * MOUSE_SENS, -1.5f, 1.5f);
                    }
                    break;
                default: break;
            }
        }

        // --- Network receive -----------------------------------------------
        ENetEvent ev;
        while (enet_host_service(net, &ev, 0) > 0) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                const uint8_t* d = ev.packet->data;
                size_t n = ev.packet->dataLength;
                if (n > 0) {
                    switch (d[0]) {
                        case MSG_SNAPSHOT: onSnapshot(d, n); break;
                        case MSG_EVENTS: onEvents(d, n); break;
                        case MSG_PLAYER_INFO: onPlayerInfo(d, n); break;
                        default: break;
                    }
                }
                enet_packet_destroy(ev.packet);
            } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                std::fprintf(stderr, "disconnected from server\n");
                running = false;
            }
        }

        // --- Fixed-rate simulation ticks ------------------------------------
        double t = now();
        acc += std::min(t - last, 0.25);
        last = t;
        while (acc >= TICK_DT) {
            acc -= TICK_DT;
            const bool* keys = SDL_GetKeyboardState(nullptr);
            SDL_MouseButtonFlags mouse = SDL_GetMouseState(nullptr, nullptr);
            InputCmd cmd{};
            cmd.seq = ++inputSeq;
            if (grabbed) {
                cmd.forward = static_cast<int8_t>((keys[SDL_SCANCODE_W] ? 1 : 0) - (keys[SDL_SCANCODE_S] ? 1 : 0));
                cmd.strafe = static_cast<int8_t>((keys[SDL_SCANCODE_D] ? 1 : 0) - (keys[SDL_SCANCODE_A] ? 1 : 0));
                if (keys[SDL_SCANCODE_SPACE]) cmd.buttons |= BTN_JUMP;
                if (mouse & SDL_BUTTON_LMASK) cmd.buttons |= BTN_PRIMARY;
                if (mouse & SDL_BUTTON_RMASK) cmd.buttons |= BTN_SECONDARY;
                if (keys[SDL_SCANCODE_Q]) cmd.buttons |= BTN_TERTIARY;
            }
            if (botMode && pred.alive) {
                float best = 1e9f;
                glm::vec3 targetPos(0);
                for (const auto& [id, r] : remotes) {
                    if (r.hidden()) continue;
                    const NetPlayer& o = r.samples.back().p;
                    float dx = o.s.x - pred.x, dz = o.s.z - pred.z;
                    float dist = std::hypot(dx, dz);
                    if (!o.s.alive || dist >= best ||
                        !lineOfSight(pred.x, pred.y + EYE_HEIGHT, pred.z, o.s.x, o.s.y + PLAYER_HEIGHT * 0.5f, o.s.z))
                        continue;
                    best = dist;
                    targetPos = glm::vec3(o.s.x, o.s.y, o.s.z);
                    float dy = (o.s.y + PLAYER_HEIGHT * 0.5f) - (pred.y + EYE_HEIGHT - 0.15f);
                    viewYaw = std::atan2(dx, -dz);
                    viewPitch = std::atan2(dy, dist);
                }
                const bool meleeBot = abilityInfo(classInfo(myCls).abilities[0]).kind == AbilityKind::Melee;
                AbilityKind second = abilityInfo(classInfo(myCls).abilities[1]).kind;
                if (best < 1e9f) {
                    cmd.buttons |= BTN_PRIMARY;
                    AbilityKind third = abilityInfo(classInfo(myCls).abilities[2]).kind;
                    if (third == AbilityKind::Detonate) {  // when the skeleton has caught up with them
                        auto mi = minions.find(myId);
                        if (mi != minions.end() && glm::distance(mi->second.samples.back().second, targetPos) < 2.5f)
                            cmd.buttons |= BTN_TERTIARY;
                    } else if (best < 8.0f) {
                        cmd.buttons |= BTN_TERTIARY;
                    }
                    // Frostbolt alongside fireball sets up shatters; keep a skeleton up.
                    if (second == AbilityKind::Projectile || (second == AbilityKind::Summon && !minions.count(myId)))
                        cmd.buttons |= BTN_SECONDARY;
                    if (best > 2.0f && meleeBot) cmd.forward = 1;
                } else if (second == AbilityKind::Stealth || second == AbilityKind::Summon) {
                    cmd.buttons |= BTN_SECONDARY;  // nobody in sight: stealth up / raise a skeleton
                }
            }
            cmd.yaw = viewYaw;
            cmd.pitch = viewPitch;

            prevPred = pred;
            if (haveServerState) {
                uint8_t fired = simulate(pred, cmd, myCls, TICK_DT);
                if (fired) {
                    int slot = (fired & 1) ? 0 : (fired & 2) ? 1 : 2;
                    castT = t;
                    castAbility = classInfo(myCls).abilities[slot];
                    castKind = abilityInfo(castAbility).kind;
                    if (castAbility == AbilityId::WarCry) castKind = AbilityKind::Summon;  // weapon raised overhead
                    if (castsLight(castAbility)) {
                        float dx, dy, dz;
                        viewDir(viewYaw, viewPitch, dx, dy, dz);
                        glm::vec3 staff = glm::vec3(pred.x, pred.y + EYE_HEIGHT - 0.2f, pred.z) + glm::vec3(dx, dy, dz) * 0.6f;
                        LightStyle ls = abilityLight(castAbility);
                        flashes.push_back({staff, abilityColor(castAbility) * ls.intensity * 1.5f, t, 0.18, ls.radius * 0.8f});
                    }
                }
                if (pred.onGround) walkPhase += std::hypot(pred.vx, pred.vz) * TICK_DT * 1.6f;
            }
            pending.push_back(cmd);
            while (pending.size() > 256) pending.pop_front();

            MsgInput msg{};
            msg.type = MSG_INPUT;
            msg.count = static_cast<uint8_t>(std::min<size_t>(INPUT_REDUNDANCY, pending.size()));
            for (int i = 0; i < msg.count; ++i) msg.cmds[i] = pending[pending.size() - msg.count + i];
            sendTo(server, &msg, MSG_INPUT_HEADER + msg.count * sizeof(InputCmd), CH_UNRELIABLE, false);
        }
        enet_host_flush(net);

        // --- Render --------------------------------------------------------
        const bool alive = pred.alive || !haveServerState;
        float alpha = static_cast<float>(acc / TICK_DT);
        glm::vec3 feet = glm::mix(glm::vec3(prevPred.x, prevPred.y, prevPred.z), glm::vec3(pred.x, pred.y, pred.z), alpha);
        Camera cam{feet + glm::vec3(0, alive || picking ? EYE_HEIGHT : 0.35f, 0), viewYaw, viewPitch};
        {
            float frameDt = lastFrameT < 0 ? 0.0f : static_cast<float>(t - lastFrameT);
            lastFrameT = t;
            trauma = std::max(0.0f, trauma - frameDt * 1.8f);
            float shake = trauma * trauma;
            float ft = static_cast<float>(t);
            cam.yaw += 0.04f * shake * std::sin(ft * 53.0f);
            cam.pitch += 0.04f * shake * std::sin(ft * 67.0f + 1.3f);
            cam.eye.y += 0.05f * shake * std::sin(ft * 41.0f + 2.1f);
        }
        const ClassInfo& myClass = classInfo(myCls);
        const bool hasSword = myCls == PlayerClass::Warrior || myCls == PlayerClass::Rogue;

        std::vector<Billboard> billboards;
        double renderT = t - INTERP_DELAY;
        for (const auto& [id, r] : remotes) {
            if (r.hidden() || !r.samples.back().p.s.alive) continue;
            PlayerClass c = static_cast<PlayerClass>(r.samples.back().p.cls);
            float yaw;
            const NetPlayer& latest = r.samples.back().p;
            Billboard b{sampleRemote(r, renderT, yaw), PLAYER_HEIGHT * 0.5f, PLAYER_HEIGHT, classColor(c), SpriteTex::Player};
            b.directional = true;
            b.facingYaw = yaw;
            WalkAnim& anim = walkAnims[id];
            anim.update(b.base, t);
            b.frame = latest.s.onGround ? anim.frame(t) : FRAME_STRIDE_A;
            // Status effects show on the body: icy blue when slowed, green when rooted.
            if (latest.s.rootTicks) b.tint = glm::mix(b.tint, glm::vec3(0.35f, 1.0f, 0.25f), 0.6f);
            else if (latest.s.slowTicks) b.tint = glm::mix(b.tint, glm::vec3(0.45f, 0.75f, 1.3f), 0.55f);
            // A bounty: a gold light bobbing over their head.
            if (latest.streak >= STREAK_BOUNTY_MIN && !latest.s.stealthTicks) {
                float bob = 0.06f * std::sin(static_cast<float>(t) * 4.0f + id);
                billboards.push_back({b.base + glm::vec3(0, PLAYER_HEIGHT + 0.3f + bob, 0), 0.32f, 0.32f,
                                      glm::vec3(1.0f, 0.75f, 0.15f), SpriteTex::Orb, 1.6f});
            }
            // Stealthed but close enough to notice: a faint shimmer.
            if (r.samples.back().p.s.stealthTicks)
                b.opacity = 0.2f + 0.08f * std::sin(static_cast<float>(t) * 9.0f + id);
            auto hurt = remoteHurtT.find(id);
            if (hurt != remoteHurtT.end() && t - hurt->second < 0.15) b.tint *= glm::vec3(2.0f, 0.45f, 0.45f);
            auto atk = remoteAttackT.find(id);
            if (atk != remoteAttackT.end() && t - atk->second < 0.2) b.brightness = 1.5f;
            billboards.push_back(b);
        }
        for (const auto& [owner, m] : minions) {
            if (m.samples.empty()) continue;
            // Eyes glow green for your own skeleton, red for everyone else's.
            glm::vec3 eyes = owner == myId ? glm::vec3(0.4f, 1.6f, 0.6f) : glm::vec3(1.8f, 0.3f, 0.2f);
            glm::vec3 pos = sampleTrack(m.samples, renderT);
            Billboard b{pos, MINION_HEIGHT * 0.5f, MINION_HEIGHT, eyes, SpriteTex::Skeleton};
            b.directional = true;
            b.facingYaw = m.yaw;
            WalkAnim& anim = minionWalkAnims[owner];
            anim.update(pos, t);
            b.frame = anim.frame(t);
            billboards.push_back(b);
        }
        for (auto it = walkAnims.begin(); it != walkAnims.end();)
            it = remotes.count(it->first) ? std::next(it) : walkAnims.erase(it);
        for (auto it = minionWalkAnims.begin(); it != minionWalkAnims.end();)
            it = minions.count(it->first) ? std::next(it) : minionWalkAnims.erase(it);
        // Bodies: a moment of collapsing, then lying there until they fade out.
        while (!corpses.empty() && (t - corpses.front().t0 > CORPSE_LIFETIME || corpses.size() > 24)) corpses.pop_front();
        for (const Corpse& c : corpses) {
            double age = t - c.t0;
            if (age < CORPSE_COLLAPSE_TIME) {
                float h = c.skeleton ? MINION_HEIGHT : PLAYER_HEIGHT;
                Billboard b{c.pos, h * 0.5f, h, c.skeleton ? glm::vec3(1.0f, 0.3f, 0.2f) : c.tint,
                            c.skeleton ? SpriteTex::Skeleton : SpriteTex::Player};
                b.directional = true;
                b.facingYaw = c.yaw;
                b.frame = FRAME_COLLAPSE;
                billboards.push_back(b);
                continue;
            }
            const float len = c.skeleton ? 1.5f : 1.8f;
            Billboard b{c.pos, len, len * 0.5f, c.tint, c.skeleton ? SpriteTex::SkeletonCorpse : SpriteTex::PlayerCorpse};
            b.opacity = static_cast<float>(std::clamp((CORPSE_LIFETIME - age) / 2.0, 0.0, 1.0));
            if (b.opacity > 0.02f) billboards.push_back(b);
        }
        for (const auto& [p, recvT] : projectiles) {
            // Extrapolate along the (straight) flight path since the snapshot.
            float dt = static_cast<float>(std::min(t - recvT, 0.2));
            AbilityId ab = static_cast<AbilityId>(p.ability);
            float size = std::max(0.3f, abilityInfo(ab).projRadius * 3.2f);
            glm::vec3 center(p.x + p.vx * dt, p.y + p.vy * dt, p.z + p.vz * dt);
            if (glm::distance(center, cam.eye) <= size * 0.5f + 0.6f) continue;
            billboards.push_back({center - glm::vec3(0, size * 0.5f, 0), size, size, abilityColor(ab), SpriteTex::Orb, 1.8f});
            // Short fading trail behind it.
            glm::vec3 back = -glm::normalize(glm::vec3(p.vx, p.vy, p.vz) + glm::vec3(1e-4f));
            for (int k = 1; k <= 3; ++k) {
                float ts = size * (1.0f - 0.22f * k);
                glm::vec3 c = center + back * (size * 0.55f * k);
                billboards.push_back({c - glm::vec3(0, ts * 0.5f, 0), ts, ts, abilityColor(ab), SpriteTex::Orb, 1.6f - 0.3f * k});
            }
        }
        bursts.erase(std::remove_if(bursts.begin(), bursts.end(), [&](const Burst& b) { return t - b.t0 > b.duration; }),
                     bursts.end());
        for (const Burst& b : bursts) {
            float k = static_cast<float>(std::sin(3.14159 * (t - b.t0) / b.duration));
            float size = std::max(0.05f, b.size * k);
            // Glows right on top of the camera would just fill the screen; their light still shows.
            if (glm::distance(b.pos, cam.eye) > b.size * 0.5f + 0.6f)
                billboards.push_back({b.pos - glm::vec3(0, size * 0.5f, 0), size, size, b.color, SpriteTex::Orb, 1.8f});
        }

        // --- Dynamic lights ---
        std::vector<PointLight> lights;
        for (const auto& [p, recvT] : projectiles) {
            float dt = static_cast<float>(std::min(t - recvT, 0.2));
            AbilityId ab = static_cast<AbilityId>(p.ability);
            LightStyle ls = abilityLight(ab);
            float flicker = ab == AbilityId::Fireball ? 0.85f + 0.15f * std::sin(static_cast<float>(t) * 40.0f + p.id) : 1.0f;
            lights.push_back({glm::vec3(p.x + p.vx * dt, p.y + p.vy * dt, p.z + p.vz * dt),
                              abilityColor(ab) * ls.intensity * flicker, ls.radius});
        }
        for (const Burst& b : bursts) {
            if (b.lightRadius <= 0) continue;
            float k = static_cast<float>(1.0 - (t - b.t0) / b.duration);
            lights.push_back({b.pos, b.color * 1.5f * k, b.lightRadius});
        }
        flashes.erase(std::remove_if(flashes.begin(), flashes.end(), [&](const Flash& f) { return t - f.t0 > f.duration; }),
                      flashes.end());
        for (const Flash& f : flashes) {
            float k = static_cast<float>(1.0 - (t - f.t0) / f.duration);
            lights.push_back({f.pos, f.color * k, f.radius});
        }
        // Torches: flickering warm light plus the torch sprite.
        for (int i = 0; i < torchCount(); ++i) {
            const Torch& tc = torchAt(i);
            float ft = static_cast<float>(t);
            float flicker = 0.85f + 0.09f * std::sin(ft * 13.0f + i * 2.1f) + 0.06f * std::sin(ft * 31.0f + i * 0.7f);
            glm::vec3 flame(tc.x, tc.y, tc.z);
            // Light sits a little out from the wall so the wall itself catches it.
            glm::vec3 lightPos = flame - glm::vec3(tc.dx, 0, tc.dz) * 0.6f;
            lights.push_back({lightPos, glm::vec3(1.0f, 0.55f, 0.2f) * 1.9f * flicker, 9.0f});
            float h = 1.0f * (0.96f + 0.04f * flicker);
            billboards.push_back({flame - glm::vec3(0, h * 0.62f, 0), h * 0.5f, h, glm::vec3(1.0f, 0.55f, 0.15f),
                                  SpriteTex::Torch, 1.1f * flicker});
        }

        // Light reaching the first-person weapon (no normals: it's a flat sprite).
        glm::vec3 weaponLight(0.85f);
        for (const PointLight& l : lights) {
            float d = glm::distance(l.pos, cam.eye);
            if (d < l.radius) weaponLight += l.color * (1.0f - d / l.radius) * (1.0f - d / l.radius) * 0.8f;
        }

        Hud hud;
        hud.weaponLight = glm::min(weaponLight, glm::vec3(2.0f));
        hud.hpFrac = myHp / myClass.maxHp;
        hud.classColor = classColor(myCls);
        for (int slot = 0; slot < NUM_ABILITY_SLOTS; ++slot) {
            float total = abilityInfo(myClass.abilities[slot]).cooldownTicks;
            hud.ready[slot] = total > 0 ? 1.0f - pred.cooldown[slot] / total : 1.0f;
        }
        hud.showWeapon = alive && !picking;
        hud.showStatus = !picking;
        hud.weapon = myCls == PlayerClass::Rogue ? WeaponTex::Dagger : hasSword ? WeaponTex::Sword : WeaponTex::Staff;
        WeaponPose pose = weaponPose(hasSword, castKind, castT < 0 ? -1.0 : t - castT, walkPhase);
        hud.weaponOffset = pose.offset;
        hud.weaponAngle = pose.angle;
        hud.weaponGlow = pose.glow;
        hud.hitMarker = hitMarkerT >= 0 && t - hitMarkerT < 0.15;
        if (!alive && !picking) hud.flash = {0.35f, 0.0f, 0.0f, 0.55f};
        else if (hurtT >= 0 && t - hurtT < 0.3) hud.flash = {0.8f, 0.05f, 0.0f, 0.25f * static_cast<float>(1.0 - (t - hurtT) / 0.3)};
        else if (healT >= 0 && t - healT < 0.4) hud.flash = {0.3f, 1.0f, 0.4f, 0.2f * static_cast<float>(1.0 - (t - healT) / 0.4)};
        else if (alive && pred.stealthTicks) hud.flash = {0.15f, 0.1f, 0.25f, 0.3f};
        if (alive && pred.stealthTicks) hud.weaponOpacity = 0.4f;

        // --- Text HUD: kill feed, centre messages, scoreboard ---
        auto colorOf = [&](uint8_t id) {
            auto it = classes.find(id);
            return it != classes.end() ? classColor(it->second) : glm::vec3(0.8f);
        };
        auto text = [&](int x, int y, const std::string& s, glm::vec3 c) { hud.texts.push_back({x, y, s, c}); };
        auto centered = [&](int y, const std::string& s, glm::vec3 c) { text((RETRO_W - textWidth(s)) / 2, y, s, c); };

        // Kill feed (top right), newest at the bottom, entries expire after 6 s.
        while (!killFeed.empty() && t - killFeed.front().t > 6.0) killFeed.pop_front();
        {
            int y = 4;
            for (const KillFeedEntry& k : killFeed) {
                bool suicide = k.killer == k.victim || k.killer == NO_PLAYER;
                std::vector<std::pair<std::string, glm::vec3>> segs;
                if (suicide) {
                    segs = {{upper(nameOf(k.victim)), colorOf(k.victim)}, {" DIED", glm::vec3(0.7f)}};
                } else {
                    segs = {{upper(nameOf(k.killer)), colorOf(k.killer)},
                            {" [" + upper(abilityInfo(k.ability).name) + "] ", abilityColor(k.ability) * 0.6f + 0.4f},
                            {upper(nameOf(k.victim)), colorOf(k.victim)}};
                }
                int w = 0;
                for (auto& sgm : segs) w += static_cast<int>(sgm.first.size()) * GLYPH_ADVANCE;
                int x = RETRO_W - 4 - w;
                bool involvesMe = k.killer == myId || k.victim == myId;
                hud.rects.push_back({x - 3, y - 2, w + 4, LINE_HEIGHT + 1,
                                     involvesMe ? glm::vec4(0.35f, 0.25f, 0.05f, 0.6f) : glm::vec4(0, 0, 0, 0.45f)});
                for (auto& [s, c] : segs) {
                    text(x, y, s, c);
                    x += static_cast<int>(s.size()) * GLYPH_ADVANCE;
                }
                y += LINE_HEIGHT + 2;
            }
        }

        // Status lines above the health bar.
        {
            int y = RETRO_H - 24;
            if (alive && pred.stealthTicks) {
                int secs = static_cast<int>(std::ceil(pred.stealthTicks * TICK_DT));
                text(6, y, "STEALTHED " + std::to_string(secs), {0.75f, 0.65f, 0.95f});
                y -= LINE_HEIGHT + 1;
            }
            if (alive && pred.rootTicks) {
                text(6, y, "ROOTED", {0.45f, 1.0f, 0.35f});
                y -= LINE_HEIGHT + 1;
            } else if (alive && pred.slowTicks) {
                text(6, y, "SLOWED", {0.5f, 0.75f, 1.0f});
                y -= LINE_HEIGHT + 1;
            }
            if (alive && myStreak >= STREAK_BOUNTY_MIN) {
                text(6, y, "BOUNTY ON YOU: " + std::to_string(myStreak) + " KILLS", {1.0f, 0.75f, 0.2f});
                y -= LINE_HEIGHT + 1;
            }
            if (auto it = minions.find(myId); it != minions.end()) {
                text(6, y, "SKELETON " + std::to_string(it->second.hpPct) + "%", abilityColor(AbilityId::RaiseDead));
                y -= LINE_HEIGHT + 1;
            }
            if (alive && myNextCls < static_cast<uint8_t>(PlayerClass::Count)) {
                PlayerClass nc = static_cast<PlayerClass>(myNextCls);
                text(6, y, "NEXT LIFE: " + upper(classInfo(nc).name), classColor(nc));
            }
        }

        // Centre messages.
        if (haveServerState && !pred.alive && !picking && deathT >= 0) {
            // Below the scoreboard, which opens automatically while dead.
            centered(RETRO_H - 50, killedByText, {1.0f, 0.35f, 0.3f});
            int secs = std::max(1, static_cast<int>(std::ceil(RESPAWN_TICKS * TICK_DT - (t - deathT))));
            centered(RETRO_H - 39, "RESPAWN IN " + std::to_string(secs), glm::vec3(0.8f));
            if (!classMenu) centered(RETRO_H - 28, "PRESS C TO CHANGE CLASS", glm::vec3(0.6f));
        } else if (t - centerMsg.t0 < centerMsg.duration) {
            centered(RETRO_H / 2 - 30, centerMsg.text, centerMsg.color);
        }
        if (t - announce.t0 < announce.duration) {
            // While dead the scoreboard covers the middle: go just above the death text instead.
            bool dead = haveServerState && !pred.alive && !picking;
            centered(dead ? RETRO_H - 62 : RETRO_H / 2 - 44, announce.text, announce.color);
        }

        // Damage numbers float up from what you hit (hidden behind full-screen panels).
        const bool* kb = SDL_GetKeyboardState(nullptr);
        const bool menuShown = classMenu || picking;
        const bool boardShown = kb[SDL_SCANCODE_TAB] || (haveServerState && !pred.alive && !picking && !menuShown);
        damageNumbers.erase(std::remove_if(damageNumbers.begin(), damageNumbers.end(),
                                           [&](const DamageNumber& d) { return t - d.t0 > DAMAGE_NUMBER_TIME; }),
                            damageNumbers.end());
        {
            glm::mat4 vp = cameraViewProj(cam, static_cast<float>(RETRO_W) / RETRO_H);
            for (const DamageNumber& d : damageNumbers) {
                if (boardShown || menuShown) break;
                float age = static_cast<float>(t - d.t0);
                glm::vec4 clip = vp * glm::vec4(d.pos + glm::vec3(0, age * 0.9f, 0), 1.0f);
                if (clip.w < 0.1f) continue;
                int sx = static_cast<int>((clip.x / clip.w * 0.5f + 0.5f) * RETRO_W);
                int sy = static_cast<int>((0.5f - clip.y / clip.w * 0.5f) * RETRO_H);
                float fade = 1.0f - 0.6f * age / static_cast<float>(DAMAGE_NUMBER_TIME);
                text(sx - textWidth(d.text) / 2, sy, d.text, d.color * fade);
            }
        }

        // Scoreboard: hold Tab, or automatically while dead.
        if (boardShown) {
            struct Row {
                uint8_t id;
                PlayerClass cls;
                int score, kills, deaths, ping, streak;
            };
            std::vector<Row> rows = {{myId, picking ? static_cast<PlayerClass>(NO_CLASS) : myCls, myScore, myKills,
                                      myDeaths, myPing, myStreak}};
            for (const auto& [id, r] : remotes) {
                if (r.samples.empty()) continue;
                const NetPlayer& np = r.samples.back().p;
                rows.push_back({id, static_cast<PlayerClass>(np.cls), np.score, np.kills, np.deaths, np.ping, np.streak});
            }
            std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
                return a.score != b.score ? a.score > b.score : a.deaths < b.deaths;
            });
            const int pw = 274, px = (RETRO_W - pw) / 2, py = 40;
            const int rowH = LINE_HEIGHT + 2;
            const int ph = 30 + static_cast<int>(rows.size()) * rowH;
            hud.rects.push_back({px, py, pw, ph, {0.03f, 0.02f, 0.02f, 0.75f}});
            hud.rects.push_back({px, py, pw, 14, {0.25f, 0.12f, 0.05f, 0.85f}});
            text(px + 6, py + 4, upper(mapName()), {1.0f, 0.8f, 0.45f});
            std::string count = std::to_string(rows.size()) + (rows.size() == 1 ? " PLAYER" : " PLAYERS");
            text(px + pw - 6 - textWidth(count), py + 4, count, glm::vec3(0.75f));
            const int cName = px + 6, cClass = px + 102, cPts = px + 172, cK = px + 200, cD = px + 222, cPing = px + 244;
            int y = py + 18;
            const glm::vec3 headC(0.6f);
            text(cName, y, "NAME", headC);
            text(cClass, y, "CLASS", headC);
            text(cPts, y, "PTS", headC);
            text(cK, y, "K", headC);
            text(cD, y, "D", headC);
            text(cPing, y, "PING", headC);
            y += rowH;
            for (const Row& r : rows) {
                if (r.id == myId) hud.rects.push_back({px + 2, y - 2, pw - 4, rowH, glm::vec4(classColor(r.cls) * 0.35f, 0.6f)});
                glm::vec3 c = r.id == myId ? glm::vec3(1.0f) : glm::vec3(0.85f);
                std::string nm = upper(nameOf(r.id)).substr(0, 15);
                text(cName, y, nm, c);
                if (r.streak >= STREAK_BOUNTY_MIN)  // has a bounty
                    text(cName + textWidth(nm) + 3, y, "*", {1.0f, 0.75f, 0.2f});
                text(cPts, y, std::to_string(r.score), {1.0f, 0.85f, 0.5f});
                if (r.cls < PlayerClass::Count) text(cClass, y, upper(classInfo(r.cls).name), classColor(r.cls));
                else text(cClass, y, "PICKING", glm::vec3(0.55f));
                text(cK, y, std::to_string(r.kills), c);
                text(cD, y, std::to_string(r.deaths), c);
                text(cPing, y, std::to_string(r.ping), r.ping > 150 ? glm::vec3(1.0f, 0.5f, 0.3f) : c);
                y += rowH;
            }
        }

        // Class picker: C to open, number keys to choose.
        if (menuShown && haveServerState) {
            const int n = static_cast<int>(PlayerClass::Count);
            const int pw = 392, px = (RETRO_W - pw) / 2, rowH = LINE_HEIGHT + 3;
            const int ph = 22 + n * rowH + 3 * LINE_HEIGHT + 6;
            const int py = std::max(4, (RETRO_H - ph) / 2 - 10);
            hud.rects.push_back({px, py, pw, ph, {0.03f, 0.02f, 0.02f, 0.82f}});
            hud.rects.push_back({px, py, pw, 14, {0.25f, 0.12f, 0.05f, 0.9f}});
            text(px + 6, py + 4, "CHOOSE YOUR CLASS", {1.0f, 0.8f, 0.45f});
            int y = py + 20;
            for (int i = 0; i < n; ++i) {
                PlayerClass c = static_cast<PlayerClass>(i);
                const ClassInfo& ci = classInfo(c);
                bool current = !picking && c == myCls;
                bool next = myNextCls == i;
                if (current || next)
                    hud.rects.push_back({px + 2, y - 2, pw - 4, rowH, glm::vec4(classColor(c) * 0.3f, 0.6f)});
                text(px + 8, y, std::to_string(i + 1), glm::vec3(1.0f, 0.85f, 0.5f));
                text(px + 22, y, upper(ci.name), classColor(c));
                text(px + 94, y, std::to_string(static_cast<int>(ci.maxHp)) + " HP", glm::vec3(0.75f));
                std::string abil;
                for (int s = 0; s < NUM_ABILITY_SLOTS; ++s)
                    abil += (s ? " / " : "") + upper(abilityInfo(ci.abilities[s]).name);
                text(px + 138, y, abil, glm::vec3(0.85f));
                if (current) text(px + pw - 6 - textWidth("NOW"), y, "NOW", glm::vec3(0.6f));
                else if (next) text(px + pw - 6 - textWidth("NEXT"), y, "NEXT", glm::vec3(0.6f));
                y += rowH;
            }
            y += 4;
            text(px + 138, y - 2, "LMB / RMB / Q", glm::vec3(0.5f));
            y += LINE_HEIGHT;
            if (picking) {
                text(px + 6, y, "PRESS 1-" + std::to_string(n) + " TO JOIN THE FIGHT", glm::vec3(0.8f));
            } else {
                text(px + 6, y, pred.alive ? "TAKES EFFECT WHEN YOU NEXT RESPAWN" : "YOU WILL RESPAWN AS YOUR PICK",
                     glm::vec3(0.7f));
                text(px + 6, y + LINE_HEIGHT, "C OR ESC TO CLOSE", glm::vec3(0.55f));
            }
        }

        renderer.renderFrame(cam, billboards, lights, hud);
        int ww, wh;
        SDL_GetWindowSizeInPixels(window, &ww, &wh);
        bool autoShot = autoShotAt > 0 && t >= autoShotAt;
        if (wantShot || autoShot) {
            char path[64];
            std::snprintf(path, sizeof path, "screenshot_%03d.ppm", shotCount++);
            if (renderer.saveScreenshot(path)) std::printf("saved %s\n", path);
            wantShot = false;
            if (autoShot) running = false;
        }
        renderer.present(ww, wh);
        SDL_GL_SwapWindow(window);
    }

    renderer.shutdown();
    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    enet_peer_disconnect_now(server, 0);
    enet_host_destroy(net);
    enet_deinitialize();
    return 0;
}
