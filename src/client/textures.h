// Procedurally generated placeholder art. Swap for real hand-drawn 64x64
// textures / sprite sheets (PNG via stb_image) when you have them.
#pragma once
#include <cstdint>
#include <vector>

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;  // row 0 = top of image
};

Image makeStoneWall();
Image makeFlagstoneFloor();
Image makeWoodCeiling();
// White-ish humanoid. Alpha 255 = draw as-is, alpha ~191 = tint with class
// color, alpha 0 = transparent.
Image makePlayerSprite();
