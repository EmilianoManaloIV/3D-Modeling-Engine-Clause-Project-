#include "polygon.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <unordered_map>

namespace poly {
namespace {

struct Projected {
    std::vector<Vec2> pts;
    float scale2 = 1.0f;  // squared size, for relative epsilons
};

// Drop the dominant axis of the normal; orient so the polygon is CCW in 2D.
Projected project(const std::vector<Vec3>& p, const std::vector<int>& face) {
    Vec3 n = faceNormalRaw(p, face);
    int ax = 0;
    if (std::fabs(n.y) > std::fabs(n[ax])) ax = 1;
    if (std::fabs(n.z) > std::fabs(n[ax])) ax = 2;
    const bool flip = n[ax] < 0;
    Projected out;
    out.pts.reserve(face.size());
    Vec2 lo(1e30f, 1e30f), hi(-1e30f, -1e30f);
    for (int v : face) {
        const Vec3& q = p[v];
        Vec2 s = ax == 0 ? Vec2(q.y, q.z) : ax == 1 ? Vec2(q.z, q.x) : Vec2(q.x, q.y);
        if (flip) s.x = -s.x;  // mirror so the winding becomes CCW
        out.pts.push_back(s);
        lo = {std::min(lo.x, s.x), std::min(lo.y, s.y)};
        hi = {std::max(hi.x, s.x), std::max(hi.y, s.y)};
    }
    float d = std::max(hi.x - lo.x, hi.y - lo.y);
    out.scale2 = std::max(d * d, 1e-30f);
    return out;
}

inline float orient(Vec2 a, Vec2 b, Vec2 c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }

bool inTriangle(Vec2 p, Vec2 a, Vec2 b, Vec2 c, float eps) {
    return orient(a, b, p) >= -eps && orient(b, c, p) >= -eps && orient(c, a, p) >= -eps;
}

// Points bucketed into a uniform grid, stored compactly (one sorted array
// plus a cell -> range table) instead of one small vector per cell, which
// made welding / T-junction repair allocation-bound on dense meshes.
struct PointGrid {
    float cell = 1.0f;
    std::vector<int> items;                                   // point indices sorted by cell
    std::unordered_map<uint64_t, std::pair<int, int>> range;  // cell -> [first, count)
    static uint64_t key(long long x, long long y, long long z) {
        return (uint64_t(x & 0x1FFFFF) << 42) | (uint64_t(y & 0x1FFFFF) << 21) | uint64_t(z & 0x1FFFFF);
    }
    void coords(Vec3 p, long long& x, long long& y, long long& z) const {
        x = (long long)std::floor(p.x / cell), y = (long long)std::floor(p.y / cell), z = (long long)std::floor(p.z / cell);
    }
    void build(const std::vector<Vec3>& pts, float cellSize) {
        cell = cellSize;
        std::vector<std::pair<uint64_t, int>> keyed(pts.size());
        for (size_t i = 0; i < pts.size(); ++i) {
            long long x, y, z;
            coords(pts[i], x, y, z);
            keyed[i] = {key(x, y, z), (int)i};
        }
        std::sort(keyed.begin(), keyed.end());
        items.resize(keyed.size());
        range.clear();
        range.reserve(keyed.size() / 2 + 16);
        for (size_t i = 0; i < keyed.size(); ++i) {
            items[i] = keyed[i].second;
            auto& r = range[keyed[i].first];
            if (r.second == 0) r.first = (int)i;
            r.second++;
        }
    }
    template <class F>
    void forCell(long long x, long long y, long long z, F&& f) const {
        auto it = range.find(key(x, y, z));
        if (it == range.end()) return;
        for (int k = it->second.first; k < it->second.first + it->second.second; ++k) f(items[k]);
    }
};

// Merges vertices within `eps` of each other. Unlike rounding to a grid, the
// 27-cell neighbourhood search also catches pairs straddling a cell border.
// Each vertex joins the first earlier vertex (that was itself kept) in range,
// exactly as an incremental weld would.
int weldNear(Mesh& m, float eps) {
    const size_t n = m.verts.size();
    PointGrid grid;
    grid.build(m.verts, eps * 4.0f);  // cells 4x eps: most points only need their own cell
    std::vector<int> rep(n, -1);
    for (size_t i = 0; i < n; ++i) {
        const Vec3 p = m.verts[i];
        long long cx, cy, cz;
        grid.coords(p, cx, cy, cz);
        // Neighbour cells only on the sides where p is within eps of the border.
        int lo[3], hi[3];
        const long long c[3] = {cx, cy, cz};
        for (int k = 0; k < 3; ++k) {
            float frac = p[k] - (float)c[k] * grid.cell;
            lo[k] = frac < eps ? -1 : 0;
            hi[k] = frac > grid.cell - eps ? 1 : 0;
        }
        int found = -1;
        for (int dx = lo[0]; dx <= hi[0]; ++dx)
            for (int dy = lo[1]; dy <= hi[1]; ++dy)
                for (int dz = lo[2]; dz <= hi[2]; ++dz)
                    grid.forCell(cx + dx, cy + dy, cz + dz, [&](int j) {
                        if (j < (int)i && rep[j] == j && (found < 0 || j < found) && length(m.verts[j] - p) <= eps) found = j;
                    });
        rep[i] = found >= 0 ? found : (int)i;
    }
    std::vector<int> remap(n);
    std::vector<Vec3> kept;
    std::vector<BoneWeights> keptW;
    const bool w = m.hasWeights();
    for (size_t i = 0; i < n; ++i) {
        if (rep[i] == (int)i) {
            remap[i] = (int)kept.size();
            kept.push_back(m.verts[i]);
            if (w) keptW.push_back(m.weights[i]);
        } else {
            remap[i] = remap[rep[i]];
        }
    }
    int merged = (int)(n - kept.size());
    m.verts.swap(kept);
    if (w) m.weights.swap(keptW);
    for (auto& f : m.faces)
        for (int& v : f) v = remap[v];
    return merged;
}

inline uint64_t edgeKey(int a, int b) {
    if (a > b) std::swap(a, b);
    return (uint64_t(uint32_t(a)) << 32) | uint32_t(b);
}

}  // namespace

bool isConvex(const std::vector<Vec3>& p, const std::vector<int>& face) {
    const size_t n = face.size();
    if (n <= 3) return true;
    Projected pr = project(p, face);
    const float eps = 1e-9f * pr.scale2;
    for (size_t i = 0; i < n; ++i)
        if (orient(pr.pts[i], pr.pts[(i + 1) % n], pr.pts[(i + 2) % n]) < -eps) return false;
    return true;
}

bool triangulate(const std::vector<Vec3>& p, const std::vector<int>& face, std::vector<int>& out) {
    const int n = (int)face.size();
    if (n < 3) return true;
    if (n == 3) {
        out.insert(out.end(), {0, 1, 2});
        return true;
    }
    Projected pr = project(p, face);
    const std::vector<Vec2>& q = pr.pts;
    const float eps = 1e-10f * pr.scale2;
    std::vector<int> idx(n);
    for (int i = 0; i < n; ++i) idx[i] = i;
    bool clean = true;
    int guard = 0;
    size_t i = 0;
    while (idx.size() > 3 && guard++ < 4 * n * n + 16) {
        const size_t m = idx.size();
        bool found = false;
        for (size_t step = 0; step < m; ++step) {
            size_t k = (i + step) % m;
            int a = idx[(k + m - 1) % m], b = idx[k], c = idx[(k + 1) % m];
            if (orient(q[a], q[b], q[c]) <= eps) continue;  // reflex or collinear: not an ear
            bool blocked = false;
            for (size_t j = 0; j < m && !blocked; ++j) {
                int v = idx[j];
                if (v == a || v == b || v == c) continue;
                // Vertices sitting exactly on a corner (duplicates) don't block.
                if (length(q[v] - q[a]) < 1e-7f || length(q[v] - q[b]) < 1e-7f || length(q[v] - q[c]) < 1e-7f)
                    continue;
                // Inclusive test: a reflex vertex touching the ear's diagonal blocks it.
                if (inTriangle(q[v], q[a], q[b], q[c], eps)) blocked = true;
            }
            if (blocked) continue;
            out.insert(out.end(), {a, b, c});
            idx.erase(idx.begin() + (long)k);
            i = (k + m - 2) % (m - 1);  // continue next to the clipped ear
            found = true;
            break;
        }
        if (!found) {
            // No proper ear (self-intersecting or fully degenerate polygon):
            // clip the corner with the largest area so we always finish.
            clean = false;
            size_t best = 0;
            float bestArea = -1e30f;
            for (size_t k = 0; k < m; ++k) {
                float ar = orient(q[idx[(k + m - 1) % m]], q[idx[k]], q[idx[(k + 1) % m]]);
                if (ar > bestArea) {
                    bestArea = ar;
                    best = k;
                }
            }
            out.insert(out.end(), {idx[(best + m - 1) % m], idx[best], idx[(best + 1) % m]});
            idx.erase(idx.begin() + (long)best);
            i = best % idx.size();
        }
    }
    if (idx.size() == 3) out.insert(out.end(), {idx[0], idx[1], idx[2]});
    return clean;
}

CleanupStats cleanup(Mesh& m, float eps) {
    CleanupStats st;
    if (m.verts.empty()) return st;
    Vec3 lo, hi;
    m.bounds(lo, hi);
    const float size = std::max(1e-6f, length(hi - lo));
    if (eps <= 0) eps = size * 1e-5f;
    const bool uvs = m.hasUVs();

    // 1. Weld.
    st.weldedVertices = weldNear(m, eps);

    // 2. Collapse repeated corners and drop faces with < 3 distinct vertices.
    auto compact = [&]() {
        std::vector<std::vector<int>> faces;
        std::vector<std::vector<Vec2>> faceUVs;
        for (size_t f = 0; f < m.faces.size(); ++f) {
            std::vector<int> face;
            std::vector<Vec2> fu;
            const auto& src = m.faces[f];
            for (size_t i = 0; i < src.size(); ++i) {
                if (!face.empty() && face.back() == src[i]) continue;
                face.push_back(src[i]);
                if (uvs) fu.push_back(m.uvs[f][i]);
            }
            while (face.size() > 1 && face.front() == face.back()) {
                face.pop_back();
                if (uvs) fu.pop_back();
            }
            if (face.size() < 3) {
                st.degenerateFaces++;
                continue;
            }
            faces.push_back(std::move(face));
            if (uvs) faceUVs.push_back(std::move(fu));
        }
        m.faces.swap(faces);
        m.uvs.swap(faceUVs);
    };
    compact();

    // 3. T-junctions: vertices lying on the interior of another face's edge.
    //    A uniform grid over the vertices (cells about one edge long) finds
    //    the candidates near each edge.
    {
        double sum = 0;
        size_t count = 0;
        for (size_t f = 0; f < m.faces.size() && count < 20000; f += 1 + m.faces.size() / 5000) {
            const auto& fc = m.faces[f];
            for (size_t i = 0; i < fc.size(); ++i, ++count) sum += length(m.verts[fc[i]] - m.verts[fc[(i + 1) % fc.size()]]);
        }
        const float avgEdge = count ? (float)(sum / count) : size / 64.0f;
        const float cell = std::max(std::min(avgEdge, size / 8.0f), eps * 8.0f);
        PointGrid grid;
        grid.build(m.verts, cell);
        std::vector<std::pair<float, int>> onEdge;
        std::vector<int> face;
        std::vector<Vec2> fu;
        for (size_t f = 0; f < m.faces.size(); ++f) {
            const auto& src = m.faces[f];
            bool changed = false;
            for (size_t i = 0; i < src.size(); ++i) {
                const int a = src[i], b = src[(i + 1) % src.size()];
                const Vec3 pa = m.verts[a], pb = m.verts[b], ab = pb - pa;
                const float len2 = dot(ab, ab);
                onEdge.clear();
                if (len2 >= eps * eps) {
                    long long x0, y0, z0, x1, y1, z1;
                    grid.coords(vmin(pa, pb) - Vec3(eps, eps, eps), x0, y0, z0);
                    grid.coords(vmax(pa, pb) + Vec3(eps, eps, eps), x1, y1, z1);
                    if ((x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) <= 20000)  // skip absurdly long edges
                        for (long long x = x0; x <= x1; ++x)
                            for (long long y = y0; y <= y1; ++y)
                                for (long long z = z0; z <= z1; ++z)
                                    grid.forCell(x, y, z, [&](int v) {
                                        if (v == a || v == b) return;
                                        float t = dot(m.verts[v] - pa, ab) / len2;
                                        if (t <= 1e-4f || t >= 1.0f - 1e-4f) return;
                                        if (length(pa + ab * t - m.verts[v]) <= eps * 4.0f) onEdge.push_back({t, v});
                                    });
                }
                if (!onEdge.empty() && !changed) {
                    // First T-junction of this face: copy the corners so far.
                    changed = true;
                    face.assign(src.begin(), src.begin() + i);
                    fu.clear();
                    if (uvs) fu.assign(m.uvs[f].begin(), m.uvs[f].begin() + i);
                }
                if (!changed) continue;
                face.push_back(a);
                if (uvs) fu.push_back(m.uvs[f][i]);
                std::sort(onEdge.begin(), onEdge.end());
                for (auto [t, v] : onEdge) {
                    if (face.back() == v) continue;
                    face.push_back(v);
                    if (uvs) fu.push_back(m.uvs[f][i] + (m.uvs[f][(i + 1) % src.size()] - m.uvs[f][i]) * t);
                    st.tJunctionsFixed++;
                }
            }
            if (changed) {
                m.faces[f] = face;
                if (uvs) m.uvs[f] = fu;
            }
        }
    }

    // 4. Zero-area faces.
    {
        std::vector<std::vector<int>> faces;
        std::vector<std::vector<Vec2>> faceUVs;
        for (size_t f = 0; f < m.faces.size(); ++f) {
            if (length(faceNormalRaw(m, m.faces[f])) <= eps * eps) {
                st.degenerateFaces++;
                continue;
            }
            faces.push_back(std::move(m.faces[f]));
            if (uvs) faceUVs.push_back(std::move(m.uvs[f]));
        }
        m.faces.swap(faces);
        m.uvs.swap(faceUVs);
    }
    removeUnusedVertices(m);
    m.validate();
    m.touch();
    return st;
}

int triangulateMesh(Mesh& m, int minSides) {
    minSides = std::max(4, minSides);
    const bool uvs = m.hasUVs();
    std::vector<std::vector<int>> faces;
    std::vector<std::vector<Vec2>> faceUVs;
    std::vector<int> corners;
    int split = 0;
    for (size_t f = 0; f < m.faces.size(); ++f) {
        const auto& face = m.faces[f];
        if ((int)face.size() < minSides) {
            faces.push_back(face);
            if (uvs) faceUVs.push_back(m.uvs[f]);
            continue;
        }
        corners.clear();
        triangulate(m.verts, face, corners);
        for (size_t t = 0; t + 2 < corners.size(); t += 3) {
            faces.push_back({face[corners[t]], face[corners[t + 1]], face[corners[t + 2]]});
            if (uvs) faceUVs.push_back({m.uvs[f][corners[t]], m.uvs[f][corners[t + 1]], m.uvs[f][corners[t + 2]]});
        }
        ++split;
    }
    m.faces.swap(faces);
    m.uvs.swap(faceUVs);
    m.validate();
    m.touch();
    return split;
}

int trisToQuads(Mesh& m, float maxAngleDeg) {
    const bool uvs = m.hasUVs();
    const float cosLimit = std::cos(toRadians(maxAngleDeg));
    // Candidate pairs: triangles sharing an edge, scored by planarity.
    std::unordered_map<uint64_t, std::vector<int>> edgeTris;
    for (int f = 0; f < (int)m.faces.size(); ++f) {
        const auto& t = m.faces[f];
        if (t.size() != 3) continue;
        for (int i = 0; i < 3; ++i) edgeTris[edgeKey(t[i], t[(i + 1) % 3])].push_back(f);
    }
    struct Pair {
        float score;
        int a, b;
        uint64_t edge;
    };
    std::vector<Pair> pairs;
    for (const auto& kv : edgeTris) {
        if (kv.second.size() != 2) continue;
        int a = kv.second[0], b = kv.second[1];
        float c = dot(normalize(faceNormalRaw(m, m.faces[a])), normalize(faceNormalRaw(m, m.faces[b])));
        if (c >= cosLimit) pairs.push_back({c, a, b, kv.first});
    }
    std::sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) { return x.score > y.score; });
    std::vector<char> used(m.faces.size(), 0);
    std::vector<char> remove(m.faces.size(), 0);
    int quads = 0;
    for (const Pair& pr : pairs) {
        if (used[pr.a] || used[pr.b]) continue;
        const auto &ta = m.faces[pr.a], &tb = m.faces[pr.b];
        const int e0 = int(pr.edge >> 32), e1 = int(pr.edge & 0xFFFFFFFFu);
        // Corner of A opposite the shared edge, rotated to the front: [o, x, y].
        int ia = 0;
        while (ta[ia] == e0 || ta[ia] == e1) ++ia;
        int x = ta[(ia + 1) % 3], y = ta[(ia + 2) % 3];
        int ib = 0;
        while (tb[ib] == e0 || tb[ib] == e1) ++ib;
        std::vector<int> quad = {ta[ia], x, tb[ib], y};
        if (!isConvex(m.verts, quad)) continue;
        std::vector<Vec2> quv;
        if (uvs) {
            auto uvOf = [&](int f, int v) {
                const auto& face = m.faces[f];
                for (size_t k = 0; k < face.size(); ++k)
                    if (face[k] == v) return m.uvs[f][k];
                return Vec2();
            };
            quv = {uvOf(pr.a, ta[ia]), uvOf(pr.a, x), uvOf(pr.b, tb[ib]), uvOf(pr.a, y)};
            // Only merge when both triangles agree on the shared edge's UVs (no seam).
            if (length(uvOf(pr.a, x) - uvOf(pr.b, x)) > 1e-5f || length(uvOf(pr.a, y) - uvOf(pr.b, y)) > 1e-5f) continue;
        }
        m.faces[pr.a] = quad;
        if (uvs) m.uvs[pr.a] = quv;
        used[pr.a] = used[pr.b] = 1;
        remove[pr.b] = 1;
        ++quads;
    }
    std::vector<std::vector<int>> faces;
    std::vector<std::vector<Vec2>> faceUVs;
    for (size_t f = 0; f < m.faces.size(); ++f) {
        if (remove[f]) continue;
        faces.push_back(std::move(m.faces[f]));
        if (uvs) faceUVs.push_back(std::move(m.uvs[f]));
    }
    m.faces.swap(faces);
    m.uvs.swap(faceUVs);
    m.validate();
    m.touch();
    return quads;
}

}  // namespace poly
