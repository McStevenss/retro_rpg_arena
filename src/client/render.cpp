#include "render.h"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "font.h"
#include "game.h"
#include "gl.h"
#include "textures.h"

using namespace glfn;

namespace {

const char* kVertexSrc = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in float aShade;
layout(location = 3) in vec3 aNormal;
uniform mat4 uViewProj;
uniform vec3 uCamPos;
out vec2 vUV;
out float vShade;
out float vDist;
out vec3 vPos;
out vec3 vNormal;
void main() {
    vUV = aUV;
    vShade = aShade;
    vPos = aPos;
    vNormal = aNormal;
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
in vec3 vPos;
in vec3 vNormal;  // zero = light from all directions (sprites)
uniform sampler2D uTex;
uniform vec3 uTint;
uniform vec3 uFogColor;
uniform float uFogStart;
uniform float uFogEnd;
uniform float uFogScale;  // 0 = no fog (HUD overlays)
uniform float uAlpha;     // < 1 only for blended overlays
const int MAX_LIGHTS = 32;
uniform int uLightCount;
uniform vec4 uLightPos[MAX_LIGHTS];    // xyz = position, w = radius
uniform vec3 uLightColor[MAX_LIGHTS];
uniform float uAmbient;
uniform int uEmissive;                 // 1 = unlit (glows, HUD)
uniform vec3 uEmissiveColor;
out vec4 fragColor;
const float LEVELS = 20.0;
const float bayer[16] = float[16](0., 8., 2., 10., 12., 4., 14., 6., 3., 11., 1., 9., 15., 7., 13., 5.);
void main() {
    vec4 c = texture(uTex, vUV);
    if (c.a < 0.5) discard;
    float tintMask = c.a < 0.9 ? 1.0 : 0.0;
    vec3 base = mix(c.rgb, c.rgb * uTint, tintMask);
    vec3 light;
    if (uEmissive == 1) {
        light = vShade * uEmissiveColor;
    } else {
        light = vec3(vShade * uAmbient);
        bool hasNormal = dot(vNormal, vNormal) > 0.25;
        for (int i = 0; i < uLightCount; ++i) {
            vec3 toLight = uLightPos[i].xyz - vPos;
            float dist = length(toLight);
            float radius = uLightPos[i].w;
            if (dist >= radius) continue;
            float atten = 1.0 - dist / radius;
            atten *= atten;
            // Faces pointing away get nothing, so light doesn't bleed through walls.
            float ndl = hasNormal ? max(dot(vNormal, toLight / max(dist, 1e-3)), 0.0) : 1.0;
            light += uLightColor[i] * atten * ndl;
        }
        // Soft limit: past 1.0, extra light rolls off toward 1.8 instead of
        // clipping everything to white at point-blank explosions.
        vec3 over = max(light - 1.0, 0.0);
        light = min(light, vec3(1.0)) + over / (1.0 + over * 1.25);
    }
    vec3 col = base * light;
    float fog = clamp((vDist - uFogStart) / (uFogEnd - uFogStart), 0.0, 1.0) * uFogScale;
    col = mix(col, uFogColor, fog);
    int idx = (int(gl_FragCoord.x) & 3) + (int(gl_FragCoord.y) & 3) * 4;
    float d = (bayer[idx] / 16.0 - 0.5) / LEVELS;
    col = floor((col + d) * LEVELS + 0.5) / LEVELS;
    fragColor = vec4(col, uAlpha);
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
// Vertex layout: position(3) uv(2) shade(1) normal(3).
constexpr int VERT_FLOATS = 9;

void addQuad(std::vector<float>& v, glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, float shade,
             glm::vec3 n = glm::vec3(0)) {
    auto push = [&](glm::vec3 p, float u, float t) { v.insert(v.end(), {p.x, p.y, p.z, u, t, shade, n.x, n.y, n.z}); };
    push(a, 0, 0); push(b, 1, 0); push(c, 1, 1);
    push(a, 0, 0); push(c, 1, 1); push(d, 0, 1);
}

void setupVertexLayout() {
    const GLsizei stride = VERT_FLOATS * sizeof(float);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(3 * sizeof(float)));
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(5 * sizeof(float)));
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(6 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glEnableVertexAttribArray(3);
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
    uFogScale_ = glGetUniformLocation(prog_, "uFogScale");
    uAlpha_ = glGetUniformLocation(prog_, "uAlpha");
    uLightCount_ = glGetUniformLocation(prog_, "uLightCount");
    uLightPos_ = glGetUniformLocation(prog_, "uLightPos");
    uLightColor_ = glGetUniformLocation(prog_, "uLightColor");
    uAmbient_ = glGetUniformLocation(prog_, "uAmbient");
    uEmissive_ = glGetUniformLocation(prog_, "uEmissive");
    uEmissiveColor_ = glGetUniformLocation(prog_, "uEmissiveColor");
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
    texOrb_ = makeTexture(makeOrbSprite(), false);
    texSword_ = makeTexture(makeSwordSprite(), false);
    texStaff_ = makeTexture(makeStaffSprite(), false);
    texWhite_ = makeTexture(makeWhite(), false);
    texTorch_ = makeTexture(makeTorchSprite(), false);
    texFont_ = makeTexture(makeFontAtlas(), false);

    buildLevel();

    glGenVertexArrays(1, &spriteVao_);
    glBindVertexArray(spriteVao_);
    glGenBuffers(1, &spriteVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, spriteVbo_);
    glBufferData(GL_ARRAY_BUFFER, 6 * VERT_FLOATS * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    setupVertexLayout();
    glBindVertexArray(0);
    return true;
}

namespace {

// Quad with explicit UVs. Corners: top-left, top-right, bottom-right, bottom-left.
void addQuadUV(std::vector<float>& v, const glm::vec3 (&p)[4], const glm::vec2 (&uv)[4], float shade, glm::vec3 n) {
    const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int i : order) v.insert(v.end(), {p[i].x, p[i].y, p[i].z, uv[i].x, uv[i].y, shade, n.x, n.y, n.z});
}

// Parts of `a` not covered by any span of `c`.
std::vector<game::Span> subtractSpans(game::Span a, const game::Column& c) {
    std::vector<game::Span> out{a};
    for (int i = 0; i < c.numSpans; ++i) {
        const game::Span& b = c.spans[i];
        std::vector<game::Span> next;
        for (const game::Span& o : out) {
            if (b.y1 <= o.y0 || b.y0 >= o.y1) { next.push_back(o); continue; }
            if (b.y0 > o.y0) next.push_back({o.y0, b.y0});
            if (b.y1 < o.y1) next.push_back({b.y1, o.y1});
        }
        out.swap(next);
    }
    out.erase(std::remove_if(out.begin(), out.end(), [](const game::Span& s) { return s.y1 - s.y0 < 1e-3f; }),
              out.end());
    return out;
}

// Vertical wall quad from (ax,az) to (bx,bz) covering heights [y0,y1].
// Texture v follows world height so bricks line up across stacked walls.
void addWall(std::vector<float>& v, float ax, float az, float bx, float bz, float y0, float y1, float u0, float u1,
             float shade, glm::vec3 n) {
    const float T = game::TILE;
    glm::vec3 p[4] = {{ax, y1, az}, {bx, y1, bz}, {bx, y0, bz}, {ax, y0, az}};
    glm::vec2 uv[4] = {{u0, -y1 / T}, {u1, -y1 / T}, {u1, -y0 / T}, {u0, -y0 / T}};
    addQuadUV(v, p, uv, shade, n);
}

void addHorizontal(std::vector<float>& v, float x0, float z0, float x1, float z1, float y, float shade, float ny) {
    const float T = game::TILE;
    glm::vec3 p[4] = {{x0, y, z0}, {x1, y, z0}, {x1, y, z1}, {x0, y, z1}};
    glm::vec2 uv[4] = {{x0 / T, z0 / T}, {x1 / T, z0 / T}, {x1 / T, z1 / T}, {x0 / T, z1 / T}};
    addQuadUV(v, p, uv, shade, glm::vec3(0, ny, 0));
}

}  // namespace

void Renderer::buildLevel() {
    using namespace game;
    std::vector<float> floorV, ceilV, wallV;
    const float T = TILE;
    // Neighbour offsets and the shade of a face pointing that way (flat fake lighting).
    const int DX[4] = {0, 0, -1, 1}, DZ[4] = {-1, 1, 0, 0};
    const float SHADE[4] = {0.9f, 1.0f, 0.7f, 0.8f};

    for (int tz = 0; tz < mapHeight(); ++tz) {
        for (int tx = 0; tx < mapWidth(); ++tx) {
            const Column& c = column(tx, tz);
            const float x0 = tx * T, x1 = x0 + T, z0 = tz * T, z1 = z0 + T;

            for (int i = 0; i < c.numSpans; ++i) {
                const Span& s = c.spans[i];
                bool underStairs = c.hasRamp && std::fabs(s.y1 - c.ramp.h0) < 1e-3f;
                if (s.y1 < WORLD_TOP && !underStairs) addHorizontal(floorV, x0, z0, x1, z1, s.y1, 0.85f, 1.0f);
                if (s.y0 > WORLD_BOTTOM) addHorizontal(ceilV, x0, z0, x1, z1, s.y0, 0.6f, -1.0f);

                // Side faces wherever the neighbouring column is open at these heights.
                for (int d = 0; d < 4; ++d) {
                    const Column& n = column(tx + DX[d], tz + DZ[d]);
                    const glm::vec3 nrm(DX[d], 0, DZ[d]);
                    for (const Span& e : subtractSpans(s, n)) {
                        if (d == 0) addWall(wallV, x0, z0, x1, z0, e.y0, e.y1, 0, 1, SHADE[d], nrm);
                        if (d == 1) addWall(wallV, x1, z1, x0, z1, e.y0, e.y1, 0, 1, SHADE[d], nrm);
                        if (d == 2) addWall(wallV, x0, z1, x0, z0, e.y0, e.y1, 0, 1, SHADE[d], nrm);
                        if (d == 3) addWall(wallV, x1, z0, x1, z1, e.y0, e.y1, 0, 1, SHADE[d], nrm);
                    }
                }
            }

            if (!c.hasRamp) continue;
            // Stairs: drawn as real steps (physics uses a smooth ramp; the
            // difference is at most half a step).
            const Ramp& r = c.ramp;
            const int steps = std::max(1, static_cast<int>(std::lround(std::fabs(r.h1 - r.h0) / 0.25f)));
            // Map (along, lateral) in [0,1]^2 to world x/z.
            auto P = [&](float f, float l) -> glm::vec2 {
                if (r.dx > 0) return {x0 + f * T, z0 + l * T};
                if (r.dx < 0) return {x1 - f * T, z0 + l * T};
                if (r.dz > 0) return {x0 + l * T, z0 + f * T};
                return {x0 + l * T, z1 - f * T};
            };
            const int lx = r.dx != 0 ? 0 : 1, lz = r.dx != 0 ? 1 : 0;  // lateral neighbour direction
            const Column& sideA = column(tx - lx, tz - lz);
            const Column& sideB = column(tx + lx, tz + lz);
            const Column& front = column(tx + r.dx, tz + r.dz);
            for (int k = 0; k < steps; ++k) {
                float f0 = static_cast<float>(k) / steps, f1 = static_cast<float>(k + 1) / steps;
                float yLow = r.h0 + (r.h1 - r.h0) * f0, yTop = r.h0 + (r.h1 - r.h0) * f1;
                glm::vec2 a = P(f0, 0), b = P(f0, 1), cc = P(f1, 1), dd = P(f1, 0);
                // Tread.
                glm::vec3 tp[4] = {{a.x, yTop, a.y}, {b.x, yTop, b.y}, {cc.x, yTop, cc.y}, {dd.x, yTop, dd.y}};
                glm::vec2 tuv[4] = {{0, f0}, {1, f0}, {1, f1}, {0, f1}};
                addQuadUV(floorV, tp, tuv, 0.85f, glm::vec3(0, 1, 0));
                // Riser.
                addWall(wallV, a.x, a.y, b.x, b.y, yLow, yTop, 0, 1, 0.75f, glm::vec3(-r.dx, 0, -r.dz));
                // Step sides where the neighbour is open.
                for (const Span& e : subtractSpans({r.h0, yTop}, sideA))
                    addWall(wallV, a.x, a.y, dd.x, dd.y, e.y0, e.y1, f0, f1, 0.7f, glm::vec3(-lx, 0, -lz));
                for (const Span& e : subtractSpans({r.h0, yTop}, sideB))
                    addWall(wallV, b.x, b.y, cc.x, cc.y, e.y0, e.y1, f0, f1, 0.7f, glm::vec3(lx, 0, lz));
            }
            // Back face at the top of the stairs, if there's open space beyond it.
            glm::vec2 fa = P(1, 0), fb = P(1, 1);
            for (const Span& e : subtractSpans({r.h0, r.h1}, front))
                addWall(wallV, fa.x, fa.y, fb.x, fb.y, e.y0, e.y1, 0, 1, 0.8f, glm::vec3(r.dx, 0, r.dz));
        }
    }
    std::vector<float> all;
    wallFirst_ = 0;
    wallCount_ = static_cast<int>(wallV.size() / VERT_FLOATS);
    floorFirst_ = wallCount_;
    floorCount_ = static_cast<int>(floorV.size() / VERT_FLOATS);
    ceilFirst_ = floorFirst_ + floorCount_;
    ceilCount_ = static_cast<int>(ceilV.size() / VERT_FLOATS);
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

void Renderer::drawQuad(glm::vec3 tl, glm::vec3 tr, glm::vec3 br, glm::vec3 bl, float shade) {
    std::vector<float> v;
    v.reserve(6 * VERT_FLOATS);
    addQuad(v, tl, tr, br, bl, shade);
    glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(v.size() * sizeof(float)), v.data());
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void Renderer::drawText(const HudText& t, glm::vec3 color, int dx, int dy) {
    const float aw = static_cast<float>(FONT_COLS * FONT_CELL_W), ah = static_cast<float>(FONT_ROWS * FONT_CELL_H);
    glUniform3f(uTint_, color.r, color.g, color.b);
    float x = static_cast<float>(t.x + dx);
    const float top = static_cast<float>(h_ - (t.y + dy));
    std::vector<float> v;
    v.reserve(t.text.size() * 6 * VERT_FLOATS);
    for (char c : t.text) {
        int cell = glyphIndex(c);
        float u0 = (cell % FONT_COLS) * FONT_CELL_W / aw, v0 = (cell / FONT_COLS) * FONT_CELL_H / ah;
        float u1 = u0 + GLYPH_W / aw, v1 = v0 + GLYPH_H / ah;
        glm::vec3 p[4] = {{x, top, 0}, {x + GLYPH_W, top, 0}, {x + GLYPH_W, top - GLYPH_H, 0}, {x, top - GLYPH_H, 0}};
        glm::vec2 uv[4] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
        addQuadUV(v, p, uv, 1.0f, glm::vec3(0));
        x += GLYPH_ADVANCE;
    }
    // One draw per string: stream into a scratch buffer sized for it.
    if (v.empty()) return;
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(v.size() * sizeof(float)), v.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(v.size() / VERT_FLOATS));
}

void Renderer::setEmissive(bool on, glm::vec3 color) {
    glUniform1i(uEmissive_, on ? 1 : 0);
    glUniform3f(uEmissiveColor_, color.r, color.g, color.b);
}

void Renderer::renderFrame(const Camera& cam, const std::vector<Billboard>& billboards,
                           const std::vector<PointLight>& lights, const Hud& hud) {
    const glm::vec3 fog(0.03f, 0.026f, 0.034f);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, w_, h_);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
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
    glUniform1f(uFogStart_, 8.0f);
    glUniform1f(uFogEnd_, 42.0f);
    glUniform1f(uFogScale_, 1.0f);
    glUniform1f(uAlpha_, 1.0f);
    glUniform1f(uAmbient_, 0.72f);
    setEmissive(false);

    // Use the lights that matter most to this view: nearest to the camera,
    // accounting for how far each one reaches.
    std::vector<const PointLight*> picked;
    for (const PointLight& l : lights) picked.push_back(&l);
    auto score = [&](const PointLight* l) { return glm::distance(l->pos, cam.eye) - l->radius; };
    std::sort(picked.begin(), picked.end(), [&](auto* a, auto* b) { return score(a) < score(b); });
    if (picked.size() > MAX_LIGHTS) picked.resize(MAX_LIGHTS);
    float lpos[MAX_LIGHTS * 4], lcol[MAX_LIGHTS * 3];
    for (size_t i = 0; i < picked.size(); ++i) {
        lpos[i * 4 + 0] = picked[i]->pos.x;
        lpos[i * 4 + 1] = picked[i]->pos.y;
        lpos[i * 4 + 2] = picked[i]->pos.z;
        lpos[i * 4 + 3] = picked[i]->radius;
        lcol[i * 3 + 0] = picked[i]->color.r;
        lcol[i * 3 + 1] = picked[i]->color.g;
        lcol[i * 3 + 2] = picked[i]->color.b;
    }
    const GLsizei count = static_cast<GLsizei>(picked.size());
    glUniform1i(uLightCount_, count);
    if (count > 0) {
        glUniform4fv(uLightPos_, count, lpos);
        glUniform3fv(uLightColor_, count, lcol);
    }

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
    const glm::vec3 right(std::cos(cam.yaw), 0, std::sin(cam.yaw));
    for (const Billboard& b : billboards) {
        glBindTexture(GL_TEXTURE_2D, b.tex == SpriteTex::Player ? texPlayer_ : b.tex == SpriteTex::Orb ? texOrb_ : texTorch_);
        setEmissive(b.tex != SpriteTex::Player);  // glows and torches are light sources themselves
        glUniform3f(uTint_, b.tint.r, b.tint.g, b.tint.b);
        glm::vec3 base = b.base;
        if (b.tex == SpriteTex::Orb) {
            // Pull glows toward the camera so they don't get half-buried in walls/floor.
            glm::vec3 toCam = cam.eye - (base + glm::vec3(0, b.height * 0.5f, 0));
            float d = glm::length(toCam);
            if (d > 1e-3f) base += toCam / d * std::min(b.width * 0.5f, d * 0.5f);
        }
        glm::vec3 l = base - right * (b.width * 0.5f), r = base + right * (b.width * 0.5f);
        glm::vec3 up(0, b.height, 0);
        drawQuad(l + up, r + up, r, l, b.brightness);
    }

    drawHud(hud);
    glBindVertexArray(0);
}

void Renderer::drawHud(const Hud& hud) {
    // 2D pass in framebuffer pixel coordinates (origin bottom-left).
    glDisable(GL_DEPTH_TEST);
    glm::mat4 ortho = glm::ortho(0.0f, static_cast<float>(w_), 0.0f, static_cast<float>(h_), -1.0f, 1.0f);
    glUniformMatrix4fv(uViewProj_, 1, GL_FALSE, glm::value_ptr(ortho));
    glUniform1f(uFogScale_, 0.0f);
    setEmissive(true, hud.weaponLight);

    if (hud.showWeapon) {
        // Weapon pivots around its handle, which sits below the bottom-right of the screen.
        const float scale = 2.2f, hw = 16 * scale, hgt = 64 * scale;
        glm::vec2 pivot(w_ * 0.72f + hud.weaponOffset.x, -12.0f + hud.weaponOffset.y);
        float c = std::cos(hud.weaponAngle), s = std::sin(hud.weaponAngle);
        auto P = [&](float x, float y) { return glm::vec3(pivot.x + x * c - y * s, pivot.y + x * s + y * c, 0); };
        glBindTexture(GL_TEXTURE_2D, hud.weapon == WeaponTex::Sword ? texSword_ : texStaff_);
        glm::vec3 tint = hud.classColor * hud.weaponGlow;
        glUniform3f(uTint_, tint.r, tint.g, tint.b);
        drawQuad(P(-hw, hgt), P(hw, hgt), P(hw, 0), P(-hw, 0), 1.0f);
    }

    if (hud.flash.a > 0.0f) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBindTexture(GL_TEXTURE_2D, texWhite_);
        setEmissive(true);
        glUniform3f(uTint_, hud.flash.r, hud.flash.g, hud.flash.b);
        glUniform1f(uAlpha_, hud.flash.a);
        float W = static_cast<float>(w_), H = static_cast<float>(h_);
        drawQuad({0, H, 0}, {W, H, 0}, {W, 0, 0}, {0, 0, 0}, 1.0f);
        glUniform1f(uAlpha_, 1.0f);
        glDisable(GL_BLEND);
    }

    // Panels and text (kill feed, scoreboard, messages).
    if (!hud.rects.empty()) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBindTexture(GL_TEXTURE_2D, texWhite_);
        setEmissive(true);
        for (const HudRect& r : hud.rects) {
            glUniform3f(uTint_, r.color.r, r.color.g, r.color.b);
            glUniform1f(uAlpha_, r.color.a);
            float x0 = static_cast<float>(r.x), x1 = static_cast<float>(r.x + r.w);
            float y0 = static_cast<float>(h_ - r.y), y1 = static_cast<float>(h_ - r.y - r.h);
            drawQuad({x0, y0, 0}, {x1, y0, 0}, {x1, y1, 0}, {x0, y1, 0}, 1.0f);
        }
        glUniform1f(uAlpha_, 1.0f);
        glDisable(GL_BLEND);
    }
    if (!hud.texts.empty()) {
        glBindTexture(GL_TEXTURE_2D, texFont_);
        setEmissive(true);
        for (const HudText& t : hud.texts) drawText(t, glm::vec3(0.02f), 1, 1);  // shadow
        for (const HudText& t : hud.texts) drawText(t, t.color, 0, 0);
        // Restore the sprite buffer's size for later single-quad draws.
        glBufferData(GL_ARRAY_BUFFER, 6 * VERT_FLOATS * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    }

    // Pixel-exact HUD bits drawn with scissored clears.
    glEnable(GL_SCISSOR_TEST);
    int cx = w_ / 2, cy = h_ / 2;
    fillRect(cx - 3, cy, 7, 1, 0.9f, 0.85f, 0.7f);
    fillRect(cx, cy - 3, 1, 7, 0.9f, 0.85f, 0.7f);
    if (hud.hitMarker) {
        for (int sx : {-1, 1})
            for (int sy : {-1, 1}) {
                fillRect(cx + sx * 5, cy + sy * 5, 1, 1, 1.0f, 0.25f, 0.2f);
                fillRect(cx + sx * 6, cy + sy * 6, 1, 1, 1.0f, 0.25f, 0.2f);
            }
    }

    const int barW = 60, barH = 5;
    fillRect(6, 6, barW + 2, barH + 2, 0.1f, 0.05f, 0.03f);
    fillRect(7, 7, barW, barH, 0.25f, 0.04f, 0.04f);
    int fill = static_cast<int>(barW * std::clamp(hud.hpFrac, 0.0f, 1.0f));
    if (fill > 0) fillRect(7, 7, fill, barH, 0.75f, 0.1f, 0.08f);

    // Ability cooldowns: two squares bottom-centre that fill up as they recharge.
    const int box = 12;
    for (int slot = 0; slot < 2; ++slot) {
        int x = cx - box - 2 + slot * (box + 4), y = 6;
        float r = std::clamp(hud.ready[slot], 0.0f, 1.0f);
        fillRect(x - 1, y - 1, box + 2, box + 2, 0.1f, 0.08f, 0.06f);
        glm::vec3 c = hud.classColor * (r >= 1.0f ? 1.0f : 0.35f);
        fillRect(x, y, box, box, 0.18f, 0.16f, 0.14f);
        int h = static_cast<int>(box * r);
        if (h > 0) fillRect(x, y, box, h, c.r, c.g, c.b);
    }
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
    GLuint texs[] = {texWall_, texFloor_, texCeil_, texPlayer_, texOrb_, texSword_, texStaff_, texWhite_, texTorch_, texFont_, fboColor_};
    glDeleteTextures(sizeof(texs) / sizeof(texs[0]), texs);
    glDeleteRenderbuffers(1, &fboDepth_);
    glDeleteFramebuffers(1, &fbo_);
    glDeleteBuffers(1, &levelVbo_);
    glDeleteBuffers(1, &spriteVbo_);
    glDeleteVertexArrays(1, &levelVao_);
    glDeleteVertexArrays(1, &spriteVao_);
    glDeleteProgram(prog_);
}
