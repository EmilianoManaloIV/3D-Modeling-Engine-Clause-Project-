#include "image_io.h"

#include <cstdio>

namespace {
uint32_t crc32(const uint8_t* data, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void put32be(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 3; i >= 0; --i) v.push_back((uint8_t)(x >> (8 * i)));
}

void writeChunk(FILE* f, const char* type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> buf;
    put32be(buf, (uint32_t)data.size());
    buf.insert(buf.end(), type, type + 4);
    buf.insert(buf.end(), data.begin(), data.end());
    put32be(buf, crc32(buf.data() + 4, buf.size() - 4));
    std::fwrite(buf.data(), 1, buf.size(), f);
}
}  // namespace

bool writePNG(const std::string& path, int w, int h, const std::vector<uint8_t>& rgb) {
    if (w <= 0 || h <= 0 || rgb.size() < size_t(w) * h * 3) return false;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    std::fwrite(sig, 1, 8, f);

    std::vector<uint8_t> ihdr;
    put32be(ihdr, (uint32_t)w);
    put32be(ihdr, (uint32_t)h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit RGB, no interlace
    writeChunk(f, "IHDR", ihdr);

    // Raw scanlines (filter byte 0), top row first.
    std::vector<uint8_t> raw;
    raw.reserve(size_t(w * 3 + 1) * h);
    for (int y = h - 1; y >= 0; --y) {
        raw.push_back(0);
        const uint8_t* row = &rgb[size_t(y) * w * 3];
        raw.insert(raw.end(), row, row + size_t(w) * 3);
    }

    // zlib stream made of "stored" deflate blocks.
    std::vector<uint8_t> z = {0x78, 0x01};
    size_t pos = 0;
    do {
        size_t n = raw.size() - pos;
        if (n > 65535) n = 65535;
        bool final = pos + n == raw.size();
        z.push_back(final ? 1 : 0);
        z.push_back((uint8_t)(n & 0xFF));
        z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)(~n & 0xFF));
        z.push_back((uint8_t)((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + (long)pos, raw.begin() + (long)(pos + n));
        pos += n;
    } while (pos < raw.size());
    uint32_t a = 1, b = 0;  // Adler-32
    for (uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    put32be(z, (b << 16) | a);
    writeChunk(f, "IDAT", z);
    writeChunk(f, "IEND", {});
    bool ok = !std::ferror(f);
    std::fclose(f);
    return ok;
}
