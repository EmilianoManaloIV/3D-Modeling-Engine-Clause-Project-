#pragma once
// Image decoding for textures, without third-party libraries:
//   PNG  - all colour types and bit depths (8/16-bit, palette, gray, alpha,
//          Adam7 interlacing), with our own zlib inflate (RFC 1950/1951).
//   JPEG - baseline and extended-sequential Huffman (the format of almost
//          every PBR texture download); greyscale and YCbCr with any
//          chroma subsampling. Progressive JPEGs are reported as unsupported.
//   TGA  - uncompressed and RLE, 8/24/32-bit.
//   BMP  - uncompressed 24/32-bit.
// Results are 8-bit RGBA, top row first.
#include <cstdint>
#include <string>
#include <vector>

struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
    bool empty() const { return rgba.empty(); }
};

bool loadImage(const std::string& path, Image& out, std::string& err);
bool decodeImage(const uint8_t* data, size_t size, Image& out, std::string& err);

// Procedural texture sets usable as "builtin:<set>_<map>" texture paths:
// sets bricks, tiles, metal; maps color, normal, roughness, ao (512 x 512).
bool generateBuiltinTexture(const std::string& name, Image& out);

// zlib stream -> bytes. Exposed for tests.
bool zlibInflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out);

// Textures loaded once per path and shared by the viewport and the
// renderers. Failed loads are remembered (with their error) so a missing
// file is not retried every frame. Thread-safe.
#include <memory>
#include <mutex>
#include <unordered_map>

class TextureCache {
public:
    std::shared_ptr<const Image> get(const std::string& path);
    std::string error(const std::string& path) const;
    // Loads all `paths` not loaded yet, in parallel on the job system.
    void preload(const std::vector<std::string>& paths);
    void clear();
    size_t bytes() const;
    uint64_t generation() const { return generation_; }  // bumps when anything is (re)loaded

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<const Image>> images_;
    std::unordered_map<std::string, std::string> errors_;
    uint64_t generation_ = 1;
};
TextureCache& textureCache();
