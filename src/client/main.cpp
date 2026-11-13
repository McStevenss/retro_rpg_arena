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

bool connectToServer(const Options& o, ENetHost*& host, ENetPeer*& peer, uint8_t& myId) {
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
            MsgWelcome w;
            std::memcpy(&w, d, sizeof w);
            myId = w.playerId;
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

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) return 1;

    if (enet_initialize() != 0) { std::fprintf(stderr, "enet_initialize failed\n"); return 1; }
    ENetHost* net = nullptr;
    ENetPeer* server = nullptr;
    uint8_t myId = 0;
    if (!connectToServer(opt, net, server, myId)) return 1;
    std::printf("joined as player #%d (%s)\n", myId, classInfo(opt.cls).name);

    if (!SDL_Init(SDL_INIT_VIDEO)) { SDL_Log("SDL_Init: %s", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    std::string title = "boyzpvp - " + opt.name + " the " + classInfo(opt.cls).name;
    SDL_Window* window = SDL_CreateWindow(title.c_str(), 1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
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
    uint32_t inputSeq = 0;
    std::deque<InputCmd> pending;  // sent but not yet acknowledged by the server

    std::map<uint8_t, Remote> remotes;

    auto now = [] { return static_cast<double>(SDL_GetTicksNS()) / 1e9; };
    double last = now(), acc = 0;
    bool running = true;
    bool wantShot = false;
    int shotCount = 0;
    // BOYZ_AUTOSHOT=S: save a screenshot after S seconds and quit (for testing).
    const char* autoShotEnv = SDL_getenv("BOYZ_AUTOSHOT");
    double autoShotAt = autoShotEnv ? now() + std::atof(autoShotEnv) : -1.0;

    auto onSnapshot = [&](const uint8_t* d, size_t n) {
        MsgSnapshotHeader hdr;
        if (n < sizeof hdr) return;
        std::memcpy(&hdr, d, sizeof hdr);
        if (n < sizeof hdr + hdr.count * sizeof(NetPlayer)) return;
        double t = now();
        std::set<uint8_t> seen;
        for (int i = 0; i < hdr.count; ++i) {
            NetPlayer np;
            std::memcpy(&np, d + sizeof hdr + i * sizeof(NetPlayer), sizeof np);
            seen.insert(np.id);
            if (np.id == myId) {
                // Reconcile: take server's authoritative state, then re-apply
                // every input the server hasn't processed yet.
                myHp = np.hp;
                pred = np.s;
                while (!pending.empty() && pending.front().seq <= np.lastInputSeq) pending.pop_front();
                for (const InputCmd& c : pending) simulate(pred, c, opt.cls, TICK_DT);
                pred.yaw = viewYaw;
                pred.pitch = viewPitch;
                if (!haveServerState) {
                    // Adopt the server's spawn facing on first contact.
                    viewYaw = pred.yaw = np.s.yaw;
                    prevPred = pred;
                    haveServerState = true;
                }
            } else {
                auto& r = remotes[np.id];
                r.samples.push_back({t, np});
                while (r.samples.size() > 32) r.samples.pop_front();
            }
        }
        for (auto it = remotes.begin(); it != remotes.end();)
            it = seen.count(it->first) ? std::next(it) : remotes.erase(it);
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
                if (ev.packet->dataLength > 0 && ev.packet->data[0] == MSG_SNAPSHOT)
                    onSnapshot(ev.packet->data, ev.packet->dataLength);
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
            InputCmd cmd{};
            cmd.seq = ++inputSeq;
            if (grabbed) {
                cmd.forward = static_cast<int8_t>((keys[SDL_SCANCODE_W] ? 1 : 0) - (keys[SDL_SCANCODE_S] ? 1 : 0));
                cmd.strafe = static_cast<int8_t>((keys[SDL_SCANCODE_D] ? 1 : 0) - (keys[SDL_SCANCODE_A] ? 1 : 0));
                if (keys[SDL_SCANCODE_SPACE]) cmd.buttons |= BTN_JUMP;
            }
            cmd.yaw = viewYaw;
            cmd.pitch = viewPitch;

            prevPred = pred;
            if (haveServerState) simulate(pred, cmd, opt.cls, TICK_DT);
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
        float alpha = static_cast<float>(acc / TICK_DT);
        glm::vec3 feet = glm::mix(glm::vec3(prevPred.x, prevPred.y, prevPred.z), glm::vec3(pred.x, pred.y, pred.z), alpha);
        Camera cam{feet + glm::vec3(0, EYE_HEIGHT, 0), viewYaw, viewPitch};

        std::vector<SpriteDraw> sprites;
        double renderT = t - INTERP_DELAY;
        for (const auto& [id, r] : remotes) {
            if (r.samples.empty()) continue;
            PlayerClass c = static_cast<PlayerClass>(r.samples.back().p.cls);
            sprites.push_back({sampleRemote(r, renderT), classColor(c)});
        }

        renderer.renderFrame(cam, sprites, myHp / classInfo(opt.cls).maxHp);
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
