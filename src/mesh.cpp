#include "mesh.h"

#include <algorithm>
#include <map>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace {
uint64_t g_versionCounter = 0;

inline uint64_t edgeKey(int a, int b) {
    if (a > b) std::swap(a, b);
    return (uint64_t(uint32_t(a)) << 32) | uint32_t(b);
}
inline uint64_t directedKey(int a, int b) { return (uint64_t(uint32_t(a)) << 32) | uint32_t(b); }

bool rayTriangle(Vec3 o, Vec3 d, Vec3 a, Vec3 b, Vec3 c, float& t) {
    Vec3 e1 = b - a, e2 = c - a;
    Vec3 p = cross(d, e2);
    float det = dot(e1, p);
    if (std::fabs(det) < 1e-12f) return false;
    float inv = 1.0f / det;
    Vec3 s = o - a;
    float u = dot(s, p) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    Vec3 q = cross(s, e1);
    float v = dot(d, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    t = dot(e2, q) * inv;
    return t > 1e-6f;
}

// Weighted blend of several weight sets (used by subdivision).
BoneWeights blendWeights(const std::vector<std::pair<const BoneWeights*, float>>& in) {
    int bones[16];
    float sums[16];
    int n = 0;
    for (const auto& item : in)
        for (int k = 0; k < 4; ++k) {
            int b = item.first->bone[k];
            if (b < 0) continue;
            float w = item.first->w[k] * item.second;
            int j = 0;
            while (j < n && bones[j] != b) ++j;
            if (j == n) {
                if (n == 16) continue;
                bones[n] = b;
                sums[n++] = 0;
            }
            sums[j] += w;
        }
    BoneWeights r;
    for (int j = 0; j < n; ++j) r.add(bones[j], sums[j]);
    r.normalize();
    return r;
}
}  // namespace

// ---------------------------------------------------------------------------
// BoneWeights
// ---------------------------------------------------------------------------
void BoneWeights::add(int b, float weight) {
    if (b < 0 || weight <= 0.0f) return;
    for (int k = 0; k < 4; ++k)
        if (bone[k] == b) {
            w[k] += weight;
            return;
        }
    int slot = -1;
    for (int k = 0; k < 4 && slot < 0; ++k)
        if (bone[k] < 0) slot = k;
    if (slot < 0) {  // full: replace the weakest influence if the new one is stronger
        slot = 0;
        for (int k = 1; k < 4; ++k)
            if (w[k] < w[slot]) slot = k;
        if (w[slot] >= weight) return;
    }
    bone[slot] = b;
    w[slot] = weight;
}

void BoneWeights::set(int b, float weight) {
    remove(b);
    if (weight > 0.0f) add(b, weight);
}

void BoneWeights::remove(int b) {
    for (int k = 0; k < 4; ++k)
        if (bone[k] == b) {
            bone[k] = -1;
            w[k] = 0;
        }
}

void BoneWeights::normalize() {
    float t = total();
    if (t <= 1e-8f) return;
    for (int k = 0; k < 4; ++k) w[k] = bone[k] >= 0 ? w[k] / t : 0.0f;
}

float BoneWeights::weightOf(int b) const {
    for (int k = 0; k < 4; ++k)
        if (bone[k] == b) return w[k];
    return 0.0f;
}

// ---------------------------------------------------------------------------
// Mesh basics
// ---------------------------------------------------------------------------
void Mesh::touch() { topology = version = ++g_versionCounter; }
void Mesh::touchPositions() { version = ++g_versionCounter; }

size_t Mesh::triangleCount() const {
    if (cachedTriTopology_ == topology && topology != 0) return cachedTris_;
    size_t n = 0;
    for (const auto& f : faces)
        if (f.size() >= 3) n += f.size() - 2;
    cachedTris_ = n;
    cachedTriTopology_ = topology;
    return n;
}

bool Mesh::bounds(Vec3& lo, Vec3& hi) const {
    if (verts.empty()) return false;
    if (cachedBoundsVersion_ != version || version == 0) {
        cachedLo_ = cachedHi_ = verts[0];
        for (const Vec3& v : verts) {
            cachedLo_ = vmin(cachedLo_, v);
            cachedHi_ = vmax(cachedHi_, v);
        }
        cachedBoundsVersion_ = version;
    }
    lo = cachedLo_;
    hi = cachedHi_;
    return true;
}

bool rayHitsBox(Vec3 o, Vec3 d, Vec3 lo, Vec3 hi, float tMax) {
    float t0 = 0.0f, t1 = tMax;
    for (int a = 0; a < 3; ++a) {
        // Slightly padded so rays grazing flat (zero-thickness) boxes still hit.
        float pad = 1e-4f * (1.0f + std::fabs(hi[a] - lo[a]));
        float l = lo[a] - pad, h = hi[a] + pad;
        if (std::fabs(d[a]) < 1e-12f) {
            if (o[a] < l || o[a] > h) return false;
            continue;
        }
        float inv = 1.0f / d[a];
        float ta = (l - o[a]) * inv, tb = (h - o[a]) * inv;
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) return false;
    }
    return true;
}

void Mesh::validate() {
    if (!uvs.empty()) {
        bool ok = uvs.size() == faces.size();
        for (size_t f = 0; ok && f < faces.size(); ++f) ok = uvs[f].size() == faces[f].size();
        if (!ok) uvs.clear();
    }
    if (!weights.empty() && weights.size() != verts.size()) weights.clear();
}

Vec3 faceNormalRaw(const std::vector<Vec3>& p, const std::vector<int>& f) {
    Vec3 n;
    const size_t count = f.size();
    for (size_t i = 0; i < count; ++i) {
        const Vec3& a = p[f[i]];
        const Vec3& b = p[f[(i + 1) % count]];
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    return n;
}

Vec3 faceNormalRaw(const Mesh& m, const std::vector<int>& f) { return faceNormalRaw(m.verts, f); }

Vec3 faceCenter(const Mesh& m, const std::vector<int>& f) {
    Vec3 c;
    for (int i : f) c += m.verts[i];
    return f.empty() ? c : c / float(f.size());
}

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------
namespace primitives {

Mesh box(float w, float h, float d, int n) {
    n = std::max(1, n);
    const float hx = w * 0.5f, hy = h * 0.5f, hz = d * 0.5f;
    struct Side {
        Vec3 o, u, v;  // origin corner and spanning edges; u x v points outward
    };
    const Side sides[6] = {
        {{-hx, -hy, hz}, {w, 0, 0}, {0, h, 0}},    // +Z
        {{hx, -hy, -hz}, {-w, 0, 0}, {0, h, 0}},   // -Z
        {{hx, -hy, hz}, {0, 0, -d}, {0, h, 0}},    // +X
        {{-hx, -hy, -hz}, {0, 0, d}, {0, h, 0}},   // -X
        {{-hx, hy, hz}, {w, 0, 0}, {0, 0, -d}},    // +Y
        {{-hx, -hy, -hz}, {w, 0, 0}, {0, 0, d}},   // -Y
    };
    Mesh m;
    for (const Side& s : sides) {
        int base = (int)m.verts.size();
        for (int j = 0; j <= n; ++j)
            for (int i = 0; i <= n; ++i) m.verts.push_back(s.o + s.u * (float(i) / n) + s.v * (float(j) / n));
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                auto idx = [&](int a, int b) { return base + b * (n + 1) + a; };
                m.faces.push_back({idx(i, j), idx(i + 1, j), idx(i + 1, j + 1), idx(i, j + 1)});
                float u0 = float(i) / n, u1 = float(i + 1) / n, v0 = float(j) / n, v1 = float(j + 1) / n;
                m.uvs.push_back({{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}});
            }
    }
    weldVertices(m, 1e-5f * std::max(w, std::max(h, d)));
    m.touch();
    return m;
}

Mesh cube(float size) { return box(size, size, size, 1); }

Mesh grid(float w, float d, int sx, int sz) {
    sx = std::max(1, sx);
    sz = std::max(1, sz);
    Mesh m;
    for (int j = 0; j <= sz; ++j)
        for (int i = 0; i <= sx; ++i) m.verts.push_back({-w * 0.5f + w * i / sx, 0.0f, d * 0.5f - d * j / sz});
    auto idx = [sx](int i, int j) { return j * (sx + 1) + i; };
    for (int j = 0; j < sz; ++j)
        for (int i = 0; i < sx; ++i) {
            m.faces.push_back({idx(i, j), idx(i + 1, j), idx(i + 1, j + 1), idx(i, j + 1)});
            float u0 = float(i) / sx, u1 = float(i + 1) / sx, v0 = float(j) / sz, v1 = float(j + 1) / sz;
            m.uvs.push_back({{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}});
        }
    m.touch();
    return m;
}

Mesh plane(float size, int n) { return grid(size, size, n, n); }

Mesh uvSphere(float r, int segments, int rings) {
    segments = std::max(3, segments);
    rings = std::max(2, rings);
    Mesh m;
    m.verts.push_back({0, r, 0});  // north pole
    for (int k = 1; k < rings; ++k) {
        float phi = kPi * k / rings;
        float y = r * std::cos(phi), rr = r * std::sin(phi);
        for (int s = 0; s < segments; ++s) {
            float theta = 2.0f * kPi * s / segments;
            m.verts.push_back({rr * std::sin(theta), y, rr * std::cos(theta)});
        }
    }
    const int south = (int)m.verts.size();
    m.verts.push_back({0, -r, 0});
    auto ring = [segments](int k, int s) { return 1 + (k - 1) * segments + (s % segments); };
    auto uv = [segments, rings](int k, float s) { return Vec2(s / segments, 1.0f - float(k) / rings); };
    for (int s = 0; s < segments; ++s) {
        m.faces.push_back({0, ring(1, s), ring(1, s + 1)});
        m.uvs.push_back({uv(0, s + 0.5f), uv(1, (float)s), uv(1, s + 1.0f)});
    }
    for (int k = 1; k < rings - 1; ++k)
        for (int s = 0; s < segments; ++s) {
            m.faces.push_back({ring(k, s), ring(k + 1, s), ring(k + 1, s + 1), ring(k, s + 1)});
            m.uvs.push_back({uv(k, (float)s), uv(k + 1, (float)s), uv(k + 1, s + 1.0f), uv(k, s + 1.0f)});
        }
    for (int s = 0; s < segments; ++s) {
        m.faces.push_back({ring(rings - 1, s), south, ring(rings - 1, s + 1)});
        m.uvs.push_back({uv(rings - 1, (float)s), uv(rings, s + 0.5f), uv(rings - 1, s + 1.0f)});
    }
    m.touch();
    return m;
}

Mesh frustum(float rb, float rt, float height, int segments) {
    segments = std::max(3, segments);
    const float h = height * 0.5f;
    const bool apex = rt <= 1e-6f;
    Mesh m;
    for (int s = 0; s < segments; ++s) {
        float t = 2.0f * kPi * s / segments;
        m.verts.push_back({rb * std::sin(t), -h, rb * std::cos(t)});
    }
    int top = (int)m.verts.size();
    if (apex) {
        m.verts.push_back({0, h, 0});
    } else {
        for (int s = 0; s < segments; ++s) {
            float t = 2.0f * kPi * s / segments;
            m.verts.push_back({rt * std::sin(t), h, rt * std::cos(t)});
        }
    }
    for (int s = 0; s < segments; ++s) {
        int s1 = (s + 1) % segments;
        float u0 = float(s) / segments, u1 = float(s + 1) / segments;
        if (apex) {
            m.faces.push_back({top, s, s1});
            m.uvs.push_back({{(u0 + u1) * 0.5f, 1}, {u0, 0}, {u1, 0}});
        } else {
            m.faces.push_back({top + s, s, s1, top + s1});
            m.uvs.push_back({{u0, 1}, {u0, 0}, {u1, 0}, {u1, 1}});
        }
    }
    auto capUV = [](Vec3 p, float r, float flip) {
        return Vec2(0.5f + p.x / (2 * r), 0.5f + flip * p.z / (2 * r));
    };
    if (!apex) {
        std::vector<int> cap;
        std::vector<Vec2> capUVs;
        for (int s = 0; s < segments; ++s) {
            cap.push_back(top + s);
            capUVs.push_back(capUV(m.verts[top + s], rt, -1.0f));
        }
        m.faces.push_back(cap);
        m.uvs.push_back(capUVs);
    }
    std::vector<int> bottom;
    std::vector<Vec2> bottomUVs;
    for (int s = segments - 1; s >= 0; --s) {
        bottom.push_back(s);
        bottomUVs.push_back(capUV(m.verts[s], rb, 1.0f));
    }
    m.faces.push_back(bottom);
    m.uvs.push_back(bottomUVs);
    m.touch();
    return m;
}

Mesh cylinder(float r, float height, int segments) { return frustum(r, r, height, segments); }
Mesh cone(float r, float height, int segments) { return frustum(r, 0.0f, height, segments); }

Mesh torus(float R, float r, int segU, int segV) {
    segU = std::max(3, segU);
    segV = std::max(3, segV);
    Mesh m;
    for (int u = 0; u < segU; ++u) {
        float theta = 2.0f * kPi * u / segU;
        Vec3 d{std::sin(theta), 0.0f, std::cos(theta)};
        for (int v = 0; v < segV; ++v) {
            float phi = 2.0f * kPi * v / segV;
            m.verts.push_back(d * (R + r * std::cos(phi)) + Vec3(0, r * std::sin(phi), 0));
        }
    }
    auto idx = [segU, segV](int u, int v) { return (u % segU) * segV + (v % segV); };
    for (int u = 0; u < segU; ++u)
        for (int v = 0; v < segV; ++v) {
            m.faces.push_back({idx(u, v), idx(u + 1, v), idx(u + 1, v + 1), idx(u, v + 1)});
            float u0 = float(u) / segU, u1 = float(u + 1) / segU, v0 = float(v) / segV, v1 = float(v + 1) / segV;
            m.uvs.push_back({{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}});
        }
    m.touch();
    return m;
}

}  // namespace primitives

// ---------------------------------------------------------------------------
// Catmull-Clark subdivision
// ---------------------------------------------------------------------------
Mesh catmullClark(const Mesh& in) {
    const int V = (int)in.verts.size();
    const int F = (int)in.faces.size();
    const bool uvs = in.hasUVs(), weights = in.hasWeights();

    struct Edge {
        int a, b;
        int f[2] = {-1, -1};
        int count = 0;
    };
    std::vector<Edge> edges;
    std::unordered_map<uint64_t, int> edgeIndex;
    edgeIndex.reserve(size_t(F) * 4);
    std::vector<std::vector<int>> faceEdges(F);  // faceEdges[f][i] = edge (face[i] -> face[i+1])
    std::vector<Vec3> facePts(F);

    for (int f = 0; f < F; ++f) {
        const auto& face = in.faces[f];
        if (face.size() < 3) continue;
        facePts[f] = faceCenter(in, face);
        for (size_t i = 0; i < face.size(); ++i) {
            int a = face[i], b = face[(i + 1) % face.size()];
            auto [it, inserted] = edgeIndex.try_emplace(edgeKey(a, b), (int)edges.size());
            if (inserted) edges.push_back({a, b});
            Edge& e = edges[it->second];
            if (e.count < 2) e.f[e.count] = f;
            e.count++;
            faceEdges[f].push_back(it->second);
        }
    }
    const int E = (int)edges.size();

    // Edge points: average of endpoints and adjacent face points (interior),
    // plain midpoint on boundary / non-manifold edges.
    std::vector<Vec3> edgePts(E);
    for (int i = 0; i < E; ++i) {
        const Edge& e = edges[i];
        Vec3 pa = in.verts[e.a], pb = in.verts[e.b];
        if (e.count == 2)
            edgePts[i] = (pa + pb + facePts[e.f[0]] + facePts[e.f[1]]) * 0.25f;
        else
            edgePts[i] = (pa + pb) * 0.5f;
    }

    // Vertex points.
    std::vector<Vec3> faceSum(V), midSum(V), boundarySum(V);
    std::vector<int> faceCount(V, 0), edgeCount(V, 0), boundaryCount(V, 0);
    std::vector<char> nonManifold(V, 0);
    for (int f = 0; f < F; ++f) {
        if (in.faces[f].size() < 3) continue;
        for (int v : in.faces[f]) {
            faceSum[v] += facePts[f];
            faceCount[v]++;
        }
    }
    for (const Edge& e : edges) {
        Vec3 mid = (in.verts[e.a] + in.verts[e.b]) * 0.5f;
        midSum[e.a] += mid; edgeCount[e.a]++;
        midSum[e.b] += mid; edgeCount[e.b]++;
        if (e.count == 1) {
            boundaryCount[e.a]++; boundarySum[e.a] += in.verts[e.b];
            boundaryCount[e.b]++; boundarySum[e.b] += in.verts[e.a];
        } else if (e.count > 2) {
            nonManifold[e.a] = nonManifold[e.b] = 1;
        }
    }

    Mesh out;
    out.verts.resize(size_t(V) + E + F);
    for (int v = 0; v < V; ++v) {
        Vec3 P = in.verts[v];
        int n = faceCount[v];
        if (nonManifold[v] || n == 0) {
            out.verts[v] = P;
        } else if (boundaryCount[v] == 2) {
            out.verts[v] = P * 0.75f + boundarySum[v] * 0.125f;
        } else if (boundaryCount[v] > 0 || n < 3) {
            out.verts[v] = P;  // corner: keep sharp
        } else {
            Vec3 Favg = faceSum[v] / float(n);
            Vec3 Ravg = midSum[v] / float(edgeCount[v]);
            out.verts[v] = (Favg + Ravg * 2.0f + P * float(n - 3)) / float(n);
        }
    }
    for (int i = 0; i < E; ++i) out.verts[V + i] = edgePts[i];
    for (int f = 0; f < F; ++f) out.verts[V + E + f] = facePts[f];

    if (weights) {
        out.weights.resize(out.verts.size());
        for (int v = 0; v < V; ++v) out.weights[v] = in.weights[v];
        for (int i = 0; i < E; ++i)
            out.weights[V + i] = blendWeights({{&in.weights[edges[i].a], 0.5f}, {&in.weights[edges[i].b], 0.5f}});
        for (int f = 0; f < F; ++f) {
            std::vector<std::pair<const BoneWeights*, float>> parts;
            for (int v : in.faces[f]) parts.push_back({&in.weights[v], 1.0f / in.faces[f].size()});
            if (!parts.empty()) out.weights[V + E + f] = blendWeights(parts);
        }
    }

    for (int f = 0; f < F; ++f) {
        const auto& face = in.faces[f];
        const int n = (int)face.size();
        if (n < 3) continue;
        Vec2 uvCenter;
        if (uvs) {
            for (const Vec2& t : in.uvs[f]) uvCenter += t;
            uvCenter = uvCenter / float(n);
        }
        for (int i = 0; i < n; ++i) {
            int eNext = faceEdges[f][i];
            int ePrev = faceEdges[f][(i + n - 1) % n];
            out.faces.push_back({face[i], V + eNext, V + E + f, V + ePrev});
            if (uvs) {
                const Vec2 a = in.uvs[f][i], next = in.uvs[f][(i + 1) % n], prev = in.uvs[f][(i + n - 1) % n];
                out.uvs.push_back({a, (a + next) * 0.5f, uvCenter, (a + prev) * 0.5f});
            }
        }
    }
    removeUnusedVertices(out);
    out.touch();
    return out;
}

void flipNormals(Mesh& m) {
    for (auto& f : m.faces) std::reverse(f.begin(), f.end());
    for (auto& u : m.uvs) std::reverse(u.begin(), u.end());
    m.touch();
}

void weldVertices(Mesh& m, float eps) {
    const float inv = 1.0f / std::max(eps, 1e-12f);
    std::map<std::tuple<long long, long long, long long>, int> cells;
    std::vector<int> remap(m.verts.size());
    std::vector<Vec3> kept;
    std::vector<BoneWeights> keptW;
    for (size_t i = 0; i < m.verts.size(); ++i) {
        const Vec3& p = m.verts[i];
        auto key = std::make_tuple((long long)std::llround(p.x * inv), (long long)std::llround(p.y * inv),
                                   (long long)std::llround(p.z * inv));
        auto it = cells.find(key);
        if (it == cells.end()) {
            it = cells.emplace(key, (int)kept.size()).first;
            kept.push_back(p);
            if (m.hasWeights()) keptW.push_back(m.weights[i]);
        }
        remap[i] = it->second;
    }
    m.verts.swap(kept);
    if (!keptW.empty()) m.weights.swap(keptW);
    for (auto& f : m.faces)
        for (int& v : f) v = remap[v];
}

std::vector<int> removeUnusedVertices(Mesh& m) {
    std::vector<char> used(m.verts.size(), 0);
    for (const auto& f : m.faces)
        for (int v : f) used[v] = 1;
    const bool w = m.hasWeights();
    std::vector<int> remap(m.verts.size(), -1);
    std::vector<Vec3> kept;
    std::vector<BoneWeights> keptW;
    kept.reserve(m.verts.size());
    for (size_t i = 0; i < m.verts.size(); ++i)
        if (used[i]) {
            remap[i] = (int)kept.size();
            kept.push_back(m.verts[i]);
            if (w) keptW.push_back(m.weights[i]);
        }
    m.verts.swap(kept);
    if (w) m.weights.swap(keptW);
    for (auto& f : m.faces)
        for (int& v : f) v = remap[v];
    return remap;
}

void deleteVertices(Mesh& m, std::vector<char>& sel) {
    sel.resize(m.verts.size(), 0);
    const bool uvs = m.hasUVs();
    std::vector<std::vector<int>> keptFaces;
    std::vector<std::vector<Vec2>> keptUVs;
    for (size_t f = 0; f < m.faces.size(); ++f) {
        bool touches = false;
        for (int v : m.faces[f])
            if (sel[v]) { touches = true; break; }
        if (touches) continue;
        keptFaces.push_back(std::move(m.faces[f]));
        if (uvs) keptUVs.push_back(std::move(m.uvs[f]));
    }
    m.faces.swap(keptFaces);
    m.uvs.swap(keptUVs);
    removeUnusedVertices(m);
    sel.assign(m.verts.size(), 0);
    m.validate();
    m.touch();
}

std::vector<int> selectedFaces(const Mesh& m, const std::vector<char>& sel) {
    std::vector<int> out;
    if (sel.size() != m.verts.size()) return out;
    for (int f = 0; f < (int)m.faces.size(); ++f) {
        const auto& face = m.faces[f];
        if (face.size() < 3) continue;
        bool all = true;
        for (int v : face)
            if (!sel[v]) { all = false; break; }
        if (all) out.push_back(f);
    }
    return out;
}

bool extrudeSelectedFaces(Mesh& m, std::vector<char>& sel, Vec3* outNormal) {
    sel.resize(m.verts.size(), 0);
    std::vector<int> region = selectedFaces(m, sel);
    if (region.empty()) return false;
    const bool uvs = m.hasUVs(), weights = m.hasWeights();

    // Directed edges inside the region; an edge is on the region boundary when
    // its reverse is not also part of the region.
    std::unordered_set<uint64_t> directed;
    for (int f : region) {
        const auto& face = m.faces[f];
        for (size_t i = 0; i < face.size(); ++i) directed.insert(directedKey(face[i], face[(i + 1) % face.size()]));
    }

    Vec3 normal;
    std::unordered_map<int, int> dup;  // old vertex -> extruded copy
    auto copyOf = [&](int v) {
        auto it = dup.find(v);
        if (it != dup.end()) return it->second;
        int n = (int)m.verts.size();
        Vec3 p = m.verts[v];
        m.verts.push_back(p);
        if (weights) {
            BoneWeights w = m.weights[v];
            m.weights.push_back(w);
        }
        dup[v] = n;
        return n;
    };

    std::vector<std::vector<int>> sideFaces;
    std::vector<std::vector<Vec2>> sideUVs;
    for (int f : region) {
        normal += faceNormalRaw(m, m.faces[f]);
        const auto face = m.faces[f];  // copy: m.verts may grow below
        for (size_t i = 0; i < face.size(); ++i) {
            size_t j = (i + 1) % face.size();
            int a = face[i], b = face[j];
            if (directed.count(directedKey(b, a))) continue;
            sideFaces.push_back({a, b, copyOf(b), copyOf(a)});
            if (uvs) {
                Vec2 ua = m.uvs[f][i], ub = m.uvs[f][j];
                // Give the wall a strip of UV space next to its edge.
                Vec2 e = ub - ua;
                Vec2 off(-e.y * 0.25f, e.x * 0.25f);
                sideUVs.push_back({ua, ub, ub + off, ua + off});
            }
        }
    }
    for (int f : region)
        for (int& v : m.faces[f]) v = copyOf(v);
    for (size_t i = 0; i < sideFaces.size(); ++i) {
        m.faces.push_back(std::move(sideFaces[i]));
        if (uvs) m.uvs.push_back(std::move(sideUVs[i]));
    }

    sel.assign(m.verts.size(), 0);
    for (const auto& kv : dup) sel[kv.second] = 1;

    std::vector<int> remap = removeUnusedVertices(m);
    std::vector<char> newSel(m.verts.size(), 0);
    for (size_t i = 0; i < remap.size(); ++i)
        if (remap[i] >= 0) newSel[remap[i]] = sel[i];
    sel.swap(newSel);

    if (outNormal) *outNormal = normalize(normal);
    m.validate();
    m.touch();
    return true;
}

std::vector<std::pair<int, int>> uniqueEdges(const Mesh& m) {
    std::vector<uint64_t> keys;
    keys.reserve(m.triangleCount() + m.faces.size() * 2);
    for (const auto& f : m.faces)
        for (size_t i = 0; i < f.size(); ++i) keys.push_back(edgeKey(f[i], f[(i + 1) % f.size()]));
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    std::vector<std::pair<int, int>> out;
    out.reserve(keys.size());
    for (uint64_t k : keys) out.push_back({int(k >> 32), int(k & 0xFFFFFFFFu)});
    return out;
}

bool raycastMesh(const Mesh& m, const std::vector<Vec3>& p, Vec3 o, Vec3 d, float& tHit) {
    bool hit = false;
    float best = 1e30f;
    for (const auto& f : m.faces) {
        if (f.size() < 3) continue;
        const Vec3& a = p[f[0]];
        for (size_t i = 1; i + 1 < f.size(); ++i) {
            float t;
            if (rayTriangle(o, d, a, p[f[i]], p[f[i + 1]], t) && t < best) {
                best = t;
                hit = true;
            }
        }
    }
    if (hit) tHit = best;
    return hit;
}

bool raycastMesh(const Mesh& m, Vec3 o, Vec3 d, float& tHit) { return raycastMesh(m, m.verts, o, d, tHit); }

void buildRenderMesh(const Mesh& m, const std::vector<Vec3>& p, bool smooth, float smoothAngleDeg, int weightSlot,
                     std::vector<RenderVertex>& outV, std::vector<uint32_t>& outI) {
    outV.clear();
    outI.clear();
    outI.reserve(m.triangleCount() * 3);
    outV.reserve(smooth ? p.size() + p.size() / 4 : m.triangleCount() + m.faces.size() * 2);
    const size_t F = m.faces.size();
    const bool uvs = m.hasUVs(), weights = weightSlot >= 0 && m.hasWeights();
    std::vector<Vec3> rawN(F), unitN(F);
    for (size_t f = 0; f < F; ++f) {
        if (m.faces[f].size() < 3) continue;
        rawN[f] = faceNormalRaw(p, m.faces[f]);
        unitN[f] = normalize(rawN[f]);
    }
    const float cosLimit = std::cos(toRadians(smoothAngleDeg));

    // Fast path: if every face around a vertex is within half the smoothing
    // angle of the vertex's average normal, all pairs are within the full
    // angle, so every corner there gets the same (vertex) normal - no
    // per-corner neighbour loop needed. True for most of a smooth surface.
    std::vector<Vec3> vertexN;
    std::vector<char> simple;
    bool anyComplex = false;
    if (smooth) {
        vertexN.assign(p.size(), Vec3());
        simple.assign(p.size(), 1);
        for (size_t f = 0; f < F; ++f)
            for (int v : m.faces[f]) vertexN[v] += rawN[f];
        for (Vec3& n : vertexN) n = normalize(n);
        const float cosHalf = std::cos(toRadians(smoothAngleDeg) * 0.5f);
        for (size_t f = 0; f < F; ++f)
            for (int v : m.faces[f])
                if (dot(unitN[f], vertexN[v]) < cosHalf || dot(vertexN[v], vertexN[v]) < 0.5f) {
                    simple[v] = 0;
                    anyComplex = true;
                }
    }

    // Vertex -> faces adjacency in compressed-row form (one allocation instead
    // of one small vector per vertex); only needed for the non-simple vertices.
    std::vector<int> adjStart, adjFaces;
    if (smooth && anyComplex) {
        adjStart.assign(p.size() + 1, 0);
        for (const auto& face : m.faces)
            for (int v : face) adjStart[v + 1]++;
        for (size_t v = 0; v < p.size(); ++v) adjStart[v + 1] += adjStart[v];
        adjFaces.resize(adjStart.back());
        std::vector<int> fill(adjStart.begin(), adjStart.end() - 1);
        for (size_t f = 0; f < F; ++f)
            for (int v : m.faces[f]) adjFaces[fill[v]++] = (int)f;
    }

    // Indexed output: a smooth ("simple") vertex becomes one GPU vertex shared
    // by all its faces, unless a UV seam gives a corner a different UV. Other
    // corners are emitted once per face (and shared by that face's triangles).
    std::vector<int> shared(smooth ? p.size() : 0, -1);
    std::vector<uint32_t> corner;
    for (size_t f = 0; f < F; ++f) {
        const auto& face = m.faces[f];
        const size_t n = face.size();
        if (n < 3) continue;
        corner.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const int v = face[i];
            const Vec2 uv = uvs ? m.uvs[f][i] : Vec2();
            if (smooth && simple[v]) {
                int s = shared[v];
                if (s >= 0 && outV[s].uv.x == uv.x && outV[s].uv.y == uv.y) {
                    corner[i] = (uint32_t)s;
                    continue;
                }
            }
            Vec3 normal = unitN[f];
            if (smooth) {
                if (simple[v]) {
                    normal = vertexN[v];
                } else {
                    Vec3 sum;
                    for (int k = adjStart[v]; k < adjStart[v + 1]; ++k) {
                        int g = adjFaces[k];
                        if (dot(unitN[g], unitN[f]) >= cosLimit) sum += rawN[g];  // area weighted
                    }
                    Vec3 nn = normalize(sum);
                    if (dot(nn, nn) > 0.5f) normal = nn;
                }
            }
            RenderVertex rv;
            rv.pos = p[v];
            rv.normal = normal;
            rv.uv = uv;
            rv.weight = weights ? m.weights[v].weightOf(weightSlot) : 0.0f;
            corner[i] = (uint32_t)outV.size();
            if (smooth && simple[v] && shared[v] < 0) shared[v] = (int)outV.size();
            outV.push_back(rv);
        }
        for (size_t i = 1; i + 1 < n; ++i) {
            outI.push_back(corner[0]);
            outI.push_back(corner[i]);
            outI.push_back(corner[i + 1]);
        }
    }
}

void buildRenderData(const Mesh& m, const std::vector<Vec3>& p, bool smooth, float smoothAngleDeg, int weightSlot,
                     std::vector<RenderVertex>& out) {
    std::vector<RenderVertex> verts;
    std::vector<uint32_t> indices;
    buildRenderMesh(m, p, smooth, smoothAngleDeg, weightSlot, verts, indices);
    out.clear();
    out.reserve(indices.size());
    for (uint32_t i : indices) out.push_back(verts[i]);
}
