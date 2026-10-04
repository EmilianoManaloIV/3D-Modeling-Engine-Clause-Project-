#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Writes an RGB image as PNG (uncompressed deflate - no zlib dependency).
// Rows are given bottom-up, the order glReadPixels returns them.
bool writePNG(const std::string& path, int width, int height, const std::vector<uint8_t>& rgbBottomUp);
