#pragma once
#include <glm/glm.hpp>
#include <string>
#include <vector>

struct Camera {
    glm::vec3 eye;
    float yaw;
    float pitch;
};

// The world projection the renderer uses (for placing things like damage
// numbers on screen).
glm::mat4 cameraViewProj(const Camera& cam, float aspect);

enum class SpriteTex { Player, Orb, Torch, Skeleton, PlayerCorpse, SkeletonCorpse };

// Camera-facing sprite in the world. `base` is the bottom centre.
struct Billboard {
    glm::vec3 base;
    float width, height;
    glm::vec3 tint;
    SpriteTex tex;
    float brightness = 1.0f;
    float opacity = 1.0f;  // < 1 = dithered see-through (stealth shimmer)
    // Characters: pick one of 8 views from the angle between the camera and
    // the way they face (Player / Skeleton sheets only).
    bool directional = false;
    float facingYaw = 0;   // same convention as PlayerState::yaw
    int frame = 0;         // animation row of the sheet (FRAME_* in textures.h)
};

// Dynamic point light (spells, impacts, ...). Colour may exceed 1 for bright lights.
struct PointLight {
    glm::vec3 pos;
    glm::vec3 color;
    float radius;
};
constexpr int MAX_LIGHTS = 32;  // nearest ones to the camera are used each frame

enum class WeaponTex { Sword, Staff, Dagger };

// HUD primitives in framebuffer pixels, origin top-left.
struct HudRect {
    int x, y, w, h;
    glm::vec4 color;  // a < 1 = translucent
};

struct HudText {
    int x, y;  // top-left of the first glyph
    std::string text;
    glm::vec3 color;
};

struct Hud {
    float hpFrac = 1;
    float ready[3] = {1, 1, 1};   // per ability slot: 0 = just used, 1 = ready
    glm::vec3 classColor{1};
    bool showWeapon = true;
    bool showStatus = true;       // crosshair, health bar, ability cooldowns
    WeaponTex weapon = WeaponTex::Sword;
    glm::vec2 weaponOffset{0};    // pixels, relative to resting position
    float weaponAngle = 0;        // radians, + = top tilts left
    float weaponGlow = 1;         // multiplies the tint (staff orb)
    float weaponOpacity = 1;      // < 1 = dithered (you are stealthed)
    glm::vec3 weaponLight{1};     // light falling on the weapon (from nearby spells)
    glm::vec4 flash{0};           // full-screen colour overlay, a = opacity
    bool hitMarker = false;
    std::vector<HudRect> rects;   // drawn first (panels, highlights)
    std::vector<HudText> texts;   // drawn on top, with a 1 px drop shadow
};

// Renders the world into a small offscreen framebuffer (the "retro" resolution)
// and then scales it up with nearest-neighbour filtering to the window.
class Renderer {
public:
    bool init(int internalW, int internalH);
    void shutdown();
    void renderFrame(const Camera& cam, const std::vector<Billboard>& billboards,
                     const std::vector<PointLight>& lights, const Hud& hud);
    void present(int windowW, int windowH);
    // Writes the low-res frame to a binary PPM file.
    bool saveScreenshot(const char* path);

private:
    void buildLevel();
    void drawQuad(glm::vec3 tl, glm::vec3 tr, glm::vec3 br, glm::vec3 bl, float shade);
    void drawQuadUV(glm::vec3 tl, glm::vec3 tr, glm::vec3 br, glm::vec3 bl, glm::vec2 uv0, glm::vec2 uv1, float shade);
    void setEmissive(bool on, glm::vec3 color = glm::vec3(1));
    void drawText(const HudText& t, glm::vec3 color, int dx, int dy);
    void drawHud(const Hud& hud);
    void fillRect(int x, int y, int w, int h, float r, float g, float b);

    int w_ = 0, h_ = 0;
    unsigned fbo_ = 0, fboColor_ = 0, fboDepth_ = 0;
    unsigned prog_ = 0;
    int uViewProj_ = -1, uCamPos_ = -1, uTint_ = -1, uFogColor_ = -1, uFogStart_ = -1, uFogEnd_ = -1;
    int uFogScale_ = -1, uAlpha_ = -1, uStipple_ = -1;
    int uLightCount_ = -1, uLightPos_ = -1, uLightColor_ = -1, uAmbient_ = -1, uEmissive_ = -1, uEmissiveColor_ = -1;
    unsigned levelVao_ = 0, levelVbo_ = 0;
    int wallFirst_ = 0, wallCount_ = 0, floorFirst_ = 0, floorCount_ = 0, ceilFirst_ = 0, ceilCount_ = 0;
    unsigned spriteVao_ = 0, spriteVbo_ = 0;
    unsigned texWall_ = 0, texFloor_ = 0, texCeil_ = 0, texPlayer_ = 0;
    unsigned texSkeleton_ = 0, texDagger_ = 0, texPlayerCorpse_ = 0, texSkeletonCorpse_ = 0;
    unsigned texOrb_ = 0, texSword_ = 0, texStaff_ = 0, texWhite_ = 0, texTorch_ = 0, texFont_ = 0;
};
