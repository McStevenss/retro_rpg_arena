// Tiny built-in 5x7 pixel font (uppercase, digits, common punctuation).
// Lowercase letters are drawn as uppercase; unknown characters as '?'.
#pragma once
#include <string>

#include "textures.h"

constexpr int GLYPH_W = 5;
constexpr int GLYPH_H = 7;
constexpr int GLYPH_ADVANCE = 6;  // glyph width + 1 px spacing
constexpr int LINE_HEIGHT = 9;

// Atlas layout: 16 x 6 cells of 6x8 px covering ASCII 32..127.
constexpr int FONT_COLS = 16;
constexpr int FONT_ROWS = 6;
constexpr int FONT_CELL_W = 6;
constexpr int FONT_CELL_H = 8;

int textWidth(const std::string& s);
int glyphIndex(char c);  // cell index in the atlas
Image makeFontAtlas();   // glyph pixels are white + tintable, background transparent
