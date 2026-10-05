"""Generates the image-decoder test files in tests/data (run with Python 3).

For each image it writes the encoded file and, next to it, `<name>.rgba`: a
16-byte header (width, height as little-endian uint32, then 8 reserved bytes)
followed by the expected 8-bit RGBA pixels, top row first. JPEG is lossy, so
for JPEGs the expectation is the source image and the test allows an error.

PNGs use Python's zlib (dynamic Huffman codes, level 9, and stored blocks at
level 0) with every filter type, all colour types, 1..16-bit depths, palettes
with transparency and Adam7 interlacing. The JPEGs come from the small
baseline encoder below (standard Annex K tables): 4:4:4, 4:2:0, greyscale and
restart intervals.
"""
import math
import os
import struct
import zlib

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")


def test_pattern(w, h, alpha=True):
    px = []
    for y in range(h):
        for x in range(w):
            r = (x * 255) // max(1, w - 1)
            g = (y * 255) // max(1, h - 1)
            b = ((x // 4 + y // 4) % 2) * 200 + 20
            a = 255 if not alpha else (64 + (x * 191) // max(1, w - 1))
            px.append((r, g, b, a))
    return px


def write_expect(name, w, h, px):
    with open(os.path.join(OUT, name + ".rgba"), "wb") as f:
        f.write(struct.pack("<II8x", w, h))
        for p in px:
            f.write(bytes(p))


# ---------------------------------------------------------------------------
# PNG
# ---------------------------------------------------------------------------
def chunk(tag, data):
    c = struct.pack(">I", len(data)) + tag + data
    return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def filter_rows(rows, bpp, ftype_for_row):
    out = bytearray()
    prev = bytearray(len(rows[0])) if rows else bytearray()
    for i, row in enumerate(rows):
        ft = ftype_for_row(i)
        out.append(ft)
        for x in range(len(row)):
            a = row[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if ft == 0:
                v = row[x]
            elif ft == 1:
                v = row[x] - a
            elif ft == 2:
                v = row[x] - b
            elif ft == 3:
                v = row[x] - ((a + b) >> 1)
            else:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                v = row[x] - pred
            out.append(v & 255)
        prev = row
    return out


def pack_row(samples, depth):
    if depth == 8:
        return bytearray(samples)
    if depth == 16:
        out = bytearray()
        for s in samples:
            out += struct.pack(">H", s)
        return out
    out = bytearray()
    acc = 0
    nbits = 0
    for s in samples:
        acc = (acc << depth) | s
        nbits += depth
        if nbits == 8:
            out.append(acc)
            acc = 0
            nbits = 0
    if nbits:
        out.append(acc << (8 - nbits))
    return out


def png(name, w, h, ctype, depth, sample_fn, expect_fn, palette=None, trns=None, interlace=False, level=9):
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    bpp = max(1, channels * depth // 8)

    def rows_for(xs, ys):
        rows = []
        for y in ys:
            samples = []
            for x in xs:
                samples += sample_fn(x, y)
            rows.append(pack_row(samples, depth))
        return rows

    raw = bytearray()
    if not interlace:
        raw = filter_rows(rows_for(range(w), range(h)), bpp, lambda i: i % 5)
    else:
        passes = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]
        for sx, sy, dx, dy in passes:
            xs = list(range(sx, w, dx))
            ys = list(range(sy, h, dy))
            if xs and ys:
                raw += filter_rows(rows_for(xs, ys), bpp, lambda i: (i + sx) % 5)
    data = b"\x89PNG\r\n\x1a\n"
    data += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, 1 if interlace else 0))
    if palette:
        data += chunk(b"PLTE", bytes(sum((list(c) for c in palette), [])))
    if trns:
        data += chunk(b"tRNS", bytes(trns))
    z = zlib.compress(bytes(raw), level)
    # Split IDAT in two to test concatenation.
    data += chunk(b"IDAT", z[: len(z) // 2]) + chunk(b"IDAT", z[len(z) // 2 :]) + chunk(b"IEND", b"")
    with open(os.path.join(OUT, name + ".png"), "wb") as f:
        f.write(data)
    write_expect(name, w, h, [expect_fn(x, y) for y in range(h) for x in range(w)])


# ---------------------------------------------------------------------------
# Baseline JPEG encoder (ITU T.81, Annex K tables)
# ---------------------------------------------------------------------------
ZIGZAG = [0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7,
          14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46,
          53, 60, 61, 54, 47, 55, 62, 63]
QY = [16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29,
      51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121,
      120, 101, 72, 92, 95, 98, 112, 100, 103, 99]
QC = [17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99,
      99, 99, 99, 99] + [99] * 32
DC_BITS = [0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0]
DC_VALS = list(range(12))
DC_BITS_C = [0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0]
AC_BITS = [0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7D]
AC_VALS = [0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22,
           0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24, 0x33, 0x62,
           0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x34, 0x35, 0x36,
           0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
           0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A,
           0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2,
           0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2,
           0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1,
           0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9,
           0xFA]


def huff_codes(bits, vals):
    codes = {}
    code = 0
    k = 0
    for length in range(1, 17):
        for _ in range(bits[length - 1]):
            codes[vals[k]] = (code, length)
            code += 1
            k += 1
        code <<= 1
    return codes


class BitWriter:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, code, length):
        for i in range(length - 1, -1, -1):
            self.acc = (self.acc << 1) | ((code >> i) & 1)
            self.n += 1
            if self.n == 8:
                self.out.append(self.acc)
                if self.acc == 0xFF:
                    self.out.append(0)
                self.acc = 0
                self.n = 0

    def flush(self):
        if self.n:
            self.put((1 << (8 - self.n)) - 1, 8 - self.n)


def fdct(block):
    out = [0.0] * 64
    for v in range(8):
        for u in range(8):
            s = 0.0
            for y in range(8):
                for x in range(8):
                    s += block[y * 8 + x] * math.cos((2 * x + 1) * u * math.pi / 16) * math.cos((2 * y + 1) * v * math.pi / 16)
            cu = math.sqrt(0.5) if u == 0 else 1.0
            cv = math.sqrt(0.5) if v == 0 else 1.0
            out[v * 8 + u] = 0.25 * cu * cv * s
    return out


def category(v):
    v = abs(v)
    n = 0
    while v:
        n += 1
        v >>= 1
    return n


def jpeg(name, w, h, rgb_fn, mode, restart=0):
    """mode: 'gray', '444' or '420'."""
    comps = [("Y", 1, 1, 0)] if mode == "gray" else (
        [("Y", 2, 2, 0), ("Cb", 1, 1, 1), ("Cr", 1, 1, 1)] if mode == "420" else
        [("Y", 1, 1, 0), ("Cb", 1, 1, 1), ("Cr", 1, 1, 1)])
    hmax = max(c[1] for c in comps)
    vmax = max(c[2] for c in comps)

    def ycc(x, y):
        x = min(x, w - 1)
        y = min(y, h - 1)
        r, g, b = rgb_fn(x, y)
        Y = 0.299 * r + 0.587 * g + 0.114 * b
        cb = -0.168736 * r - 0.331264 * g + 0.5 * b + 128
        cr = 0.5 * r - 0.418688 * g - 0.081312 * b + 128
        return (Y, cb, cr)

    dc = [huff_codes(DC_BITS, DC_VALS), huff_codes(DC_BITS_C, DC_VALS)]
    ac = [huff_codes(AC_BITS, AC_VALS), huff_codes(AC_BITS, AC_VALS)]
    q = [QY, QC]
    bw = BitWriter()
    pred = [0, 0, 0]
    mcu_w, mcu_h = 8 * hmax, 8 * vmax
    mcus_x = (w + mcu_w - 1) // mcu_w
    mcus_y = (h + mcu_h - 1) // mcu_h
    count = 0
    rst = 0
    for my in range(mcus_y):
        for mx in range(mcus_x):
            if restart and count and count % restart == 0:
                bw.flush()
                bw.out += bytes([0xFF, 0xD0 + rst])
                rst = (rst + 1) % 8
                pred = [0, 0, 0]
            for ci, (cname, hs, vs, tbl) in enumerate(comps):
                for by in range(vs):
                    for bx in range(hs):
                        block = []
                        for yy in range(8):
                            for xx in range(8):
                                # Sample position of this component's pixel (box-averaged for subsampled chroma).
                                fx = hmax // hs
                                fy = vmax // vs
                                px0 = (mx * hs + bx) * 8 * fx + xx * fx
                                py0 = (my * vs + by) * 8 * fy + yy * fy
                                s = 0.0
                                for oy in range(fy):
                                    for ox in range(fx):
                                        s += ycc(px0 + ox, py0 + oy)[ci]
                                block.append(s / (fx * fy) - 128.0)
                        coef = fdct(block)
                        qc = [int(round(coef[ZIGZAG[k]] / q[tbl][ZIGZAG[k]])) for k in range(64)]
                        diff = qc[0] - pred[ci]
                        pred[ci] = qc[0]
                        n = category(diff)
                        bw.put(*dc[tbl][n])
                        if n:
                            bw.put(diff if diff > 0 else diff + (1 << n) - 1, n)
                        run = 0
                        for k in range(1, 64):
                            v = qc[k]
                            if v == 0:
                                run += 1
                                continue
                            while run > 15:
                                bw.put(*ac[tbl][0xF0])
                                run -= 16
                            n = category(v)
                            bw.put(*ac[tbl][(run << 4) | n])
                            bw.put(v if v > 0 else v + (1 << n) - 1, n)
                            run = 0
                        if run:
                            bw.put(*ac[tbl][0x00])
            count += 1
    bw.flush()

    def seg(marker, payload):
        return bytes([0xFF, marker]) + struct.pack(">H", len(payload) + 2) + payload

    data = b"\xFF\xD8" + seg(0xE0, b"JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00")
    data += seg(0xDB, bytes([0]) + bytes(QY[ZIGZAG[k]] for k in range(64)))
    if mode != "gray":
        data += seg(0xDB, bytes([1]) + bytes(QC[ZIGZAG[k]] for k in range(64)))
    sof = struct.pack(">BHHB", 8, h, w, len(comps))
    for i, (cname, hs, vs, tbl) in enumerate(comps):
        sof += bytes([i + 1, (hs << 4) | vs, tbl])
    data += seg(0xC0, sof)
    data += seg(0xC4, bytes([0x00]) + bytes(DC_BITS) + bytes(DC_VALS))
    data += seg(0xC4, bytes([0x10]) + bytes(AC_BITS) + bytes(AC_VALS))
    if mode != "gray":
        data += seg(0xC4, bytes([0x01]) + bytes(DC_BITS_C) + bytes(DC_VALS))
        data += seg(0xC4, bytes([0x11]) + bytes(AC_BITS) + bytes(AC_VALS))
    if restart:
        data += seg(0xDD, struct.pack(">H", restart))
    sos = bytes([len(comps)])
    for i, (cname, hs, vs, tbl) in enumerate(comps):
        sos += bytes([i + 1, (tbl << 4) | tbl])
    sos += bytes([0, 63, 0])
    data += seg(0xDA, sos) + bytes(bw.out) + b"\xFF\xD9"
    with open(os.path.join(OUT, name + ".jpg"), "wb") as f:
        f.write(data)
    exp = []
    for y in range(h):
        for x in range(w):
            r, g, b = rgb_fn(x, y)
            if mode == "gray":
                Y = int(round(0.299 * r + 0.587 * g + 0.114 * b))
                exp.append((Y, Y, Y, 255))
            else:
                exp.append((r, g, b, 255))
    write_expect(name, w, h, exp)


def smooth_rgb(x, y):
    return (int(127 + 100 * math.sin(x * 0.2)), int(127 + 100 * math.cos(y * 0.15)), (x * 2 + y * 3) % 200 + 20)


def main():
    os.makedirs(OUT, exist_ok=True)
    W, H = 37, 29  # odd sizes exercise partial blocks / rows
    pat = test_pattern(W, H)

    def rgba(x, y):
        return list(pat[y * W + x])

    png("rgba8", W, H, 6, 8, rgba, lambda x, y: tuple(pat[y * W + x]))
    png("rgba8_stored", W, H, 6, 8, rgba, lambda x, y: tuple(pat[y * W + x]), level=0)
    png("rgb8_interlaced", W, H, 2, 8, lambda x, y: rgba(x, y)[:3], lambda x, y: tuple(pat[y * W + x][:3]) + (255,),
        interlace=True)
    png("rgba16", W, H, 6, 16, lambda x, y: [v * 257 for v in rgba(x, y)], lambda x, y: tuple(pat[y * W + x]))
    png("gray8", W, H, 0, 8, lambda x, y: [pat[y * W + x][0]], lambda x, y: (pat[y * W + x][0],) * 3 + (255,))
    png("gray_alpha8", W, H, 4, 8, lambda x, y: [pat[y * W + x][1], pat[y * W + x][3]],
        lambda x, y: (pat[y * W + x][1],) * 3 + (pat[y * W + x][3],))
    for depth in (1, 2, 4):
        mx = (1 << depth) - 1
        png("gray%d" % depth, W, H, 0, depth, lambda x, y, mx=mx: [(x + y) % (mx + 1)],
            lambda x, y, mx=mx: ((x + y) % (mx + 1) * 255 // mx,) * 3 + (255,))
    pal = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (250, 200, 10)]
    png("palette2", W, H, 3, 2, lambda x, y: [(x // 3 + y) % 4], lambda x, y: pal[(x // 3 + y) % 4] + ((128,) if (x // 3 + y) % 4 == 1 else (255,)),
        palette=pal, trns=[255, 128])
    jpeg("jpeg444", 40, 24, smooth_rgb, "444")
    jpeg("jpeg420", 37, 29, smooth_rgb, "420", restart=2)
    jpeg("jpeg_gray", 21, 19, smooth_rgb, "gray", restart=3)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
