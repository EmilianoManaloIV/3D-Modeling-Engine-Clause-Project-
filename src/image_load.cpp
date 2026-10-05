#include "image_load.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// ============================================================================
// Inflate (RFC 1951)
// ============================================================================
struct BitReader {
    const uint8_t* p;
    const uint8_t* end;
    uint32_t bits = 0;
    int count = 0;
    bool overrun = false;
    uint32_t get(int n) {
        while (count < n) {
            uint32_t byte = 0;
            if (p < end) byte = *p++;
            else overrun = true;
            bits |= byte << count;
            count += 8;
        }
        uint32_t v = bits & ((n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1));
        bits >>= n;
        count -= n;
        return v;
    }
    void alignByte() {
        bits >>= count & 7;
        count -= count & 7;
    }
};

// Canonical Huffman decoding table: counts per length + symbols sorted.
struct Huffman {
    uint16_t count[16] = {};
    uint16_t symbol[320] = {};
    bool build(const uint8_t* lengths, int n) {
        std::memset(count, 0, sizeof count);
        for (int i = 0; i < n; ++i) count[lengths[i]]++;
        count[0] = 0;
        uint16_t offs[16];
        offs[1] = 0;
        for (int i = 1; i < 15; ++i) offs[i + 1] = offs[i] + count[i];
        for (int i = 0; i < n; ++i)
            if (lengths[i]) symbol[offs[lengths[i]]++] = (uint16_t)i;
        return true;
    }
    int decode(BitReader& br) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= (int)br.get(1);
            int c = count[len];
            if (code - c < first) return symbol[index + (code - first)];
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool inflateBlock(BitReader& br, std::vector<uint8_t>& out, const Huffman& lit, const Huffman& dist) {
    for (;;) {
        int sym = lit.decode(br);
        if (sym < 0 || br.overrun) return false;
        if (sym < 256) {
            out.push_back((uint8_t)sym);
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            int len = kLenBase[sym] + (int)br.get(kLenExtra[sym]);
            int ds = dist.decode(br);
            if (ds < 0 || ds >= 30) return false;
            size_t d = kDistBase[ds] + br.get(kDistExtra[ds]);
            if (d > out.size()) return false;
            size_t from = out.size() - d;
            for (int i = 0; i < len; ++i) out.push_back(out[from + i]);
        }
    }
}

bool inflateRaw(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    BitReader br{data, data + size};
    static Huffman fixedLit, fixedDist;
    static bool fixedReady = false;
    if (!fixedReady) {
        uint8_t l[288];
        for (int i = 0; i < 144; ++i) l[i] = 8;
        for (int i = 144; i < 256; ++i) l[i] = 9;
        for (int i = 256; i < 280; ++i) l[i] = 7;
        for (int i = 280; i < 288; ++i) l[i] = 8;
        fixedLit.build(l, 288);
        uint8_t d[30];
        for (int i = 0; i < 30; ++i) d[i] = 5;
        fixedDist.build(d, 30);
        fixedReady = true;
    }
    for (;;) {
        int final = (int)br.get(1);
        int type = (int)br.get(2);
        if (type == 0) {  // stored
            br.alignByte();
            // Drain the bit buffer, which holds whole bytes now.
            uint32_t len = br.get(16), nlen = br.get(16);
            if ((len ^ 0xFFFF) != nlen) return false;
            for (uint32_t i = 0; i < len; ++i) out.push_back((uint8_t)br.get(8));
            if (br.overrun) return false;
        } else if (type == 1) {
            if (!inflateBlock(br, out, fixedLit, fixedDist)) return false;
        } else if (type == 2) {
            int hlit = (int)br.get(5) + 257, hdist = (int)br.get(5) + 1, hclen = (int)br.get(4) + 4;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t cl[19] = {};
            for (int i = 0; i < hclen; ++i) cl[order[i]] = (uint8_t)br.get(3);
            Huffman clh;
            clh.build(cl, 19);
            uint8_t lens[320] = {};
            int n = 0;
            while (n < hlit + hdist) {
                int sym = clh.decode(br);
                if (sym < 0 || br.overrun) return false;
                if (sym < 16) {
                    lens[n++] = (uint8_t)sym;
                } else {
                    int rep = 0;
                    uint8_t val = 0;
                    if (sym == 16) {
                        if (n == 0) return false;
                        val = lens[n - 1];
                        rep = 3 + (int)br.get(2);
                    } else if (sym == 17) {
                        rep = 3 + (int)br.get(3);
                    } else {
                        rep = 11 + (int)br.get(7);
                    }
                    if (n + rep > hlit + hdist) return false;
                    while (rep--) lens[n++] = val;
                }
            }
            Huffman lit, dist;
            lit.build(lens, hlit);
            dist.build(lens + hlit, hdist);
            if (!inflateBlock(br, out, lit, dist)) return false;
        } else {
            return false;
        }
        if (final) return true;
    }
}

// ============================================================================
// PNG
// ============================================================================
uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

int paeth(int a, int b, int c) {
    int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

bool unfilter(uint8_t* data, size_t avail, int rowBytes, int rows, int bpp, std::vector<uint8_t>& out, size_t& used) {
    out.assign((size_t)rowBytes * rows, 0);
    std::vector<uint8_t> prev(rowBytes, 0);
    size_t pos = 0;
    for (int y = 0; y < rows; ++y) {
        if (pos + 1 + rowBytes > avail) return false;
        int ft = data[pos++];
        uint8_t* row = &out[(size_t)y * rowBytes];
        std::memcpy(row, data + pos, rowBytes);
        pos += rowBytes;
        for (int x = 0; x < rowBytes; ++x) {
            int a = x >= bpp ? row[x - bpp] : 0, b = prev[x], c = x >= bpp ? prev[x - bpp] : 0;
            switch (ft) {
                case 0: break;
                case 1: row[x] = uint8_t(row[x] + a); break;
                case 2: row[x] = uint8_t(row[x] + b); break;
                case 3: row[x] = uint8_t(row[x] + ((a + b) >> 1)); break;
                case 4: row[x] = uint8_t(row[x] + paeth(a, b, c)); break;
                default: return false;
            }
        }
        std::memcpy(prev.data(), row, rowBytes);
    }
    used = pos;
    return true;
}

bool decodePNG(const uint8_t* d, size_t size, Image& img, std::string& err) {
    size_t p = 8;
    int w = 0, h = 0, depth = 0, ctype = 0, interlace = 0;
    std::vector<uint8_t> idat, palette, trns;
    while (p + 8 <= size) {
        uint32_t len = be32(d + p);
        const uint8_t* type = d + p + 4;
        const uint8_t* body = d + p + 8;
        if (p + 12 + len > size) break;
        if (!std::memcmp(type, "IHDR", 4) && len >= 13) {
            w = (int)be32(body);
            h = (int)be32(body + 4);
            depth = body[8];
            ctype = body[9];
            interlace = body[12];
        } else if (!std::memcmp(type, "PLTE", 4)) {
            palette.assign(body, body + len);
        } else if (!std::memcmp(type, "tRNS", 4)) {
            trns.assign(body, body + len);
        } else if (!std::memcmp(type, "IDAT", 4)) {
            idat.insert(idat.end(), body, body + len);
        } else if (!std::memcmp(type, "IEND", 4)) {
            break;
        }
        p += 12 + len;
    }
    if (w <= 0 || h <= 0 || w > 32768 || h > 32768) return err = "bad PNG header", false;
    std::vector<uint8_t> raw;
    if (!zlibInflate(idat.data(), idat.size(), raw)) return err = "corrupt PNG data", false;
    int channels = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    if (!channels || (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16))
        return err = "unsupported PNG format", false;
    const int bitsPerPixel = channels * depth, bpp = std::max(1, bitsPerPixel / 8);
    img.width = w;
    img.height = h;
    img.rgba.assign((size_t)w * h * 4, 255);

    // Sample (channel c of pixel x) from an unfiltered row.
    auto sample = [&](const uint8_t* row, int x, int c) -> int {
        if (depth == 8) return row[x * channels + c];
        if (depth == 16) return row[(x * channels + c) * 2];  // high byte
        int bitPos = (x * channels + c) * depth;
        int v = (row[bitPos >> 3] >> (8 - depth - (bitPos & 7))) & ((1 << depth) - 1);
        return ctype == 3 ? v : v * 255 / ((1 << depth) - 1);
    };
    auto put = [&](const uint8_t* row, int x, int dx, int dy) {
        uint8_t* o = &img.rgba[((size_t)dy * w + dx) * 4];
        if (ctype == 3) {
            int i = sample(row, x, 0);
            o[0] = (size_t)i * 3 + 2 < palette.size() ? palette[i * 3] : 0;
            o[1] = (size_t)i * 3 + 2 < palette.size() ? palette[i * 3 + 1] : 0;
            o[2] = (size_t)i * 3 + 2 < palette.size() ? palette[i * 3 + 2] : 0;
            o[3] = (size_t)i < trns.size() ? trns[i] : 255;
        } else if (channels <= 2) {
            o[0] = o[1] = o[2] = (uint8_t)sample(row, x, 0);
            o[3] = channels == 2 ? (uint8_t)sample(row, x, 1) : 255;
        } else {
            for (int c = 0; c < channels; ++c) o[c] = (uint8_t)sample(row, x, c);
        }
    };
    if (!interlace) {
        int rowBytes = (w * bitsPerPixel + 7) / 8;
        std::vector<uint8_t> px;
        size_t used = 0;
        if (!unfilter(raw.data(), raw.size(), rowBytes, h, bpp, px, used)) return err = "corrupt PNG rows", false;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) put(&px[(size_t)y * rowBytes], x, x, y);
        return true;
    }
    // Adam7: seven reduced images.
    static const int sx[7] = {0, 4, 0, 2, 0, 1, 0}, sy[7] = {0, 0, 4, 0, 2, 0, 1};
    static const int dx[7] = {8, 8, 4, 4, 2, 2, 1}, dy[7] = {8, 8, 8, 4, 4, 2, 2};
    size_t pos = 0;
    for (int pass = 0; pass < 7; ++pass) {
        int pw = (w - sx[pass] + dx[pass] - 1) / dx[pass], ph = (h - sy[pass] + dy[pass] - 1) / dy[pass];
        if (pw <= 0 || ph <= 0) continue;
        int rowBytes = (pw * bitsPerPixel + 7) / 8;
        std::vector<uint8_t> px;
        size_t used = 0;
        if (!unfilter(raw.data() + pos, raw.size() - pos, rowBytes, ph, bpp, px, used))
            return err = "corrupt PNG rows", false;
        pos += used;
        for (int y = 0; y < ph; ++y)
            for (int x = 0; x < pw; ++x) put(&px[(size_t)y * rowBytes], x, sx[pass] + x * dx[pass], sy[pass] + y * dy[pass]);
    }
    return true;
}

// ============================================================================
// JPEG (baseline / extended sequential Huffman)
// ============================================================================
struct JpegHuff {
    uint8_t bits[17] = {};
    uint8_t vals[256] = {};
    // Fast decode via (code, length) search tables.
    int maxcode[18], valptr[17], mincode[17];
    // Lookahead: for the next 9 bits, (length << 8 | symbol), 0 = longer code.
    uint16_t fast[512];
    void build() {
        int code = 0, k = 0;
        for (int l = 1; l <= 16; ++l) {
            valptr[l] = k;
            mincode[l] = code;
            code += bits[l];
            k += bits[l];
            maxcode[l] = bits[l] ? code - 1 : -1;
            code <<= 1;
        }
        maxcode[17] = 0x7FFFFFFF;
        std::memset(fast, 0, sizeof fast);
        int c = 0, kk = 0;
        for (int l = 1; l <= 9; ++l) {
            for (int i = 0; i < bits[l]; ++i, ++c, ++kk)
                for (int f = 0; f < (1 << (9 - l)); ++f) fast[(c << (9 - l)) | f] = (uint16_t)((l << 8) | vals[kk]);
            c <<= 1;
        }
    }
};

struct JpegComp {
    int id, h, v, tq, td = 0, ta = 0;
    int bw = 0, bh = 0;  // blocks across / down (padded to the MCU)
    std::vector<uint8_t> pixels;
    int dcPred = 0;
};

struct JpegDecoder {
    const uint8_t* d;
    size_t size, pos = 0;
    uint16_t qt[4][64] = {};
    JpegHuff hdc[4], hac[4];
    std::vector<JpegComp> comps;
    int width = 0, height = 0, hmax = 1, vmax = 1, restart = 0;
    bool progressive = false;
    // Entropy-coded segment bit reader.
    uint32_t bitbuf = 0;
    int bitcnt = 0;
    bool hitMarker = false;

    int readBit() {
        if (bitcnt == 0) {
            int b = 0;
            if (!hitMarker && pos < size) {
                b = d[pos++];
                if (b == 0xFF) {
                    int n = pos < size ? d[pos] : 0;
                    if (n == 0) ++pos;  // stuffed zero
                    else {
                        hitMarker = true;  // a marker: feed zeros from now on
                        --pos;
                        b = 0;
                    }
                }
            }
            bitbuf = (uint32_t)b;
            bitcnt = 8;
        }
        --bitcnt;
        return (bitbuf >> bitcnt) & 1;
    }
    int receive(int n) {
        int v = 0;
        for (int i = 0; i < n; ++i) v = (v << 1) | readBit();
        return v;
    }
    static int extend(int v, int n) { return n == 0 ? 0 : (v < (1 << (n - 1)) ? v - (1 << n) + 1 : v); }
    // Bits not yet consumed, as a peekable window (refilled byte by byte).
    uint32_t peekBuf = 0;
    int peekCnt = 0;
    int peek9() {
        while (peekCnt < 9) {
            peekBuf = (peekBuf << 1) | (uint32_t)readBit();
            ++peekCnt;
        }
        return (int)((peekBuf >> (peekCnt - 9)) & 511);
    }
    int takeBit() {
        if (peekCnt > 0) {
            --peekCnt;
            return (int)((peekBuf >> peekCnt) & 1);
        }
        return readBit();
    }
    int receiveP(int n) {
        int v = 0;
        for (int i = 0; i < n; ++i) v = (v << 1) | takeBit();
        return v;
    }
    int decodeHuff(const JpegHuff& h) {
        int f = h.fast[peek9()];
        if (f) {
            peekCnt -= f >> 8;
            return f & 255;
        }
        int code = takeBit(), l = 1;
        while (l <= 16 && code > h.maxcode[l]) {
            code = (code << 1) | takeBit();
            ++l;
        }
        if (l > 16) return 0;
        return h.vals[h.valptr[l] + code - h.mincode[l]];
    }
    void resetBits() {
        bitcnt = 0;
        hitMarker = false;
        peekCnt = 0;
        peekBuf = 0;
    }
};

const uint8_t kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                             41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                             30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// Fixed-point separable 8x8 inverse DCT (the integer "jidctint" algorithm
// of the IJG library, in the compact form popularised by stb_image): ~10x
// fewer multiplies than evaluating the cosine sums directly. Columns first
// (skipping all-zero AC columns), then rows with level shift and clamping.
inline int f2f(float x) { return (int)(x * 4096 + 0.5f); }
#define MODELER_IDCT_1D(s0, s1, s2, s3, s4, s5, s6, s7)                                                     \
    int t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3;                                                \
    p2 = s2;                                                                                               \
    p3 = s6;                                                                                               \
    p1 = (p2 + p3) * f2f(0.5411961f);                                                                      \
    t2 = p1 + p3 * f2f(-1.847759065f);                                                                     \
    t3 = p1 + p2 * f2f(0.765366865f);                                                                      \
    p2 = s0;                                                                                               \
    p3 = s4;                                                                                               \
    t0 = (p2 + p3) * 4096;                                                                                 \
    t1 = (p2 - p3) * 4096;                                                                                 \
    x0 = t0 + t3;                                                                                          \
    x3 = t0 - t3;                                                                                          \
    x1 = t1 + t2;                                                                                          \
    x2 = t1 - t2;                                                                                          \
    t0 = s7;                                                                                               \
    t1 = s5;                                                                                               \
    t2 = s3;                                                                                               \
    t3 = s1;                                                                                               \
    p3 = t0 + t2;                                                                                          \
    p4 = t1 + t3;                                                                                          \
    p1 = t0 + t3;                                                                                          \
    p2 = t1 + t2;                                                                                          \
    p5 = (p3 + p4) * f2f(1.175875602f);                                                                    \
    t0 = t0 * f2f(0.298631336f);                                                                           \
    t1 = t1 * f2f(2.053119869f);                                                                           \
    t2 = t2 * f2f(3.072711026f);                                                                           \
    t3 = t3 * f2f(1.501321110f);                                                                           \
    p1 = p5 + p1 * f2f(-0.899976223f);                                                                     \
    p2 = p5 + p2 * f2f(-2.562915447f);                                                                     \
    p3 = p3 * f2f(-1.961570560f);                                                                          \
    p4 = p4 * f2f(-0.390180644f);                                                                          \
    t3 += p1 + p4;                                                                                         \
    t2 += p2 + p3;                                                                                         \
    t1 += p2 + p4;                                                                                         \
    t0 += p1 + p3;

inline uint8_t clampByte(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

// `in`: dequantised coefficients in natural (row-major) order.
void idct8x8(const int* in, uint8_t* out, int stride) {
    int v[64];
    for (int i = 0; i < 8; ++i) {
        const int* d = in + i;
        int* o = v + i;
        if (d[8] == 0 && d[16] == 0 && d[24] == 0 && d[32] == 0 && d[40] == 0 && d[48] == 0 && d[56] == 0) {
            int dc = d[0] * 4;
            o[0] = o[8] = o[16] = o[24] = o[32] = o[40] = o[48] = o[56] = dc;
            continue;
        }
        MODELER_IDCT_1D(d[0], d[8], d[16], d[24], d[32], d[40], d[48], d[56])
        x0 += 512;
        x1 += 512;
        x2 += 512;
        x3 += 512;
        o[0] = (x0 + t3) >> 10;
        o[56] = (x0 - t3) >> 10;
        o[8] = (x1 + t2) >> 10;
        o[48] = (x1 - t2) >> 10;
        o[16] = (x2 + t1) >> 10;
        o[40] = (x2 - t1) >> 10;
        o[24] = (x3 + t0) >> 10;
        o[32] = (x3 - t0) >> 10;
    }
    for (int i = 0; i < 8; ++i) {
        const int* r = v + i * 8;
        uint8_t* o = out + (size_t)i * stride;
        MODELER_IDCT_1D(r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7])
        // Rounding (65536) and the +128 level shift, both scaled by 2^17.
        x0 += 65536 + (128 << 17);
        x1 += 65536 + (128 << 17);
        x2 += 65536 + (128 << 17);
        x3 += 65536 + (128 << 17);
        o[0] = clampByte((x0 + t3) >> 17);
        o[7] = clampByte((x0 - t3) >> 17);
        o[1] = clampByte((x1 + t2) >> 17);
        o[6] = clampByte((x1 - t2) >> 17);
        o[2] = clampByte((x2 + t1) >> 17);
        o[5] = clampByte((x2 - t1) >> 17);
        o[3] = clampByte((x3 + t0) >> 17);
        o[4] = clampByte((x3 - t0) >> 17);
    }
}
#undef MODELER_IDCT_1D

bool decodeJPEG(const uint8_t* data, size_t size, Image& img, std::string& err) {
    JpegDecoder j;
    j.d = data;
    j.size = size;
    j.pos = 2;
    auto u16 = [&](size_t at) { return (int(j.d[at]) << 8) | j.d[at + 1]; };
    bool frameSeen = false;
    while (j.pos + 4 <= size) {
        if (j.d[j.pos] != 0xFF) {
            ++j.pos;
            continue;
        }
        int marker = j.d[j.pos + 1];
        j.pos += 2;
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01 || marker == 0xFF) continue;
        if (marker == 0xD9) break;
        int len = u16(j.pos);
        size_t seg = j.pos + 2, segEnd = j.pos + len;
        if (segEnd > size) return err = "truncated JPEG", false;
        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
            j.progressive = marker == 0xC2;
            if (j.progressive) return err = "progressive JPEG is not supported (re-save as baseline)", false;
            if (j.d[seg] != 8) return err = "only 8-bit JPEG is supported", false;
            j.height = u16(seg + 1);
            j.width = u16(seg + 3);
            int n = j.d[seg + 5];
            if (n != 1 && n != 3) return err = "unsupported JPEG colour format (CMYK?)", false;
            for (int i = 0; i < n; ++i) {
                JpegComp c;
                c.id = j.d[seg + 6 + i * 3];
                c.h = j.d[seg + 7 + i * 3] >> 4;
                c.v = j.d[seg + 7 + i * 3] & 15;
                c.tq = j.d[seg + 8 + i * 3] & 3;
                if (c.h < 1 || c.v < 1 || c.h > 4 || c.v > 4) return err = "bad JPEG sampling", false;
                j.hmax = std::max(j.hmax, c.h);
                j.vmax = std::max(j.vmax, c.v);
                j.comps.push_back(c);
            }
            frameSeen = true;
        } else if (marker >= 0xC3 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            return err = "unsupported JPEG type (lossless / arithmetic)", false;
        } else if (marker == 0xC4) {
            size_t q = seg;
            while (q < segEnd) {
                int tc = j.d[q] >> 4, th = j.d[q] & 3;
                JpegHuff& h = tc ? j.hac[th] : j.hdc[th];
                int total = 0;
                for (int l = 1; l <= 16; ++l) total += (h.bits[l] = j.d[q + l]);
                if (total > 256 || q + 17 + total > segEnd) return err = "bad JPEG Huffman table", false;
                std::memcpy(h.vals, j.d + q + 17, total);
                h.build();
                q += 17 + total;
            }
        } else if (marker == 0xDB) {
            size_t q = seg;
            while (q < segEnd) {
                int pq = j.d[q] >> 4, tq = j.d[q] & 3;
                for (int k = 0; k < 64; ++k) j.qt[tq][kZigzag[k]] = pq ? (uint16_t)u16(q + 1 + k * 2) : j.d[q + 1 + k];
                q += 1 + (pq ? 128 : 64);
            }
        } else if (marker == 0xDD) {
            j.restart = u16(seg);
        } else if (marker == 0xDA) {
            if (!frameSeen) return err = "JPEG scan before frame", false;
            int ns = j.d[seg];
            std::vector<JpegComp*> scomps;
            for (int i = 0; i < ns; ++i) {
                int cid = j.d[seg + 1 + i * 2], t = j.d[seg + 2 + i * 2];
                for (auto& c : j.comps)
                    if (c.id == cid) {
                        c.td = t >> 4;
                        c.ta = t & 15;
                        scomps.push_back(&c);
                    }
            }
            j.pos = segEnd;
            // Decode the (single, interleaved or not) scan.
            const int mcuW = 8 * j.hmax, mcuH = 8 * j.vmax;
            const int mcusX = (j.width + mcuW - 1) / mcuW, mcusY = (j.height + mcuH - 1) / mcuH;
            for (auto& c : j.comps) {
                c.bw = mcusX * c.h;
                c.bh = mcusY * c.v;
                if (c.pixels.empty()) c.pixels.assign((size_t)c.bw * 8 * c.bh * 8, 128);
                c.dcPred = 0;
            }
            j.resetBits();
            int block[64];
            auto decodeBlock = [&](JpegComp& c, int bx, int by) {
                int coef[64] = {};
                int acCount = 0;
                int t = j.decodeHuff(j.hdc[c.td]);
                int diff = t ? JpegDecoder::extend(j.receiveP(t), t) : 0;
                c.dcPred += diff;
                coef[0] = c.dcPred;
                for (int k = 1; k < 64;) {
                    int rs = j.decodeHuff(j.hac[c.ta]);
                    int r = rs >> 4, s = rs & 15;
                    if (s == 0) {
                        if (r != 15) break;
                        k += 16;
                        continue;
                    }
                    k += r;
                    if (k > 63) break;
                    coef[kZigzag[k]] = JpegDecoder::extend(j.receiveP(s), s);
                    ++k;
                    acCount = 1;
                }
                if (bx >= c.bw || by >= c.bh) return;
                uint8_t* dst = &c.pixels[((size_t)by * 8 * c.bw + bx) * 8];
                if (!acCount) {  // flat block: the IDCT of a DC term is a constant
                    int val = (int)std::lround(coef[0] * j.qt[c.tq][0] * 0.125f + 128.0f);
                    uint8_t v8 = (uint8_t)std::max(0, std::min(255, val));
                    for (int y = 0; y < 8; ++y) std::memset(dst + (size_t)y * c.bw * 8, v8, 8);
                    return;
                }
                for (int i = 0; i < 64; ++i) block[i] = coef[i] * j.qt[c.tq][i];
                idct8x8(block, dst, c.bw * 8);
            };
            int mcuCount = 0;
            auto handleRestart = [&]() {
                if (!j.restart || mcuCount == 0 || mcuCount % j.restart) return;
                // Skip to the RSTn marker and reset the predictors.
                j.resetBits();
                while (j.pos + 1 < size && !(j.d[j.pos] == 0xFF && j.d[j.pos + 1] >= 0xD0 && j.d[j.pos + 1] <= 0xD7))
                    ++j.pos;
                if (j.pos + 1 < size) j.pos += 2;
                for (auto& c : j.comps) c.dcPred = 0;
            };
            if (scomps.size() == 1) {
                // Non-interleaved: blocks cover only the component's real size.
                JpegComp& c = *scomps[0];
                int bx = (j.width * c.h / j.hmax + 7) / 8, by = (j.height * c.v / j.vmax + 7) / 8;
                for (int y = 0; y < by; ++y)
                    for (int x = 0; x < bx; ++x) {
                        handleRestart();
                        decodeBlock(c, x, y);
                        ++mcuCount;
                    }
            } else {
                for (int my = 0; my < mcusY; ++my)
                    for (int mx = 0; mx < mcusX; ++mx) {
                        handleRestart();
                        for (JpegComp* c : scomps)
                            for (int v = 0; v < c->v; ++v)
                                for (int h = 0; h < c->h; ++h) decodeBlock(*c, mx * c->h + h, my * c->v + v);
                        ++mcuCount;
                    }
            }
            // Continue after the entropy-coded data (next marker).
            while (j.pos + 1 < size && !(j.d[j.pos] == 0xFF && j.d[j.pos + 1] != 0 &&
                                         !(j.d[j.pos + 1] >= 0xD0 && j.d[j.pos + 1] <= 0xD7)))
                ++j.pos;
            continue;
        }
        j.pos = segEnd;
    }
    if (!frameSeen || j.comps.empty() || j.comps[0].pixels.empty()) return err = "no JPEG image data", false;
    img.width = j.width;
    img.height = j.height;
    img.rgba.assign((size_t)j.width * j.height * 4, 255);
    for (int y = 0; y < j.height; ++y)
        for (int x = 0; x < j.width; ++x) {
            float ch[3];
            for (size_t c = 0; c < j.comps.size(); ++c) {
                const JpegComp& k = j.comps[c];
                int sx = x * k.h / j.hmax, sy = y * k.v / j.vmax;  // nearest upsampling
                ch[c] = k.pixels[(size_t)sy * k.bw * 8 + sx];
            }
            uint8_t* o = &img.rgba[((size_t)y * j.width + x) * 4];
            if (j.comps.size() == 1) {
                o[0] = o[1] = o[2] = (uint8_t)ch[0];
            } else {  // YCbCr (JFIF) -> RGB
                float Y = ch[0], cb = ch[1] - 128.0f, cr = ch[2] - 128.0f;
                auto cl = [](float v) { return (uint8_t)std::max(0.0f, std::min(255.0f, v + 0.5f)); };
                o[0] = cl(Y + 1.402f * cr);
                o[1] = cl(Y - 0.344136f * cb - 0.714136f * cr);
                o[2] = cl(Y + 1.772f * cb);
            }
        }
    return true;
}

// ============================================================================
// TGA and BMP
// ============================================================================
bool decodeTGA(const uint8_t* d, size_t size, Image& img, std::string& err) {
    if (size < 18) return err = "truncated TGA", false;
    int idLen = d[0], cmapType = d[1], type = d[2];
    int w = d[12] | (d[13] << 8), h = d[14] | (d[15] << 8), bpp = d[16], desc = d[17];
    if (cmapType != 0 || !(type == 2 || type == 3 || type == 10 || type == 11) || !(bpp == 8 || bpp == 24 || bpp == 32))
        return err = "unsupported TGA format", false;
    size_t p = 18 + idLen;
    const int bytes = bpp / 8;
    img.width = w;
    img.height = h;
    img.rgba.assign((size_t)w * h * 4, 255);
    std::vector<uint8_t> px((size_t)w * h * bytes);
    if (type == 2 || type == 3) {
        if (p + px.size() > size) return err = "truncated TGA", false;
        std::memcpy(px.data(), d + p, px.size());
    } else {
        size_t o = 0;
        while (o < px.size() && p < size) {
            int hdr = d[p++], n = (hdr & 127) + 1;
            if (hdr & 128) {
                if (p + bytes > size) break;
                for (int i = 0; i < n && o < px.size(); ++i, o += bytes) std::memcpy(&px[o], d + p, bytes);
                p += bytes;
            } else {
                size_t len = std::min((size_t)n * bytes, px.size() - o);
                if (p + len > size) break;
                std::memcpy(&px[o], d + p, len);
                p += len;
                o += len;
            }
        }
    }
    const bool topDown = (desc & 0x20) != 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t* s = &px[((size_t)y * w + x) * bytes];
            uint8_t* o = &img.rgba[((size_t)(topDown ? y : h - 1 - y) * w + x) * 4];
            if (bytes == 1) o[0] = o[1] = o[2] = s[0];
            else {
                o[0] = s[2];
                o[1] = s[1];
                o[2] = s[0];
                if (bytes == 4) o[3] = s[3];
            }
        }
    return true;
}

bool decodeBMP(const uint8_t* d, size_t size, Image& img, std::string& err) {
    if (size < 54) return err = "truncated BMP", false;
    auto u32 = [&](size_t at) { return (uint32_t)d[at] | (uint32_t(d[at + 1]) << 8) | (uint32_t(d[at + 2]) << 16) | (uint32_t(d[at + 3]) << 24); };
    uint32_t offset = u32(10);
    int w = (int)u32(18), h = (int)u32(22);
    int bpp = d[28] | (d[29] << 8), compression = (int)u32(30);
    if ((bpp != 24 && bpp != 32) || (compression != 0 && compression != 3)) return err = "unsupported BMP format", false;
    const bool topDown = h < 0;
    h = std::abs(h);
    const int bytes = bpp / 8, stride = (w * bytes + 3) & ~3;
    if (offset + (size_t)stride * h > size) return err = "truncated BMP", false;
    img.width = w;
    img.height = h;
    img.rgba.assign((size_t)w * h * 4, 255);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t* s = d + offset + (size_t)y * stride + x * bytes;
            uint8_t* o = &img.rgba[((size_t)(topDown ? y : h - 1 - y) * w + x) * 4];
            o[0] = s[2];
            o[1] = s[1];
            o[2] = s[0];
        }
    return true;
}

}  // namespace

bool zlibInflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    out.clear();
    if (size < 2) return false;
    if ((data[0] & 15) != 8 || ((data[0] << 8) | data[1]) % 31 != 0) return false;  // deflate, valid check bits
    return inflateRaw(data + 2, size - 2, out);
}

bool decodeImage(const uint8_t* data, size_t size, Image& out, std::string& err) {
    out = Image();
    static const uint8_t pngSig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (size >= 8 && !std::memcmp(data, pngSig, 8)) return decodePNG(data, size, out, err);
    if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8) return decodeJPEG(data, size, out, err);
    if (size >= 2 && data[0] == 'B' && data[1] == 'M') return decodeBMP(data, size, out, err);
    if (size >= 18) return decodeTGA(data, size, out, err);  // TGA has no magic number
    err = "unknown image format";
    return false;
}

bool loadImage(const std::string& path, Image& out, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    std::vector<uint8_t> data;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) {
        data.resize((size_t)n);
        if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
    }
    std::fclose(f);
    if (data.empty()) {
        err = "cannot read " + path;
        return false;
    }
    if (!decodeImage(data.data(), data.size(), out, err)) {
        err = path + ": " + err;
        return false;
    }
    return true;
}

// ============================================================================
// Texture cache
// ============================================================================
#include "jobs.h"

std::shared_ptr<const Image> TextureCache::get(const std::string& path) {
    if (path.empty()) return nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = images_.find(path);
        if (it != images_.end()) return it->second;
        if (errors_.count(path)) return nullptr;
    }
    auto img = std::make_shared<Image>();
    std::string err;
    bool ok;
    if (path.compare(0, 8, "builtin:") == 0) {
        ok = generateBuiltinTexture(path.substr(8), *img);
        if (!ok) err = "unknown built-in texture " + path;
    } else {
        ok = loadImage(path, *img, err);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ok) {
        errors_[path] = err;
        return nullptr;
    }
    ++generation_;
    return images_[path] = img;
}

std::string TextureCache::error(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = errors_.find(path);
    return it == errors_.end() ? std::string() : it->second;
}

void TextureCache::preload(const std::vector<std::string>& paths) {
    std::vector<std::string> todo;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const std::string& p : paths)
            if (!p.empty() && !images_.count(p) && !errors_.count(p) &&
                std::find(todo.begin(), todo.end(), p) == todo.end())
                todo.push_back(p);
    }
    jobs::parallelFor(0, (int)todo.size(), 1, [&](int b, int e) {
        for (int i = b; i < e; ++i) get(todo[i]);
    });
}

void TextureCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    images_.clear();
    errors_.clear();
    ++generation_;
}

size_t TextureCache::bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = 0;
    for (const auto& kv : images_) n += kv.second ? kv.second->rgba.size() : 0;
    return n;
}

TextureCache& textureCache() {
    static TextureCache cache;
    return cache;
}

// ============================================================================
// Built-in procedural texture sets ("builtin:<set>_<map>"), so sample
// scenes work without image files next to the executable.
// ============================================================================
namespace {
float hash2(int x, int y, int seed) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xFFFFFF) / 16777215.0f;
}
float valueNoise(float x, float y, int seed) {
    int xi = (int)std::floor(x), yi = (int)std::floor(y);
    float fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    float a = hash2(xi, yi, seed), b = hash2(xi + 1, yi, seed), c = hash2(xi, yi + 1, seed), d = hash2(xi + 1, yi + 1, seed);
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}
float fbm(float x, float y, int seed) {
    float s = 0, amp = 0.5f;
    for (int o = 0; o < 5; ++o, x *= 2, y *= 2, amp *= 0.5f) s += amp * valueNoise(x, y, seed + o);
    return s;
}
// Height / colour / roughness of one texel of a set; u, v in [0, 1).
struct Sample {
    float height, rough, r, g, b;
};
Sample sampleSet(const std::string& set, float u, float v) {
    Sample s{};
    if (set == "bricks") {
        const float rows = 8, cols = 4, mortar = 0.06f;
        float y = v * rows;
        int row = (int)y;
        float x = u * cols + (row % 2) * 0.5f;
        int col = (int)std::floor(x);
        float fx = x - col, fy = y - row;
        float edge = std::min(std::min(fx, 1 - fx) * 2.0f, std::min(fy, 1 - fy));  // distance to the joint
        float brick = std::min(1.0f, std::max(0.0f, (edge - mortar * 0.5f) / (mortar * 0.6f)));
        float n = fbm(u * 24, v * 24, 7), tone = hash2(col, row, 3);
        s.height = brick * (0.85f + 0.15f * n);
        s.rough = brick > 0.5f ? 0.55f + 0.35f * n : 0.95f;
        if (brick > 0.5f) {
            s.r = 0.50f + 0.18f * tone + 0.12f * (n - 0.5f);
            s.g = 0.20f + 0.08f * tone + 0.06f * (n - 0.5f);
            s.b = 0.14f + 0.05f * tone;
        } else {
            s.r = s.g = s.b = 0.62f + 0.1f * n;
        }
    } else if (set == "tiles") {
        const float n = 6;
        float x = u * n, y = v * n;
        float fx = x - std::floor(x), fy = y - std::floor(y);
        float edge = std::min(std::min(fx, 1 - fx), std::min(fy, 1 - fy));
        float tile = std::min(1.0f, std::max(0.0f, (edge - 0.02f) / 0.03f));
        float noise = fbm(u * 40, v * 40, 11);
        bool dark = (((int)x + (int)y) & 1) != 0;
        s.height = tile;
        s.rough = tile > 0.5f ? 0.12f + 0.15f * noise : 0.9f;
        float c = tile > 0.5f ? (dark ? 0.12f : 0.85f) + 0.05f * noise : 0.45f;
        s.r = c;
        s.g = c;
        s.b = c * (dark ? 1.1f : 0.97f);
    } else {  // "metal": brushed metal
        float n = valueNoise(u * 400, v * 6, 5) * 0.6f + fbm(u * 8, v * 8, 9) * 0.4f;
        s.height = 0.5f + 0.02f * n;
        s.rough = 0.25f + 0.25f * n;
        s.r = s.g = s.b = 0.8f + 0.1f * n;
    }
    return s;
}
}  // namespace

bool generateBuiltinTexture(const std::string& name, Image& out) {
    // name = "<set>_<map>", map in color | normal | roughness | ao
    size_t us = name.rfind('_');
    if (us == std::string::npos) return false;
    const std::string set = name.substr(0, us), map = name.substr(us + 1);
    if (set != "bricks" && set != "tiles" && set != "metal") return false;
    if (map != "color" && map != "normal" && map != "roughness" && map != "ao") return false;
    const int N = 512;
    out.width = out.height = N;
    out.rgba.assign((size_t)N * N * 4, 255);
    std::vector<float> h((size_t)N * N);
    jobs::parallelFor(0, N, 16, [&](int b, int e) {
        for (int y = b; y < e; ++y)
            for (int x = 0; x < N; ++x) h[(size_t)y * N + x] = sampleSet(set, (x + 0.5f) / N, 1.0f - (y + 0.5f) / N).height;
    });
    jobs::parallelFor(0, N, 16, [&](int b, int e) {
        for (int y = b; y < e; ++y)
            for (int x = 0; x < N; ++x) {
                uint8_t* p = &out.rgba[((size_t)y * N + x) * 4];
                Sample s = sampleSet(set, (x + 0.5f) / N, 1.0f - (y + 0.5f) / N);
                auto q = [](float v) { return (uint8_t)std::max(0.0f, std::min(255.0f, v * 255.0f + 0.5f)); };
                if (map == "color") {  // sRGB
                    p[0] = q(s.r);
                    p[1] = q(s.g);
                    p[2] = q(s.b);
                } else if (map == "roughness") {
                    p[0] = p[1] = p[2] = q(s.rough);
                } else if (map == "ao") {
                    float avg = 0;  // crevices: lower than the surroundings
                    for (int k = -3; k <= 3; ++k)
                        avg += h[(size_t)((y + k + N) % N) * N + x] + h[(size_t)y * N + (x + k + N) % N];
                    avg /= 14.0f;
                    p[0] = p[1] = p[2] = q(std::min(1.0f, 0.6f + 0.4f * s.height + 0.6f * (h[(size_t)y * N + x] - avg)));
                } else {  // tangent-space normal (OpenGL convention: +Y = up in the image)
                    auto H = [&](int xx, int yy) { return h[(size_t)((yy + N) % N) * N + (xx + N) % N]; };
                    float dx = (H(x + 1, y) - H(x - 1, y)) * 0.5f * N / 64.0f;
                    float dy = (H(x, y - 1) - H(x, y + 1)) * 0.5f * N / 64.0f;
                    float len = std::sqrt(dx * dx + dy * dy + 1.0f);
                    p[0] = q((-dx / len) * 0.5f + 0.5f);
                    p[1] = q((-dy / len) * 0.5f + 0.5f);
                    p[2] = q((1.0f / len) * 0.5f + 0.5f);
                }
            }
    });
    return true;
}
