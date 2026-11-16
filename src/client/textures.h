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
// Character sprite sheets: SPRITE_VIEWS 32x64 cells left to right (front,
// front three-quarter, side, back three-quarter, back); turned views face
// screen-left and get mirrored for the other side.
// Rows top to bottom are animation frames: standing, two walking strides,
// and collapsing (shown briefly when they die, before the corpse).
constexpr int SPRITE_VIEWS = 5;
constexpr int SPRITE_FRAMES = 4;
constexpr int FRAME_STAND = 0, FRAME_STRIDE_A = 1, FRAME_STRIDE_B = 2, FRAME_COLLAPSE = 3;
Image makePlayerSheet();        // white-ish humanoid, robe tinted by class
Image makePlayerCorpse();       // 64x32: lying in a pool of blood (robe tinted)
Image makeSkeletonCorpse();     // 64x32: a heap of bones
Image makeOrbSprite();          // glowing ball for projectiles / impacts
Image makeSkeletonSheet();      // necromancer minion; eye glow tinted
Image makeSwordSprite();        // first-person weapon (warrior)
Image makeDaggerSprite();       // first-person weapon (rogue)
Image makeStaffSprite();        // first-person weapon (casters), orb tinted
Image makeTorchSprite();        // wall torch: flame (tinted) on a wooden handle
Image makeWhite();              // 1x1 white (tintable), for flat-colour overlays
