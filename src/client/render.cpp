#include "render.h"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "game.h"
#include "gl.h"
#include "textures.h"

using namespace glfn;

namespace {

const char* kVertexSrc = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in float aShade;
uniform mat4 uViewProj;
uniform vec3 uCamPos;
out vec2 vUV;
out float vShade;
out float vDist;
void main() {
    vUV = aUV;
    vShade = aShade;
    vDist = distance(aPos, uCamPos);
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)";

// Daggerfall-ish look: flat per-face shading, dark distance fog, and a crude
// "limited palette" via ordered dithering + colour quantisation.
const char* kFragmentSrc = R"(#version 330 core
in vec2 vUV;
in float vShade;
in float vDist;
uniform sampler2D uTex;
uniform vec3 uTint;
uniform vec3 uFogColor;
uniform float uFogStart;
uniform float uFogEnd;
out vec4 fragColor;
const float LEVELS = 20.0;
const float bayer[16] = float[16](0., 8., 2., 10., 12., 4., 14., 6., 3., 11., 1., 9., 15., 7., 13., 5.);
void main() {
    vec4 c = texture(uTex, vUV);
    if (c.a < 0.5) discard;
    float tintMask = c.a < 0.9 ? 1.0 : 0.0;
    vec3 col = mix(c.rgb, c.rgb * uTint, tintMask) * vShade;
    float fog = clamp((vDist - uFogStart) / (uFogEnd - uFogStart), 0.0, 1.0);
    col = mix(col, uFogColor, fog);
    int idx = (int(gl_FragCoord.x) & 3) + (int(gl_FragCoord.y) & 3) * 4;
    float d = (bayer[idx] / 16.0 - 0.5) / LEVELS;
    col = floor((col + d) * LEVELS + 0.5) / LEVELS;
    fragColor = vec4(col, 1.0);
}
)";

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        SDL_Log("shader compile error:\n%s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint linkProgram(const char* vsSrc, const char* fsSrc) {
    GLuint vs = compile(GL_VERTEX_SHADER, vsSrc);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fsSrc);
    if (!vs || !fs) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        SDL_Log("program link error:\n%s", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

GLuint makeTexture(const Image& img, bool repeat) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.w, img.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLint wrap = repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    return t;
}

// a = top-left, b = top-right, c = bottom-right, d = bottom-left
void addQuad(std::vector<float>& v, glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, float shade) {
    auto push = [&](glm::vec3 p, float u, float t) { v.insert(v.end(), {p.x, p.y, p.z, u, t, shade}); };
    push(a, 0, 0); push(b, 1, 0); push(c, 1, 1);
    push(a, 0, 0); push(c, 1, 1); push(d, 0, 1);
}

void setupVertexLayout() {
    const GLsizei stride = 6 * sizeof(float);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(3 * sizeof(float)));
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(5 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
}

}  // namespace

bool Renderer::init(int internalW, int internalH) {
    w_ = internalW;
    h_ = internalH;

    prog_ = linkProgram(kVertexSrc, kFragmentSrc);
    if (!prog_) return false;
    uViewProj_ = glGetUniformLocation(prog_, "uViewProj");
    uCamPos_ = glGetUniformLocation(prog_, "uCamPos");
    uTint_ = glGetUniformLocation(prog_, "uTint");
    uFogColor_ = glGetUniformLocation(prog_, "uFogColor");
    uFogStart_ = glGetUniformLocation(prog_, "uFogStart");
    uFogEnd_ = glGetUniformLocation(prog_, "uFogEnd");
    glUseProgram(prog_);
    glUniform1i(glGetUniformLocation(prog_, "uTex"), 0);

    // Low-res offscreen target.
    glGenTextures(1, &fboColor_);
    glBindTexture(GL_TEXTURE_2D, fboColor_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w_, h_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenRenderbuffers(1, &fboDepth_);
    glBindRenderbuffer(GL_RENDERBUFFER, fboDepth_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w_, h_);
    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fboColor_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fboDepth_);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        SDL_Log("offscreen framebuffer incomplete");
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    texWall_ = makeTexture(makeStoneWall(), true);
    texFloor_ = makeTexture(makeFlagstoneFloor(), true);
    texCeil_ = makeTexture(makeWoodCeiling(), true);
    texPlayer_ = makeTexture(makePlayerSprite(), false);

    buildLevel();

    glGenVertexArrays(1, &spriteVao_);
    glBindVertexArray(spriteVao_);
    glGenBuffers(1, &spriteVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, spriteVbo_);
    glBufferData(GL_ARRAY_BUFFER, 36 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    setupVertexLayout();
    glBindVertexArray(0);
    return true;
}

void Renderer::buildLevel() {
    using namespace game;
    std::vector<float> floorV, ceilV, wallV;
    const float H = WALL_H;
    for (int tz = 0; tz < mapHeight(); ++tz) {
        for (int tx = 0; tx < mapWidth(); ++tx) {
            if (isSolid(tx, tz)) continue;
            float x0 = tx * TILE, x1 = x0 + TILE, z0 = tz * TILE, z1 = z0 + TILE;
            addQuad(floorV, {x0, 0, z0}, {x1, 0, z0}, {x1, 0, z1}, {x0, 0, z1}, 0.85f);
            addQuad(ceilV, {x0, H, z0}, {x1, H, z0}, {x1, H, z1}, {x0, H, z1}, 0.6f);
            // Different brightness per facing = classic flat "fake lighting".
            if (isSolid(tx, tz - 1)) addQuad(wallV, {x0, H, z0}, {x1, H, z0}, {x1, 0, z0}, {x0, 0, z0}, 1.0f);
            if (isSolid(tx, tz + 1)) addQuad(wallV, {x1, H, z1}, {x0, H, z1}, {x0, 0, z1}, {x1, 0, z1}, 0.9f);
            if (isSolid(tx - 1, tz)) addQuad(wallV, {x0, H, z1}, {x0, H, z0}, {x0, 0, z0}, {x0, 0, z1}, 0.75f);
            if (isSolid(tx + 1, tz)) addQuad(wallV, {x1, H, z0}, {x1, H, z1}, {x1, 0, z1}, {x1, 0, z0}, 0.7f);
        }
    }
    std::vector<float> all;
    wallFirst_ = 0;
    wallCount_ = static_cast<int>(wallV.size() / 6);
    floorFirst_ = wallCount_;
    floorCount_ = static_cast<int>(floorV.size() / 6);
    ceilFirst_ = floorFirst_ + floorCount_;
    ceilCount_ = static_cast<int>(ceilV.size() / 6);
    all.insert(all.end(), wallV.begin(), wallV.end());
    all.insert(all.end(), floorV.begin(), floorV.end());
    all.insert(all.end(), ceilV.begin(), ceilV.end());

    glGenVertexArrays(1, &levelVao_);
    glBindVertexArray(levelVao_);
    glGenBuffers(1, &levelVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, levelVbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(all.size() * sizeof(float)), all.data(), GL_STATIC_DRAW);
    setupVertexLayout();
    glBindVertexArray(0);
}

void Renderer::fillRect(int x, int y, int w, int h, float r, float g, float b) {
    glScissor(x, y, w, h);
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void Renderer::renderFrame(const Camera& cam, const std::vector<SpriteDraw>& sprites, float hpFrac) {
    const glm::vec3 fog(0.02f, 0.018f, 0.025f);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, w_, h_);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(fog.r, fog.g, fog.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    glm::vec3 dir(std::sin(cam.yaw) * std::cos(cam.pitch), std::sin(cam.pitch),
                  -std::cos(cam.yaw) * std::cos(cam.pitch));
    glm::mat4 proj = glm::perspective(glm::radians(70.0f), static_cast<float>(w_) / h_, 0.05f, 100.0f);
    glm::mat4 view = glm::lookAt(cam.eye, cam.eye + dir, glm::vec3(0, 1, 0));
    glm::mat4 vp = proj * view;

    glUseProgram(prog_);
    glUniformMatrix4fv(uViewProj_, 1, GL_FALSE, glm::value_ptr(vp));
    glUniform3f(uCamPos_, cam.eye.x, cam.eye.y, cam.eye.z);
    glUniform3f(uTint_, 1, 1, 1);
    glUniform3f(uFogColor_, fog.r, fog.g, fog.b);
    glUniform1f(uFogStart_, 5.0f);
    glUniform1f(uFogEnd_, 32.0f);

    glBindVertexArray(levelVao_);
    glBindTexture(GL_TEXTURE_2D, texWall_);
    glDrawArrays(GL_TRIANGLES, wallFirst_, wallCount_);
    glBindTexture(GL_TEXTURE_2D, texFloor_);
    glDrawArrays(GL_TRIANGLES, floorFirst_, floorCount_);
    glBindTexture(GL_TEXTURE_2D, texCeil_);
    glDrawArrays(GL_TRIANGLES, ceilFirst_, ceilCount_);

    // Billboards: always face the camera around the Y axis (Daggerfall style).
    // TODO: 8-directional sprite frames chosen from (camera angle - player yaw).
    glBindVertexArray(spriteVao_);
    glBindBuffer(GL_ARRAY_BUFFER, spriteVbo_);
    glBindTexture(GL_TEXTURE_2D, texPlayer_);
    const glm::vec3 right(std::cos(cam.yaw), 0, std::sin(cam.yaw));
    const float halfW = game::PLAYER_HEIGHT * 0.25f, height = game::PLAYER_HEIGHT;
    for (const SpriteDraw& s : sprites) {
        glm::vec3 l = s.feet - right * halfW, r = s.feet + right * halfW, up(0, height, 0);
        std::vector<float> v;
        addQuad(v, l + up, r + up, r, l, 1.0f);
        glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(v.size() * sizeof(float)), v.data());
        glUniform3f(uTint_, s.tint.r, s.tint.g, s.tint.b);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    glBindVertexArray(0);

    // HUD drawn with scissored clears: cheap, pixel-exact at the retro resolution.
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);
    int cx = w_ / 2, cy = h_ / 2;
    fillRect(cx - 3, cy, 7, 1, 0.9f, 0.85f, 0.7f);
    fillRect(cx, cy - 3, 1, 7, 0.9f, 0.85f, 0.7f);
    const int barW = 60, barH = 5;
    fillRect(6, 6, barW + 2, barH + 2, 0.1f, 0.05f, 0.03f);
    fillRect(7, 7, barW, barH, 0.25f, 0.04f, 0.04f);
    int fill = static_cast<int>(barW * std::clamp(hpFrac, 0.0f, 1.0f));
    if (fill > 0) fillRect(7, 7, fill, barH, 0.75f, 0.1f, 0.08f);
    glDisable(GL_SCISSOR_TEST);
}

void Renderer::present(int windowW, int windowH) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, windowW, windowH);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    float scale = std::min(static_cast<float>(windowW) / w_, static_cast<float>(windowH) / h_);
    int dw = static_cast<int>(w_ * scale), dh = static_cast<int>(h_ * scale);
    int dx = (windowW - dw) / 2, dy = (windowH - dh) / 2;
    glBlitFramebuffer(0, 0, w_, h_, dx, dy, dx + dw, dy + dh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool Renderer::saveScreenshot(const char* path) {
    std::vector<uint8_t> px(static_cast<size_t>(w_) * h_ * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glReadPixels(0, 0, w_, h_, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w_, h_);
    for (int y = h_ - 1; y >= 0; --y)
        for (int x = 0; x < w_; ++x) std::fwrite(&px[(static_cast<size_t>(y) * w_ + x) * 4], 1, 3, f);
    std::fclose(f);
    return true;
}

void Renderer::shutdown() {
    GLuint texs[] = {texWall_, texFloor_, texCeil_, texPlayer_, fboColor_};
    glDeleteTextures(5, texs);
    glDeleteRenderbuffers(1, &fboDepth_);
    glDeleteFramebuffers(1, &fbo_);
    glDeleteBuffers(1, &levelVbo_);
    glDeleteBuffers(1, &spriteVbo_);
    glDeleteVertexArrays(1, &levelVao_);
    glDeleteVertexArrays(1, &spriteVao_);
    glDeleteProgram(prog_);
}
