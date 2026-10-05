#include "image_io.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// PNG writer with real DEFLATE compression (no zlib dependency):
//   - each scanline picks the PNG filter (None / Sub / Up / Paeth) with the
//     smallest sum of absolute residuals (the usual heuristic);
//   - the filtered bytes are compressed with LZ77 (32 KiB window, hash chains,
//     lazy-free greedy matching) and encoded with the fixed Huffman codes of
//     RFC 1951 sec. 3.2.6 in a single block.
// UI screenshots (large flat areas) shrink roughly 20-50x versus storing.
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

// --- DEFLATE (fixed Huffman) ---------------------------------------------------
struct BitWriter {
    std::vector<uint8_t>& out;
    uint32_t acc = 0;
    int count = 0;
    explicit BitWriter(std::vector<uint8_t>& o) : out(o) {}
    void bits(uint32_t value, int n) {  // LSB first
        acc |= value << count;
        count += n;
        while (count >= 8) {
            out.push_back((uint8_t)acc);
            acc >>= 8;
            count -= 8;
        }
    }
    void huff(uint32_t code, int n) {  // Huffman codes go MSB first
        uint32_t r = 0;
        for (int i = 0; i < n; ++i) r |= ((code >> i) & 1u) << (n - 1 - i);
        bits(r, n);
    }
    void flush() {
        if (count > 0) out.push_back((uint8_t)acc);
        acc = 0;
        count = 0;
    }
};

void literal(BitWriter& w, int sym) {  // fixed literal/length code
    if (sym <= 143) w.huff(0x30 + sym, 8);
    else if (sym <= 255) w.huff(0x190 + (sym - 144), 9);
    else if (sym <= 279) w.huff(sym - 256, 7);
    else w.huff(0xC0 + (sym - 280), 8);
}

const int kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                          35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const int kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const int kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                           193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const int kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void match(BitWriter& w, int len, int dist) {
    int li = 28;
    while (kLenBase[li] > len) --li;
    literal(w, 257 + li);
    if (kLenExtra[li]) w.bits(len - kLenBase[li], kLenExtra[li]);
    int di = 29;
    while (kDistBase[di] > dist) --di;
    w.huff(di, 5);
    if (kDistExtra[di]) w.bits(dist - kDistBase[di], kDistExtra[di]);
}

void deflateFixed(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    constexpr int kWindow = 32768, kHashBits = 15, kMaxChain = 48, kMinMatch = 3, kMaxMatch = 258;
    std::vector<int> head(1 << kHashBits, -1), prev(kWindow, -1);
    auto hash = [&](size_t i) {
        uint32_t h = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
        return (h * 2654435761u) >> (32 - kHashBits);
    };
    BitWriter w(out);
    w.bits(1, 1);  // BFINAL
    w.bits(1, 2);  // BTYPE = 01 (fixed Huffman)
    const size_t n = in.size();
    size_t i = 0;
    auto insert = [&](size_t pos) {
        if (pos + kMinMatch > n) return;
        uint32_t h = hash(pos);
        prev[pos % kWindow] = head[h];
        head[h] = (int)pos;
    };
    while (i < n) {
        int bestLen = 0, bestDist = 0;
        if (i + kMinMatch <= n) {
            int cand = head[hash(i)];
            const int maxLen = (int)std::min<size_t>(kMaxMatch, n - i);
            for (int chain = 0; cand >= 0 && chain < kMaxChain; ++chain) {
                size_t c = (size_t)cand;
                if (i - c > (size_t)kWindow - 1) break;
                if (in[c + bestLen] == in[i + bestLen]) {
                    int len = 0;
                    while (len < maxLen && in[c + len] == in[i + len]) ++len;
                    if (len > bestLen) {
                        bestLen = len;
                        bestDist = (int)(i - c);
                        if (len == maxLen) break;
                    }
                }
                int next = prev[c % kWindow];
                if (next >= cand) break;  // stale entry from a previous window cycle
                cand = next;
            }
        }
        if (bestLen >= kMinMatch) {
            match(w, bestLen, bestDist);
            for (int k = 0; k < bestLen; ++k) insert(i + k);
            i += bestLen;
        } else {
            literal(w, in[i]);
            insert(i);
            ++i;
        }
    }
    literal(w, 256);  // end of block
    w.flush();
}

int paeth(int a, int b, int c) {
    int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

}  // namespace

bool writePNG(const std::string& path, int w, int h, const std::vector<uint8_t>& rgb) {
    if (w <= 0 || h <= 0 || rgb.size() < size_t(w) * h * 3) return false;

    // Filtered scanlines, top row first (input rows are bottom-up).
    const size_t stride = size_t(w) * 3;
    std::vector<uint8_t> raw;
    raw.reserve((stride + 1) * h);
    std::vector<uint8_t> candidates[4];
    for (auto& c : candidates) c.resize(stride);
    const uint8_t* prevRow = nullptr;
    for (int y = h - 1; y >= 0; --y) {
        const uint8_t* row = &rgb[size_t(y) * stride];
        long best = -1;
        int bestFilter = 0;
        for (int filter = 0; filter < 4; ++filter) {
            long sum = 0;
            for (size_t x = 0; x < stride; ++x) {
                int a = x >= 3 ? row[x - 3] : 0, b = prevRow ? prevRow[x] : 0, c = (prevRow && x >= 3) ? prevRow[x - 3] : 0;
                int pred = filter == 0 ? 0 : filter == 1 ? a : filter == 2 ? b : paeth(a, b, c);
                uint8_t v = (uint8_t)(row[x] - pred);
                candidates[filter][x] = v;
                sum += v < 128 ? v : 256 - v;
            }
            if (best < 0 || sum < best) {
                best = sum;
                bestFilter = filter;
            }
        }
        raw.push_back((uint8_t)(bestFilter == 3 ? 4 : bestFilter));  // PNG filter ids: 0 none, 1 sub, 2 up, 4 paeth
        raw.insert(raw.end(), candidates[bestFilter].begin(), candidates[bestFilter].end());
        prevRow = row;
    }

    std::vector<uint8_t> z = {0x78, 0x9C};  // zlib header (deflate, 32K window)
    deflateFixed(raw, z);
    uint32_t a = 1, b = 0;  // Adler-32 of the uncompressed data
    for (uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    put32be(z, (b << 16) | a);

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    std::fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    put32be(ihdr, (uint32_t)w);
    put32be(ihdr, (uint32_t)h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit RGB, deflate, adaptive filtering, no interlace
    writeChunk(f, "IHDR", ihdr);
    writeChunk(f, "IDAT", z);
    writeChunk(f, "IEND", {});
    bool ok = !std::ferror(f);
    std::fclose(f);
    return ok;
}
