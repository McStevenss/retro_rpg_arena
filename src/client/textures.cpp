#include "textures.h"

#include <algorithm>
#include <cmath>

namespace {

uint32_t hash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

float rnd(int x, int y, int seed) {
    uint32_t h = hash(static_cast<uint32_t>(x) * 73856093U ^ static_cast<uint32_t>(y) * 19349663U ^
                      static_cast<uint32_t>(seed) * 83492791U);
    return (h & 0xffff) / 65535.0f;
}

Image blank(int w, int h) {
    Image img;
    img.w = w;
    img.h = h;
    img.rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    return img;
}

void put(Image& img, int x, int y, float r, float g, float b, uint8_t a = 255) {
    if (x < 0 || y < 0 || x >= img.w || y >= img.h) return;
    uint8_t* p = &img.rgba[(static_cast<size_t>(y) * img.w + x) * 4];
    p[0] = static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f));
    p[1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 255.0f));
    p[2] = static_cast<uint8_t>(std::clamp(b, 0.0f, 255.0f));
    p[3] = a;
}

uint8_t alphaAt(const Image& img, int x, int y) {
    if (x < 0 || y < 0 || x >= img.w || y >= img.h) return 0;
    return img.rgba[(static_cast<size_t>(y) * img.w + x) * 4 + 3];
}

// 1px dark outline so sprites read against dark walls.
Image outlined(const Image& img) {
    Image out = img;
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x)
            if (alphaAt(img, x, y) == 0 && (alphaAt(img, x - 1, y) || alphaAt(img, x + 1, y) ||
                                            alphaAt(img, x, y - 1) || alphaAt(img, x, y + 1)))
                put(out, x, y, 15, 12, 10);
    return out;
}

}  // namespace

Image makeStoneWall() {
    Image img = blank(64, 64);
    const int bh = 16, bw = 32;
    for (int y = 0; y < 64; ++y) {
        int row = y / bh;
        int off = (row % 2) * (bw / 2);
        for (int x = 0; x < 64; ++x) {
            int bx = (x + off) & 63;
            int col = bx / bw;
            int ly = y % bh, lx = bx % bw;
            if (ly == 0 || lx == 0) {
                float n = 0.8f + 0.4f * rnd(x, y, 1);
                put(img, x, y, 38 * n, 34 * n, 30 * n);
                continue;
            }
            float t = 0.7f + 0.3f * rnd(row, col, 2);
            float n = 0.82f + 0.18f * rnd(x, y, 3);
            float edge = 1.0f;
            if (ly == 1 || lx == 1) edge = 1.18f;
            if (ly == bh - 1 || lx == bw - 1) edge = 0.7f;
            float k = t * n * edge;
            put(img, x, y, 122 * k, 112 * k, 98 * k);
        }
    }
    return img;
}

Image makeFlagstoneFloor() {
    Image img = blank(64, 64);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            int sx = x / 32, sy = y / 32;
            int lx = x % 32, ly = y % 32;
            if (lx == 0 || ly == 0) {
                put(img, x, y, 30, 27, 24);
                continue;
            }
            float t = 0.75f + 0.25f * rnd(sx, sy, 4);
            float n = 0.8f + 0.2f * rnd(x, y, 5);
            float edge = (lx == 31 || ly == 31) ? 0.75f : (lx == 1 || ly == 1) ? 1.12f : 1.0f;
            float k = t * n * edge;
            put(img, x, y, 92 * k, 84 * k, 72 * k);
        }
    }
    return img;
}

Image makeWoodCeiling() {
    Image img = blank(64, 64);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            int plank = x / 8;
            if (x % 8 == 0) {
                put(img, x, y, 22, 14, 8);
                continue;
            }
            float t = 0.75f + 0.25f * rnd(plank, 0, 6);
            float grain = 0.85f + 0.15f * std::sin(y * 0.35f + rnd(plank, 1, 7) * 6.0f + (x % 8) * 0.6f);
            float n = 0.9f + 0.1f * rnd(x, y, 8);
            float k = t * grain * n;
            put(img, x, y, 96 * k, 62 * k, 34 * k);
        }
    }
    return img;
}

namespace {

constexpr uint8_t TINT_A = 191;

// Directional views, as stored left to right in a sprite sheet. The turned
// views face screen-left; the renderer mirrors them for the other side.
enum View { FRONT, FRONT_34, SIDE, BACK_34, BACK };
// Animation frames, stored top to bottom.
enum Pose { STAND, STRIDE_A, STRIDE_B };

int iround(float v) { return static_cast<int>(std::lround(v)); }

// One view/pose of a robed person into a 32x64 image.
void paintPerson(Image& img, int view, int pose) {
    auto P = [&](int x, int y, float r, float g, float b, uint8_t a = 255) { put(img, x, y, r, g, b, a); };
    const float widthK[5] = {1.0f, 0.85f, 0.6f, 0.85f, 1.0f};
    const float k = widthK[view];
    const bool back = view >= BACK_34;
    // +1 / -1 = which leg is forward; 0 = standing.
    const int sw = pose == STRIDE_A ? 1 : pose == STRIDE_B ? -1 : 0;

    // `swing` slides the hand sideways (forward/back on turned views).
    auto arm = [&](int x0, int w, float s, int yEnd, int swing) {
        for (int y = 18; y < yEnd; ++y) {
            int xo = iround(swing * (y - 18) / static_cast<float>(yEnd - 18));
            for (int x = x0; x < x0 + w; ++x) {
                if (y >= yEnd - 4) P(x + xo, y, 215 * s, 175 * s, 140 * s);  // hand
                else P(x + xo, y, 205 * s, 205 * s, 205 * s, TINT_A);
            }
        }
    };
    // `slant` moves the foot sideways, `lift` raises it (a bent knee seen from the front).
    auto leg = [&](int x0, int w, float s, int slant, int lift) {
        for (int y = 52; y < 64 - lift; ++y) {
            int xo = iround(slant * (y - 52) / 11.0f);
            for (int x = x0; x < x0 + w; ++x) P(x + xo, y, 60 * s, 42 * s, 28 * s);
        }
    };

    // Far arm behind the body first (turned views). Arms swing against the legs.
    if (view == FRONT_34 || view == BACK_34) arm(22, 2, 0.65f, 41, -2 * sw);
    if (view == SIDE) arm(17, 2, 0.6f, 40, -3 * sw);

    // Legs / boots (far leg darker).
    if (view == FRONT || view == BACK) { leg(11, 3, 1.0f, 0, sw > 0 ? 3 : 0); leg(18, 3, 1.0f, 0, sw < 0 ? 3 : 0); }
    else if (view == SIDE) { leg(17, 2, 0.7f, 3 * sw, 0); leg(13, 3, 1.0f, -3 * sw, 0); }
    else { leg(18, 2, 0.75f, 2 * sw, 0); leg(11, 3, 1.0f, -2 * sw, 0); }

    // Robe / body: trapezoid, tinted by class colour, lit from the left.
    for (int y = 16; y < 52; ++y) {
        float t = (y - 16) / 36.0f;
        float half = (6.0f + 4.5f * t) * k;
        float cx = view == SIDE ? 15.5f : 16.0f;
        for (int x = 0; x < 32; ++x) {
            float dx = x + 0.5f - cx;
            if (std::fabs(dx) > half) continue;
            float fold = 0.85f + 0.15f * std::sin(dx * 1.3f);
            float shade = (dx < 0 ? 1.0f : 0.82f) * fold * (0.92f + 0.08f * rnd(x, y, 9));
            if (back && std::fabs(dx) < 1.0f && y < 40) shade *= 0.8f;  // seam down the back
            P(x, y, 225 * shade, 225 * shade, 225 * shade, TINT_A);
        }
    }
    // Belt (only across the visible robe).
    for (int y = 31; y < 33; ++y) {
        float half = (6.0f + 4.5f * (y - 16) / 36.0f) * k - 1.0f;
        float cx = view == SIDE ? 15.5f : 16.0f;
        for (int x = 0; x < 32; ++x)
            if (std::fabs(x + 0.5f - cx) <= half) P(x, y, 70, 45, 25);
    }

    // Near arms, over the body. From the front, the arm swinging forward looks shorter.
    if (view == FRONT || view == BACK) { arm(6, 3, 1.0f, sw < 0 ? 40 : 42, 0); arm(23, 3, 0.8f, sw > 0 ? 40 : 42, 0); }
    else if (view == SIDE) arm(14, 3, 0.95f, 42, 3 * sw);
    else arm(7, 3, 1.0f, 42, 2 * sw);

    // Head. Hair covers more of it the further the person faces away.
    const float hx = view == FRONT || view == BACK ? 16.0f : 15.5f;
    for (int y = 3; y < 16; ++y) {
        for (int x = 9; x < 23; ++x) {
            float dx = x + 0.5f - hx, dy = y + 0.5f - 9.5f;
            if (dx * dx + dy * dy > 30.0f) continue;
            float s = dx < 1.5f ? 1.0f : 0.82f;
            bool hair = y < 6;
            switch (view) {
                case FRONT_34: hair = hair || dx > 3.5f; break;
                case SIDE: hair = hair || dx > 0.0f; break;
                case BACK_34: hair = hair || dx > -3.5f; break;
                case BACK: hair = true; break;
                default: break;
            }
            if (hair) P(x, y, 70 * s, 50 * s, 30 * s);
            else P(x, y, 220 * s, 180 * s, 145 * s);
        }
    }
    switch (view) {  // face
        case FRONT: P(14, 9, 20, 20, 20); P(18, 9, 20, 20, 20); break;
        case FRONT_34: P(12, 9, 20, 20, 20); P(16, 9, 20, 20, 20); break;
        case SIDE: P(12, 9, 20, 20, 20); P(9, 10, 220, 180, 145); break;  // eye and nose
        default: break;
    }
}

// One view/pose of a skeleton; eye sockets are tinted (they glow).
void paintSkeleton(Image& img, int view, int pose) {
    auto bone = [&](int x, int y, float s = 1.0f) {
        float n = s * (0.9f + 0.1f * rnd(x, y, 21));
        put(img, x, y, 215 * n, 205 * n, 175 * n);
    };
    auto P = [&](int x, int y, float r, float g, float b, uint8_t a = 255) { put(img, x, y, r, g, b, a); };
    const float widthK[5] = {1.0f, 0.75f, 0.4f, 0.75f, 1.0f};
    const float k = widthK[view];
    const bool back = view >= BACK_34;
    const int sw = pose == STRIDE_A ? 1 : pose == STRIDE_B ? -1 : 0;

    auto arm = [&](int x, float s, int swing, int yEnd = 40) {
        for (int y = 18; y < yEnd; ++y) bone(x + iround(swing * (y - 18) / static_cast<float>(yEnd - 18)), y, s);
        int hx = x + swing;
        bone(hx - 1, yEnd, s); bone(hx + 1, yEnd, s); bone(hx - 1, yEnd + 1, s * 0.8f); bone(hx + 1, yEnd + 1, s * 0.8f);
    };
    auto leg = [&](int x, float s, int slant, int lift) {
        for (int y = 44; y < 64 - lift; ++y) bone(x + iround(slant * (y - 44) / 19.0f), y, s);
        bone(x + iround(slant * 9 / 19.0f) + 1, 53, s);  // knee
        int fx0 = x + slant, fy = 63 - lift;
        for (int fx = fx0 - 2; fx < fx0 + 2; ++fx) bone(fx, fy, s);
    };

    // Far arm / leg first on turned views.
    if (view == FRONT_34 || view == BACK_34) arm(21, 0.65f, -2 * sw);
    if (view == SIDE) arm(17, 0.6f, -3 * sw);
    // Legs.
    if (view == FRONT || view == BACK) { leg(12, 1.0f, 0, sw > 0 ? 3 : 0); leg(19, 0.8f, 0, sw < 0 ? 3 : 0); }
    else if (view == SIDE) { leg(17, 0.65f, 3 * sw, 0); leg(14, 1.0f, -3 * sw, 0); }
    else { leg(18, 0.7f, 2 * sw, 0); leg(12, 1.0f, -2 * sw, 0); }

    // Skull with a jaw.
    const float sx = view == FRONT || view == BACK ? 16.0f : 15.0f;
    for (int y = 4; y < 17; ++y)
        for (int x = 9; x < 23; ++x) {
            float dx = x + 0.5f - sx, dy = y + 0.5f - 10.0f;
            if (dx * dx + dy * dy * 1.1f > 34.0f) continue;
            bone(x, y, dx < 1.5f ? 1.0f : 0.82f);
        }
    switch (view) {
        case FRONT:
            for (int x = 13; x < 19; ++x) P(x, 14, 40, 30, 25);  // teeth gap
            for (int y = 8; y < 11; ++y)
                for (int x : {12, 13, 18, 19}) P(x, y, 255, 255, 255, TINT_A);
            break;
        case FRONT_34:
            for (int x = 11; x < 16; ++x) P(x, 14, 40, 30, 25);
            for (int y = 8; y < 11; ++y)
                for (int x : {11, 12, 16}) P(x, y, 255, 255, 255, TINT_A);
            break;
        case SIDE:
            for (int x = 10; x < 13; ++x) P(x, 14, 40, 30, 25);
            for (int y = 8; y < 11; ++y)
                for (int x : {11, 12}) P(x, y, 255, 255, 255, TINT_A);
            break;
        default:  // back of the skull: a crack
            for (int dy : {7, 8, 9}) P(static_cast<int>(sx) + (dy == 8 ? 2 : 1), dy, 90, 80, 65);
            break;
    }

    // Spine (with vertebra bumps from behind).
    const int spineX = view == SIDE ? 16 : 15;
    for (int y = 17; y < 40; ++y) {
        bone(spineX, y);
        bone(spineX + 1, y, 0.85f);
        if (back && y % 3 == 0) bone(spineX - 1, y, 0.9f), bone(spineX + 2, y, 0.7f);
    }
    // Ribs.
    for (int r = 0; r < 4; ++r) {
        int y = 20 + r * 4;
        int half = std::max(1, static_cast<int>((7 - r) * k + 0.5f));
        int cx = view == SIDE ? 15 : 16;
        for (int x = cx - half; x < cx + half; ++x) bone(x, y, x < cx ? 1.0f : 0.82f);
        bone(cx - half, y + 1, 0.9f);
        bone(cx + half - 1, y + 1, 0.75f);
    }
    // Shoulders and near arms hanging forward, with claws.
    int sh = static_cast<int>(8 * k + 0.5f);
    for (int x = 16 - sh; x < 16 + sh; ++x) bone(x, 18, 0.95f);
    if (view == FRONT || view == BACK) { arm(8, 1.0f, 0, sw < 0 ? 38 : 40); arm(23, 0.8f, 0, sw > 0 ? 38 : 40); }
    else if (view == SIDE) arm(13, 0.95f, 3 * sw);
    else arm(9, 1.0f, 2 * sw);
    // Pelvis.
    int ph = std::max(2, static_cast<int>(5 * k + 0.5f));
    for (int y = 40; y < 44; ++y)
        for (int x = 16 - ph; x < 16 + ph; ++x)
            if (y == 40 || x == 16 - ph || x == 15 + ph) bone(x, y, x < 16 ? 1.0f : 0.82f);
    // Tattered dark loincloth.
    for (int y = 41; y < 50; ++y)
        for (int x = 16 - ph + 1; x < 16 + ph - 1; ++x)
            if (rnd(x, y, 22) > 0.25f + (y - 41) * 0.07f) P(x, y, 45, 30, 50);
}

using Painter = void (*)(Image&, int view, int pose);

Image paintCell(Painter paint, int view, int pose) {
    Image c = blank(32, 64);
    paint(c, view, pose);
    return c;
}

void blit(Image& dst, const Image& src, int ox, int oy) {
    for (int y = 0; y < src.h; ++y)
        for (int x = 0; x < src.w; ++x) {
            const uint8_t* p = &src.rgba[(static_cast<size_t>(y) * src.w + x) * 4];
            if (p[3]) put(dst, ox + x, oy + y, p[0], p[1], p[2], p[3]);
        }
}

// Knees buckling: squashed toward the floor and leaning forward (screen-left).
Image collapsed(const Image& c) {
    Image out = blank(c.w, c.h);
    const int top = 24;  // squashed body spans [top, 64)
    for (int y = top; y < c.h; ++y) {
        int sy = (y - top) * c.h / (c.h - top);
        int lean = iround((c.h - y) * 0.18f);
        for (int x = 0; x < c.w; ++x) {
            int sx = x + lean;
            if (sx < 0 || sx >= c.w) continue;
            const uint8_t* p = &c.rgba[(static_cast<size_t>(sy) * c.w + sx) * 4];
            if (p[3]) put(out, x, y, p[0], p[1], p[2], p[3]);
        }
    }
    return out;
}

// Rows: stand, stride A, stride B, collapsing (death). Each cell is outlined
// on its own so outlines don't bleed into the neighbouring frame.
Image sheet(Painter paint) {
    Image img = blank(32 * SPRITE_VIEWS, 64 * SPRITE_FRAMES);
    for (int v = 0; v < SPRITE_VIEWS; ++v) {
        for (int pose = STAND; pose <= STRIDE_B; ++pose) blit(img, outlined(paintCell(paint, v, pose)), v * 32, pose * 64);
        blit(img, outlined(collapsed(paintCell(paint, v, STAND))), v * 32, FRAME_COLLAPSE * 64);
    }
    return img;
}

// Lying on the floor: the side view turned 90 degrees (head to the left),
// over a pool of blood.
Image corpse(Painter paint, bool blood) {
    Image side = paintCell(paint, SIDE, STAND);
    Image out = blank(64, 32);
    // Rotated: source row -> dest column, source column -> dest row. Find how
    // far down it can go so the body rests on the bottom edge.
    int maxRow = 0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 32; ++x)
            if (alphaAt(side, x, y)) maxRow = std::max(maxRow, x);
    const int shift = 29 - maxRow;
    if (blood)
        for (int y = 24; y < 32; ++y)
            for (int x = 0; x < 64; ++x) {
                float ex = (x + 0.5f - 34.0f) / 26.0f, ey = (y + 0.5f - 29.5f) / 2.6f;
                if (ex * ex + ey * ey > 1.0f - 0.25f * rnd(x, y, 31)) continue;
                float n = 0.75f + 0.25f * rnd(x, y, 32);
                put(out, x, y, 95 * n, 8 * n, 6 * n);
            }
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 32; ++x) {
            const uint8_t* p = &side.rgba[(static_cast<size_t>(y) * 32 + x) * 4];
            if (!p[3]) continue;
            // Bones scatter a little as the skeleton falls apart.
            int jitter = blood ? 0 : iround((rnd(y / 5, 0, 33) - 0.5f) * 4.0f);
            put(out, y, x + shift + jitter, p[0], p[1], p[2], p[3]);
        }
    return outlined(out);
}

}  // namespace

Image makePlayerSheet() { return sheet(paintPerson); }
Image makeSkeletonSheet() { return sheet(paintSkeleton); }
Image makePlayerCorpse() { return corpse(paintPerson, true); }
Image makeSkeletonCorpse() { return corpse(paintSkeleton, false); }

Image makeOrbSprite() {
    const uint8_t TINT = 191;
    Image img = blank(16, 16);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            float dx = x + 0.5f - 8.0f, dy = y + 0.5f - 8.0f;
            float d = std::sqrt(dx * dx + dy * dy);
            if (d < 2.6f) put(img, x, y, 255, 255, 240);  // hot white core
            else if (d < 7.5f) {
                float k = 1.0f - (d - 2.6f) / 4.9f * 0.55f;
                k *= 0.9f + 0.1f * rnd(x, y, 10);
                put(img, x, y, 255 * k, 255 * k, 255 * k, TINT);
            }
        }
    }
    return img;
}

Image makeDaggerSprite() {
    Image img = blank(32, 64);
    // Short, wide blade, leaf-shaped toward the tip.
    for (int y = 18; y < 48; ++y) {
        int half = y < 22 ? (y - 18) / 2 : (y < 40 ? 2 : 1);
        for (int x = 16 - half - 1; x <= 15 + half + 1; ++x) {
            float n = 0.93f + 0.07f * rnd(x, y, 23);
            if (x <= 15 - half) put(img, x, y, 230 * n, 235 * n, 240 * n);
            else if (x >= 16 + half) put(img, x, y, 95 * n, 100 * n, 110 * n);
            else put(img, x, y, 160 * n, 166 * n, 178 * n);
        }
    }
    // Small guard.
    for (int y = 48; y < 51; ++y)
        for (int x = 10; x < 22; ++x) put(img, x, y, 60, 55, 60);
    // Wrapped grip.
    for (int y = 51; y < 60; ++y)
        for (int x = 14; x < 18; ++x) {
            float k = ((y + x) % 3 == 0) ? 0.6f : 1.0f;
            put(img, x, y, 45 * k, 35 * k, 30 * k);
        }
    for (int y = 60; y < 63; ++y)
        for (int x = 14; x < 18; ++x) put(img, x, y, 90, 85, 95);
    return outlined(img);
}

Image makeSwordSprite() {
    Image img = blank(32, 64);
    // Blade with a tapered tip.
    for (int y = 0; y < 45; ++y) {
        int x0 = 14, x1 = 17;
        if (y < 2) { x0 = 15; x1 = 16; }
        for (int x = x0; x <= x1; ++x) {
            float n = 0.93f + 0.07f * rnd(x, y, 11);
            if (x == 14) put(img, x, y, 225 * n, 230 * n, 235 * n);
            else if (x == 17) put(img, x, y, 105 * n, 110 * n, 120 * n);
            else put(img, x, y, 170 * n, 176 * n, 186 * n);
        }
    }
    // Crossguard.
    for (int y = 45; y < 49; ++y)
        for (int x = 5; x < 27; ++x) {
            float k = (y == 45) ? 1.2f : (y == 48 ? 0.7f : 1.0f);
            put(img, x, y, 170 * k, 125 * k, 45 * k);
        }
    // Leather grip.
    for (int y = 49; y < 59; ++y)
        for (int x = 14; x < 18; ++x) {
            float k = ((y + x) % 3 == 0) ? 0.7f : 1.0f;
            put(img, x, y, 85 * k, 52 * k, 30 * k);
        }
    // Pommel.
    for (int y = 59; y < 63; ++y)
        for (int x = 13; x < 19; ++x) put(img, x, y, 175, 130, 50);
    return outlined(img);
}

Image makeStaffSprite() {
    const uint8_t TINT = 191;
    Image img = blank(32, 64);
    // Shaft.
    for (int y = 14; y < 64; ++y)
        for (int x = 14; x < 18; ++x) {
            float k = (x == 14 ? 1.15f : x == 17 ? 0.7f : 1.0f) * (0.88f + 0.12f * rnd(x, y / 3, 12));
            if (y % 11 == 0) k *= 0.75f;  // knots
            put(img, x, y, 112 * k, 74 * k, 40 * k);
        }
    // Claws holding the orb.
    for (int y = 7; y < 16; ++y) {
        put(img, 9, y, 100, 66, 36);
        put(img, 10, y, 120, 80, 44);
        put(img, 21, y, 90, 60, 32);
        put(img, 22, y, 80, 54, 30);
    }
    // Orb (tinted by class colour, brightened by the renderer when casting).
    for (int y = 1; y < 17; ++y)
        for (int x = 9; x < 23; ++x) {
            float dx = x + 0.5f - 16.0f, dy = y + 0.5f - 9.0f;
            float d = std::sqrt(dx * dx + dy * dy);
            if (d > 6.2f) continue;
            float k = 1.0f - d / 6.2f * 0.45f;
            put(img, x, y, 245 * k, 245 * k, 245 * k, TINT);
        }
    put(img, 14, 6, 255, 255, 255);
    put(img, 13, 7, 255, 255, 255);
    return outlined(img);
}

Image makeTorchSprite() {
    const uint8_t TINT = 191;
    Image img = blank(16, 32);
    // Handle, narrowing toward the bottom.
    for (int y = 15; y < 32; ++y) {
        int half = y < 24 ? 2 : 1;
        for (int x = 8 - half; x < 8 + half; ++x) {
            float k = (x < 8 ? 1.0f : 0.7f) * (0.85f + 0.15f * rnd(x, y, 13));
            put(img, x, y, 95 * k, 60 * k, 32 * k);
        }
    }
    // Metal cup holding the flame.
    for (int y = 12; y < 16; ++y)
        for (int x = 4; x < 12; ++x) put(img, x, y, 70, 64, 60);
    // Flame: teardrop with a hot core (untinted) and an outer tongue (tinted).
    for (int y = 0; y < 13; ++y) {
        for (int x = 0; x < 16; ++x) {
            float fy = (13 - y) / 13.0f;                 // 0 at cup, 1 at tip
            float half = 4.5f * std::sin(3.14159f * std::sqrt(fy) * 0.95f) + 0.3f;
            float dx = std::fabs(x + 0.5f - 8.0f);
            if (dx > half) continue;
            if (dx < half * 0.45f && fy < 0.6f) put(img, x, y, 255, 245, 200);
            else put(img, x, y, 255, 255 - 40 * fy, 230, TINT);
        }
    }
    return img;
}

Image makeWhite() {
    Image img = blank(1, 1);
    put(img, 0, 0, 255, 255, 255, 191);  // marked tintable, so overlays take the tint colour
    return img;
}
