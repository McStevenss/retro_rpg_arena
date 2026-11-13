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
// Sprites use the alpha channel as a mask: 255 = draw as-is, ~191 = multiply
// by the tint colour, 0 = transparent.
Image makePlayerSprite();       // white-ish humanoid, robe tinted by class
Image makeOrbSprite();          // glowing ball for projectiles / impacts
Image makeSwordSprite();        // first-person weapon (warrior)
Image makeStaffSprite();        // first-person weapon (casters), orb tinted
Image makeTorchSprite();        // wall torch: flame (tinted) on a wooden handle
Image makeWhite();              // 1x1 white (tintable), for flat-colour overlays
