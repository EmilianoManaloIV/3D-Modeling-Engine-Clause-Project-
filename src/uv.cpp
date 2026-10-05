#include "uv.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>

namespace uv {
namespace {

std::vector<int> allIfEmpty(const Mesh& m, std::vector<int> faces) {
    if (faces.empty()) {
        faces.resize(m.faces.size());
        std::iota(faces.begin(), faces.end(), 0);
    }
    return faces;
}

// Projection bases for the 6 axis directions (+X,-X,+Y,-Y,+Z,-Z), chosen so
// that u x v equals the axis: charts are never mirrored.
void axisBasis(int cls, Vec3& u, Vec3& v) {
    switch (cls) {
        case 0: u = {0, 0, -1}; v = {0, 1, 0}; break;
        case 1: u = {0, 0, 1}; v = {0, 1, 0}; break;
        case 2: u = {1, 0, 0}; v = {0, 0, -1}; break;
        case 3: u = {1, 0, 0}; v = {0, 0, 1}; break;
        case 4: u = {1, 0, 0}; v = {0, 1, 0}; break;
        default: u = {-1, 0, 0}; v = {0, 1, 0}; break;
    }
}

int dominantClass(Vec3 n) {
    int ax = 0;
    if (std::fabs(n.y) > std::fabs(n[ax])) ax = 1;
    if (std::fabs(n.z) > std::fabs(n[ax])) ax = 2;
    return ax * 2 + (n[ax] < 0 ? 1 : 0);
}

// Basis for an arbitrary normal, again with u x v = n.
void normalBasis(Vec3 n, Vec3& u, Vec3& v) {
    n = normalize(n);
    Vec3 up = std::fabs(n.y) < 0.99f ? Vec3(0, 1, 0) : Vec3(0, 0, n.y > 0 ? -1.0f : 1.0f);
    u = normalize(cross(up, n));
    v = cross(n, u);
}

void projectFace(Mesh& m, int f, Vec3 u, Vec3 v) {
    const auto& face = m.faces[f];
    for (size_t i = 0; i < face.size(); ++i) m.uvs[f][i] = {dot(m.verts[face[i]], u), dot(m.verts[face[i]], v)};
}

struct UnionFind {
    std::vector<int> p;
    explicit UnionFind(int n) : p(n) { std::iota(p.begin(), p.end(), 0); }
    int find(int x) { return p[x] == x ? x : p[x] = find(p[x]); }
    void unite(int a, int b) { p[find(a)] = find(b); }
};

uint64_t edgeKey(int a, int b) {
    if (a > b) std::swap(a, b);
    return (uint64_t(uint32_t(a)) << 32) | uint32_t(b);
}

// Groups the listed faces into connected components; `connect(fa, fb, a, b)`
// decides whether two faces sharing edge (a,b) belong together.
template <class Connect>
std::vector<int> components(const Mesh& m, const std::vector<int>& faces, int* count, Connect connect) {
    UnionFind uf((int)faces.size());
    // (edge, face) pairs sorted by edge: faces sharing an edge become adjacent
    // runs. Much cheaper than a hash map of small vectors.
    std::vector<std::pair<uint64_t, int>> edgeFaces;
    for (int li = 0; li < (int)faces.size(); ++li) {
        const auto& face = m.faces[faces[li]];
        for (size_t i = 0; i < face.size(); ++i)
            edgeFaces.push_back({edgeKey(face[i], face[(i + 1) % face.size()]), li});
    }
    std::sort(edgeFaces.begin(), edgeFaces.end());
    for (size_t r = 0; r < edgeFaces.size();) {
        size_t e = r + 1;
        while (e < edgeFaces.size() && edgeFaces[e].first == edgeFaces[r].first) ++e;
        const int a = int(edgeFaces[r].first >> 32), b = int(edgeFaces[r].first & 0xFFFFFFFFu);
        for (size_t i = r; i + 1 < e; ++i)
            for (size_t j = i + 1; j < e; ++j)
                if (connect(faces[edgeFaces[i].second], faces[edgeFaces[j].second], a, b))
                    uf.unite(edgeFaces[i].second, edgeFaces[j].second);
        r = e;
    }
    std::vector<int> ids(faces.size(), -1), out(faces.size());
    int n = 0;
    for (int li = 0; li < (int)faces.size(); ++li) {
        int root = uf.find(li);
        if (ids[root] < 0) ids[root] = n++;
        out[li] = ids[root];
    }
    if (count) *count = n;
    return out;
}

int cornerOf(const std::vector<int>& face, int v) {
    for (size_t i = 0; i < face.size(); ++i)
        if (face[i] == v) return (int)i;
    return -1;
}

// Shelf packing of islands (bounding boxes) into the unit square, keeping
// their relative sizes so texel density stays uniform.
void packIslands(Mesh& m, const std::vector<int>& faces, const std::vector<int>& island, int count) {
    struct Isl {
        Vec2 lo{1e30f, 1e30f}, hi{-1e30f, -1e30f};
        Vec2 place;
    };
    std::vector<Isl> isl(count);
    for (size_t li = 0; li < faces.size(); ++li)
        for (const Vec2& t : m.uvs[faces[li]]) {
            Isl& I = isl[island[li]];
            I.lo = {std::min(I.lo.x, t.x), std::min(I.lo.y, t.y)};
            I.hi = {std::max(I.hi.x, t.x), std::max(I.hi.y, t.y)};
        }
    float area = 0, maxW = 0;
    for (const Isl& I : isl) {
        area += (I.hi.x - I.lo.x) * (I.hi.y - I.lo.y);
        maxW = std::max(maxW, I.hi.x - I.lo.x);
    }
    area = std::max(area, 1e-12f);
    const float margin = 0.02f * std::sqrt(area);
    std::vector<int> order(count);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return (isl[a].hi.y - isl[a].lo.y) > (isl[b].hi.y - isl[b].lo.y);
    });

    float bestSide = 1e30f, bestW = 0;
    for (float k = 0.9f; k <= 2.0f; k += 0.1f) {  // try a few shelf widths, keep the squarest
        float W = std::max(std::sqrt(area) * k, maxW + 2 * margin);
        float x = margin, y = margin, rowH = 0;
        for (int i : order) {
            float w = isl[i].hi.x - isl[i].lo.x, h = isl[i].hi.y - isl[i].lo.y;
            if (x + w + margin > W && x > margin) {
                x = margin;
                y += rowH + margin;
                rowH = 0;
            }
            x += w + margin;
            rowH = std::max(rowH, h);
        }
        float side = std::max(W, y + rowH + margin);
        if (side < bestSide) {
            bestSide = side;
            bestW = W;
        }
    }
    float x = margin, y = margin, rowH = 0;
    for (int i : order) {
        float w = isl[i].hi.x - isl[i].lo.x, h = isl[i].hi.y - isl[i].lo.y;
        if (x + w + margin > bestW && x > margin) {
            x = margin;
            y += rowH + margin;
            rowH = 0;
        }
        isl[i].place = {x, y};
        x += w + margin;
        rowH = std::max(rowH, h);
    }
    const float s = 1.0f / bestSide;
    for (size_t li = 0; li < faces.size(); ++li) {
        const Isl& I = isl[island[li]];
        for (Vec2& t : m.uvs[faces[li]]) t = (t - I.lo + I.place) * s;
    }
}

// Cylindrical/spherical "u" wraps around; fix faces that straddle the seam.
void fixSeam(std::vector<Vec2>& uvs, const std::vector<char>& pole) {
    float lo = 1e30f, hi = -1e30f;
    for (size_t i = 0; i < uvs.size(); ++i)
        if (!pole[i]) {
            lo = std::min(lo, uvs[i].x);
            hi = std::max(hi, uvs[i].x);
        }
    if (hi - lo > 0.5f)
        for (size_t i = 0; i < uvs.size(); ++i)
            if (!pole[i] && uvs[i].x < 0.5f) uvs[i].x += 1.0f;
    // Pole corners have no defined u: use the average of the others.
    float sum = 0;
    int n = 0;
    for (size_t i = 0; i < uvs.size(); ++i)
        if (!pole[i]) {
            sum += uvs[i].x;
            ++n;
        }
    for (size_t i = 0; i < uvs.size(); ++i)
        if (pole[i]) uvs[i].x = n ? sum / n : 0.5f;
}

}  // namespace

const char* methodName(Method m) {
    switch (m) {
        case Method::Smart: return "Smart";
        case Method::Box: return "Box";
        case Method::Planar: return "Planar";
        case Method::Cylindrical: return "Cylindrical";
        case Method::Spherical: return "Spherical";
        default: return "Per-face";
    }
}

void ensure(Mesh& m) {
    if (m.hasUVs()) return;
    m.uvs.assign(m.faces.size(), {});
    for (size_t f = 0; f < m.faces.size(); ++f) m.uvs[f].assign(m.faces[f].size(), Vec2());
}

void bounds(const Mesh& m, const std::vector<int>& faces, Vec2& lo, Vec2& hi) {
    lo = {1e30f, 1e30f};
    hi = {-1e30f, -1e30f};
    if (!m.hasUVs()) return;
    for (int f : allIfEmpty(m, faces))
        for (const Vec2& t : m.uvs[f]) {
            lo = {std::min(lo.x, t.x), std::min(lo.y, t.y)};
            hi = {std::max(hi.x, t.x), std::max(hi.y, t.y)};
        }
}

void unwrap(Mesh& m, Method method, std::vector<int> faces) {
    ensure(m);
    faces = allIfEmpty(m, faces);
    if (faces.empty()) return;

    switch (method) {
        case Method::Box: {
            for (int f : faces) {
                Vec3 u, v;
                axisBasis(dominantClass(faceNormalRaw(m, m.faces[f])), u, v);
                projectFace(m, f, u, v);
            }
            fit(m, faces);
            break;
        }
        case Method::Planar: {
            Vec3 n;
            for (int f : faces) n += faceNormalRaw(m, m.faces[f]);
            if (length(n) < 1e-8f) n = {0, 0, 1};
            Vec3 u, v;
            normalBasis(n, u, v);
            for (int f : faces) projectFace(m, f, u, v);
            fit(m, faces);
            break;
        }
        case Method::Cylindrical:
        case Method::Spherical: {
            Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
            for (int f : faces)
                for (int v : m.faces[f]) {
                    lo = vmin(lo, m.verts[v]);
                    hi = vmax(hi, m.verts[v]);
                }
            const Vec3 c = (lo + hi) * 0.5f;
            const float height = std::max(1e-6f, hi.y - lo.y);
            for (int f : faces) {
                const auto& face = m.faces[f];
                std::vector<char> pole(face.size(), 0);
                for (size_t i = 0; i < face.size(); ++i) {
                    Vec3 d = m.verts[face[i]] - c;
                    pole[i] = (d.x * d.x + d.z * d.z) < 1e-10f;
                    float u = std::atan2(d.x, d.z) / (2 * kPi) + 0.5f;
                    float v = method == Method::Cylindrical
                                  ? (m.verts[face[i]].y - lo.y) / height
                                  : 0.5f + std::asin(clampf(d.y / std::max(1e-6f, length(d)), -1, 1)) / kPi;
                    m.uvs[f][i] = {u, v};
                }
                fixSeam(m.uvs[f], pole);
            }
            break;
        }
        case Method::Smart: {
            std::vector<int> cls(m.faces.size(), 0);
            for (int f : faces) cls[f] = dominantClass(faceNormalRaw(m, m.faces[f]));
            int count = 0;
            std::vector<int> chart =
                components(m, faces, &count, [&](int fa, int fb, int, int) { return cls[fa] == cls[fb]; });
            for (int f : faces) {
                Vec3 u, v;
                axisBasis(cls[f], u, v);
                projectFace(m, f, u, v);
            }
            packIslands(m, faces, chart, count);
            break;
        }
        case Method::PerFace: {
            std::vector<int> island(faces.size());
            for (size_t li = 0; li < faces.size(); ++li) {
                Vec3 u, v;
                const auto& face = m.faces[faces[li]];
                Vec3 n = faceNormalRaw(m, face);
                // Align u with the face's first edge for a tidy layout.
                u = normalize(m.verts[face[1]] - m.verts[face[0]]);
                v = normalize(cross(normalize(n), u));
                if (length(n) < 1e-12f || length(u) < 0.5f) normalBasis({0, 0, 1}, u, v);
                projectFace(m, faces[li], u, v);
                island[li] = (int)li;
            }
            packIslands(m, faces, island, (int)faces.size());
            break;
        }
    }
    m.touch();
}

std::vector<int> islandIds(const Mesh& m, const std::vector<int>& faces, int* count) {
    if (!m.hasUVs()) {
        if (count) *count = 0;
        return std::vector<int>(faces.size(), 0);
    }
    // Two faces share an island when they share an edge *and* agree on the
    // UVs at both of its vertices (i.e. the edge is not a seam).
    return components(m, faces, count, [&](int fa, int fb, int a, int b) {
        const auto &A = m.faces[fa], &B = m.faces[fb];
        int ia = cornerOf(A, a), ib = cornerOf(A, b), ja = cornerOf(B, a), jb = cornerOf(B, b);
        if (ia < 0 || ib < 0 || ja < 0 || jb < 0) return false;
        return length(m.uvs[fa][ia] - m.uvs[fb][ja]) < 1e-5f && length(m.uvs[fa][ib] - m.uvs[fb][jb]) < 1e-5f;
    });
}

void pack(Mesh& m, std::vector<int> faces) {
    ensure(m);
    faces = allIfEmpty(m, faces);
    int count = 0;
    std::vector<int> ids = islandIds(m, faces, &count);
    if (count > 0) packIslands(m, faces, ids, count);
    m.touch();
}

void fit(Mesh& m, std::vector<int> faces) {
    ensure(m);
    faces = allIfEmpty(m, faces);
    Vec2 lo, hi;
    bounds(m, faces, lo, hi);
    float size = std::max(hi.x - lo.x, hi.y - lo.y);
    if (size <= 1e-12f) return;
    for (int f : faces)
        for (Vec2& t : m.uvs[f]) t = (t - lo) / size;
    m.touch();
}

void rotate90(Mesh& m, std::vector<int> faces) {
    ensure(m);
    faces = allIfEmpty(m, faces);
    Vec2 lo, hi;
    bounds(m, faces, lo, hi);
    Vec2 c = (lo + hi) * 0.5f;
    for (int f : faces)
        for (Vec2& t : m.uvs[f]) {
            Vec2 d = t - c;
            t = c + Vec2(-d.y, d.x);
        }
    m.touch();
}

void flip(Mesh& m, std::vector<int> faces, bool horizontal) {
    ensure(m);
    faces = allIfEmpty(m, faces);
    Vec2 lo, hi;
    bounds(m, faces, lo, hi);
    for (int f : faces)
        for (Vec2& t : m.uvs[f]) {
            if (horizontal) t.x = lo.x + hi.x - t.x;
            else t.y = lo.y + hi.y - t.y;
        }
    m.touch();
}

void translate(Mesh& m, const std::vector<int>& faces, Vec2 delta) {
    if (!m.hasUVs()) return;
    for (int f : allIfEmpty(m, faces))
        for (Vec2& t : m.uvs[f]) t += delta;
    m.touch();
}

}  // namespace uv
