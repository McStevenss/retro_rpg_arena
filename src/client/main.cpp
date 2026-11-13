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

#include "game.h"
#include "gl.h"
#include "protocol.h"
#include "render.h"

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
};

glm::vec3 classColor(PlayerClass c) {
    switch (c) {
        case PlayerClass::Warrior: return {0.85f, 0.3f, 0.25f};
        case PlayerClass::Mage: return {0.35f, 0.45f, 1.0f};
        case PlayerClass::Druid: return {0.4f, 0.8f, 0.3f};
        case PlayerClass::Healer: return {1.0f, 0.95f, 0.75f};
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
            if (!parseClass(argv[++i], o.cls)) {
                std::fprintf(stderr, "unknown class '%s' (warrior, mage, druid, healer)\n", argv[i]);
                return false;
            }
        } else {
            std::fprintf(stderr,
                         "usage: boyz_client [--host H] [--port P] [--name N] "
                         "[--class warrior|mage|druid|healer]\n");
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
    hello.cls = static_cast<uint8_t>(o.cls);
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
};

// Position of a remote player at time `t` (in the past), interpolated between snapshots.
glm::vec3 sampleRemote(const Remote& r, double t) {
    const auto& s = r.samples;
    auto pos = [](const NetPlayer& p) { return glm::vec3(p.s.x, p.s.y, p.s.z); };
    if (t <= s.front().t) return pos(s.front().p);
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        if (t >= s[i].t && t <= s[i + 1].t) {
            float a = static_cast<float>((t - s[i].t) / (s[i + 1].t - s[i].t));
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
        default: return {4.0f, 1.0f};
    }
}

bool castsLight(AbilityId id) {
    AbilityKind k = abilityInfo(id).kind;
    return k == AbilityKind::Projectile || k == AbilityKind::SelfHeal || k == AbilityKind::HealOverTime;
}

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
        case AbilityKind::SelfHeal:
        case AbilityKind::HealOverTime:  // raise the staff overhead
            if (since < 0.6) { float b = bump(0.6); w.offset.y += 45 * b; w.glow = 1.0f + 2.0f * b; }
            break;
    }
    return w;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) return 1;

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
    std::printf("joined as player #%d (%s) on map '%s'\n", myId, classInfo(opt.cls).name, welcome.mapName);

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
    uint16_t myKills = 0, myDeaths = 0;
    uint32_t inputSeq = 0;
    std::deque<InputCmd> pending;  // sent but not yet acknowledged by the server
    const ClassInfo& myClass = classInfo(opt.cls);
    const bool hasSword = opt.cls == PlayerClass::Warrior;

    std::map<uint8_t, Remote> remotes;
    std::map<uint8_t, std::string> names;
    std::map<uint8_t, double> remoteAttackT, remoteHurtT;
    std::vector<std::pair<NetProjectile, double>> projectiles;  // latest snapshot + receive time
    std::vector<Burst> bursts;
    std::vector<Flash> flashes;

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
        std::string title = "boyzpvp - " + opt.name + " the " + myClass.name + "   |   kills " +
                            std::to_string(myKills) + "   deaths " + std::to_string(myDeaths);
        SDL_SetWindowTitle(window, title.c_str());
    };
    updateTitle();

    auto onSnapshot = [&](const uint8_t* d, size_t n) {
        MsgSnapshotHeader hdr;
        if (n < sizeof hdr) return;
        std::memcpy(&hdr, d, sizeof hdr);
        if (n < sizeof hdr + hdr.playerCount * sizeof(NetPlayer) + hdr.projCount * sizeof(NetProjectile)) return;
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
                if (np.kills != myKills || np.deaths != myDeaths) {
                    myKills = np.kills;
                    myDeaths = np.deaths;
                    updateTitle();
                }
                pred = np.s;
                while (!pending.empty() && pending.front().seq <= np.lastInputSeq) pending.pop_front();
                for (const InputCmd& c : pending) simulate(pred, c, opt.cls, TICK_DT);  // ignore re-fired abilities
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
                case EV_ATTACK:
                    if (e.a != myId) {
                        remoteAttackT[e.a] = t;
                        if (castsLight(ab)) {
                            LightStyle ls = abilityLight(ab);
                            flashes.push_back({pos + glm::vec3(0, EYE_HEIGHT, 0), abilityColor(ab) * ls.intensity * 1.5f,
                                               t, 0.18, ls.radius * 0.8f});
                        }
                    }
                    break;
                case EV_IMPACT: {
                    float size = std::max(0.5f, abilityInfo(ab).splashRadius * 0.9f);
                    bursts.push_back({pos, abilityColor(ab), t, 0.3, size, abilityLight(ab).radius * 1.3f});
                    break;
                }
                case EV_DAMAGE:
                    if (e.b == myId) hurtT = t;
                    else remoteHurtT[e.b] = t;
                    if (e.a == myId && e.b != myId) hitMarkerT = t;
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
                case EV_KILL:
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
        names[m.id] = m.name;
        if (m.id != myId) {
            std::printf("  %s joined as %s\n", m.name, classInfo(static_cast<PlayerClass>(m.cls)).name);
            std::fflush(stdout);
        }
    };

    while (running) {
        // --- Window / input events -----------------------------------------
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_EVENT_QUIT: running = false; break;
                case SDL_EVENT_KEY_DOWN:
                    if (e.key.scancode == SDL_SCANCODE_F12) wantShot = true;
                    if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
                        if (grabbed) { grabbed = false; SDL_SetWindowRelativeMouseMode(window, false); }
                        else running = false;
                    }
                    break;
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
            }
            if (botMode && pred.alive) {
                float best = 1e9f;
                for (const auto& [id, r] : remotes) {
                    if (r.samples.empty()) continue;
                    const NetPlayer& o = r.samples.back().p;
                    float dx = o.s.x - pred.x, dz = o.s.z - pred.z;
                    float dist = std::hypot(dx, dz);
                    if (!o.s.alive || dist >= best ||
                        !lineOfSight(pred.x, pred.y + EYE_HEIGHT, pred.z, o.s.x, o.s.y + PLAYER_HEIGHT * 0.5f, o.s.z))
                        continue;
                    best = dist;
                    float dy = (o.s.y + PLAYER_HEIGHT * 0.5f) - (pred.y + EYE_HEIGHT - 0.15f);
                    viewYaw = std::atan2(dx, -dz);
                    viewPitch = std::atan2(dy, dist);
                }
                if (best < 1e9f) {
                    cmd.buttons |= BTN_PRIMARY;
                    if (best > 2.0f && opt.cls == PlayerClass::Warrior) cmd.forward = 1;
                }
            }
            cmd.yaw = viewYaw;
            cmd.pitch = viewPitch;

            prevPred = pred;
            if (haveServerState) {
                uint8_t fired = simulate(pred, cmd, opt.cls, TICK_DT);
                if (fired) {
                    int slot = (fired & 1) ? 0 : 1;
                    castT = t;
                    castAbility = myClass.abilities[slot];
                    castKind = abilityInfo(castAbility).kind;
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
        Camera cam{feet + glm::vec3(0, alive ? EYE_HEIGHT : 0.35f, 0), viewYaw, viewPitch};

        std::vector<Billboard> billboards;
        double renderT = t - INTERP_DELAY;
        for (const auto& [id, r] : remotes) {
            if (r.samples.empty() || !r.samples.back().p.s.alive) continue;
            PlayerClass c = static_cast<PlayerClass>(r.samples.back().p.cls);
            Billboard b{sampleRemote(r, renderT), PLAYER_HEIGHT * 0.5f, PLAYER_HEIGHT, classColor(c), SpriteTex::Player};
            auto hurt = remoteHurtT.find(id);
            if (hurt != remoteHurtT.end() && t - hurt->second < 0.15) b.tint *= glm::vec3(2.0f, 0.45f, 0.45f);
            auto atk = remoteAttackT.find(id);
            if (atk != remoteAttackT.end() && t - atk->second < 0.2) b.brightness = 1.5f;
            billboards.push_back(b);
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
        hud.classColor = classColor(opt.cls);
        for (int slot = 0; slot < NUM_ABILITY_SLOTS; ++slot) {
            float total = abilityInfo(myClass.abilities[slot]).cooldownTicks;
            hud.ready[slot] = total > 0 ? 1.0f - pred.cooldown[slot] / total : 1.0f;
        }
        hud.showWeapon = alive;
        hud.weapon = hasSword ? WeaponTex::Sword : WeaponTex::Staff;
        WeaponPose pose = weaponPose(hasSword, castKind, castT < 0 ? -1.0 : t - castT, walkPhase);
        hud.weaponOffset = pose.offset;
        hud.weaponAngle = pose.angle;
        hud.weaponGlow = pose.glow;
        hud.hitMarker = hitMarkerT >= 0 && t - hitMarkerT < 0.15;
        if (!alive) hud.flash = {0.35f, 0.0f, 0.0f, 0.55f};
        else if (hurtT >= 0 && t - hurtT < 0.3) hud.flash = {0.8f, 0.05f, 0.0f, 0.25f * static_cast<float>(1.0 - (t - hurtT) / 0.3)};
        else if (healT >= 0 && t - healT < 0.4) hud.flash = {0.3f, 1.0f, 0.4f, 0.2f * static_cast<float>(1.0 - (t - healT) / 0.4)};

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
