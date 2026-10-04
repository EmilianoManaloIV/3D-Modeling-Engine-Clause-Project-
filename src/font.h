#pragma once
// Built-in 5x7 bitmap font (printable ASCII 32..126), packed at startup into a
// single-channel texture atlas. Keeps the executable free of font files.
#include <cstdint>
#include <vector>

namespace font {
constexpr int kGlyphW = 5, kGlyphH = 9;  // 7 rows above the baseline + 2 for descenders
constexpr int kCellW = 6, kCellH = 10;   // atlas cell / horizontal advance
constexpr int kCols = 16;                // glyphs per atlas row
constexpr int kAtlasW = 128, kAtlasH = 64;
constexpr int kWhiteX = 124, kWhiteY = 60;  // centre of a solid white block (untextured quads)

void buildAtlas(std::vector<uint8_t>& pixels);  // kAtlasW * kAtlasH bytes, row 0 = top
}  // namespace font
