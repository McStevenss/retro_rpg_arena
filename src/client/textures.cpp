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

Image makePlayerSprite() {
    const uint8_t TINT = 191;
    Image img = blank(32, 64);
    // Robe / body: trapezoid, tinted by class color.
    for (int y = 16; y < 52; ++y) {
        float t = (y - 16) / 36.0f;
        float half = 6.0f + 4.5f * t;
        for (int x = 0; x < 32; ++x) {
            float dx = x + 0.5f - 16.0f;
            if (std::fabs(dx) > half) continue;
            float fold = 0.85f + 0.15f * std::sin(dx * 1.3f);
            float shade = (dx < 0 ? 1.0f : 0.82f) * fold * (0.92f + 0.08f * rnd(x, y, 9));
            put(img, x, y, 225 * shade, 225 * shade, 225 * shade, TINT);
        }
    }
    // Arms (tinted sleeves) and hands (skin).
    for (int y = 18; y < 42; ++y) {
        for (int x : {6, 7, 8, 23, 24, 25}) {
            bool hand = y >= 38;
            float s = (x < 16 ? 1.0f : 0.8f);
            if (hand) put(img, x, y, 215 * s, 175 * s, 140 * s);
            else put(img, x, y, 205 * s, 205 * s, 205 * s, TINT);
        }
    }
    // Belt.
    for (int y = 31; y < 33; ++y)
        for (int x = 9; x < 23; ++x) put(img, x, y, 70, 45, 25);
    // Legs / boots.
    for (int y = 52; y < 64; ++y)
        for (int x : {11, 12, 13, 18, 19, 20}) put(img, x, y, 60, 42, 28);
    // Head.
    for (int y = 3; y < 16; ++y) {
        for (int x = 10; x < 22; ++x) {
            float dx = x + 0.5f - 16.0f, dy = y + 0.5f - 9.5f;
            if (dx * dx + dy * dy > 30.0f) continue;
            float s = dx < 1.5f ? 1.0f : 0.82f;
            if (y < 6) put(img, x, y, 70 * s, 50 * s, 30 * s);  // hair
            else put(img, x, y, 220 * s, 180 * s, 145 * s);
        }
    }
    put(img, 14, 9, 20, 20, 20);
    put(img, 18, 9, 20, 20, 20);

    return outlined(img);
}

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
