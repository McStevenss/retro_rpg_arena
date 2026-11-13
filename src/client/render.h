#pragma once
#include <glm/glm.hpp>
#include <vector>

struct Camera {
    glm::vec3 eye;
    float yaw;
    float pitch;
};

struct SpriteDraw {
    glm::vec3 feet;
    glm::vec3 tint;
};

// Renders the world into a small offscreen framebuffer (the "retro" resolution)
// and then scales it up with nearest-neighbour filtering to the window.
class Renderer {
public:
    bool init(int internalW, int internalH);
    void shutdown();
    void renderFrame(const Camera& cam, const std::vector<SpriteDraw>& sprites, float hpFrac);
    void present(int windowW, int windowH);
    // Writes the low-res frame to a binary PPM file.
    bool saveScreenshot(const char* path);

private:
    void buildLevel();
    void fillRect(int x, int y, int w, int h, float r, float g, float b);

    int w_ = 0, h_ = 0;
    unsigned fbo_ = 0, fboColor_ = 0, fboDepth_ = 0;
    unsigned prog_ = 0;
    int uViewProj_ = -1, uCamPos_ = -1, uTint_ = -1, uFogColor_ = -1, uFogStart_ = -1, uFogEnd_ = -1;
    unsigned levelVao_ = 0, levelVbo_ = 0;
    int wallFirst_ = 0, wallCount_ = 0, floorFirst_ = 0, floorCount_ = 0, ceilFirst_ = 0, ceilCount_ = 0;
    unsigned spriteVao_ = 0, spriteVbo_ = 0;
    unsigned texWall_ = 0, texFloor_ = 0, texCeil_ = 0, texPlayer_ = 0;
};
