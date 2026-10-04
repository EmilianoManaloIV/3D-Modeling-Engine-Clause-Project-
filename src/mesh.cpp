#include "mesh.h"

#include <algorithm>
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
}  // namespace

void Mesh::touch() { version = ++g_versionCounter; }

size_t Mesh::triangleCount() const {
    size_t n = 0;
    for (const auto& f : faces)
        if (f.size() >= 3) n += f.size() - 2;
    return n;
}

Vec3 faceNormalRaw(const Mesh& m, const std::vector<int>& f) {
    Vec3 n;
    const size_t count = f.size();
    for (size_t i = 0; i < count; ++i) {
        const Vec3& a = m.verts[f[i]];
        const Vec3& b = m.verts[f[(i + 1) % count]];
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    return n;
}

Vec3 faceCenter(const Mesh& m, const std::vector<int>& f) {
    Vec3 c;
    for (int i : f) c += m.verts[i];
    return f.empty() ? c : c / float(f.size());
}

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------
namespace primitives {

Mesh cube(float size) {
    float h = size * 0.5f;
    Mesh m;
    m.verts = {{-h, -h, -h}, {h, -h, -h}, {h, h, -h}, {-h, h, -h},
               {-h, -h, h},  {h, -h, h},  {h, h, h},  {-h, h, h}};
    m.faces = {{4, 5, 6, 7},   // +Z
               {1, 0, 3, 2},   // -Z
               {5, 1, 2, 6},   // +X
               {0, 4, 7, 3},   // -X
               {7, 6, 2, 3},   // +Y
               {0, 1, 5, 4}};  // -Y
    m.touch();
    return m;
}

Mesh plane(float size, int n) {
    n = std::max(1, n);
    float h = size * 0.5f;
    Mesh m;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
            m.verts.push_back({-h + size * i / n, 0.0f, -h + size * j / n});
    auto idx = [n](int i, int j) { return j * (n + 1) + i; };
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
            m.faces.push_back({idx(i, j + 1), idx(i + 1, j + 1), idx(i + 1, j), idx(i, j)});
    m.touch();
    return m;
}

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
    for (int s = 0; s < segments; ++s) m.faces.push_back({0, ring(1, s), ring(1, s + 1)});
    for (int k = 1; k < rings - 1; ++k)
        for (int s = 0; s < segments; ++s)
            m.faces.push_back({ring(k, s), ring(k + 1, s), ring(k + 1, s + 1), ring(k, s + 1)});
    for (int s = 0; s < segments; ++s) m.faces.push_back({ring(rings - 1, s), south, ring(rings - 1, s + 1)});
    m.touch();
    return m;
}

Mesh cylinder(float r, float height, int segments) {
    segments = std::max(3, segments);
    float h = height * 0.5f;
    Mesh m;
    for (int s = 0; s < segments; ++s) {
        float t = 2.0f * kPi * s / segments;
        m.verts.push_back({r * std::sin(t), -h, r * std::cos(t)});
    }
    for (int s = 0; s < segments; ++s) {
        float t = 2.0f * kPi * s / segments;
        m.verts.push_back({r * std::sin(t), h, r * std::cos(t)});
    }
    for (int s = 0; s < segments; ++s) {
        int s1 = (s + 1) % segments;
        m.faces.push_back({segments + s, s, s1, segments + s1});
    }
    std::vector<int> top, bottom;
    for (int s = 0; s < segments; ++s) top.push_back(segments + s);
    for (int s = segments - 1; s >= 0; --s) bottom.push_back(s);
    m.faces.push_back(top);
    m.faces.push_back(bottom);
    m.touch();
    return m;
}

Mesh cone(float r, float height, int segments) {
    segments = std::max(3, segments);
    float h = height * 0.5f;
    Mesh m;
    m.verts.push_back({0, h, 0});  // apex
    for (int s = 0; s < segments; ++s) {
        float t = 2.0f * kPi * s / segments;
        m.verts.push_back({r * std::sin(t), -h, r * std::cos(t)});
    }
    for (int s = 0; s < segments; ++s) m.faces.push_back({0, 1 + s, 1 + (s + 1) % segments});
    std::vector<int> base;
    for (int s = segments - 1; s >= 0; --s) base.push_back(1 + s);
    m.faces.push_back(base);
    m.touch();
    return m;
}

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
        for (int v = 0; v < segV; ++v)
            m.faces.push_back({idx(u, v), idx(u + 1, v), idx(u + 1, v + 1), idx(u, v + 1)});
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

    for (int f = 0; f < F; ++f) {
        const auto& face = in.faces[f];
        const int n = (int)face.size();
        if (n < 3) continue;
        for (int i = 0; i < n; ++i) {
            int eNext = faceEdges[f][i];
            int ePrev = faceEdges[f][(i + n - 1) % n];
            out.faces.push_back({face[i], V + eNext, V + E + f, V + ePrev});
        }
    }
    removeUnusedVertices(out);
    out.touch();
    return out;
}

void flipNormals(Mesh& m) {
    for (auto& f : m.faces) std::reverse(f.begin(), f.end());
    m.touch();
}

std::vector<int> removeUnusedVertices(Mesh& m) {
    std::vector<char> used(m.verts.size(), 0);
    for (const auto& f : m.faces)
        for (int v : f) used[v] = 1;
    std::vector<int> remap(m.verts.size(), -1);
    std::vector<Vec3> kept;
    kept.reserve(m.verts.size());
    for (size_t i = 0; i < m.verts.size(); ++i)
        if (used[i]) {
            remap[i] = (int)kept.size();
            kept.push_back(m.verts[i]);
        }
    m.verts.swap(kept);
    for (auto& f : m.faces)
        for (int& v : f) v = remap[v];
    return remap;
}

void deleteVertices(Mesh& m, std::vector<char>& sel) {
    sel.resize(m.verts.size(), 0);
    std::vector<std::vector<int>> keptFaces;
    for (auto& f : m.faces) {
        bool touches = false;
        for (int v : f)
            if (sel[v]) { touches = true; break; }
        if (!touches) keptFaces.push_back(std::move(f));
    }
    m.faces.swap(keptFaces);
    removeUnusedVertices(m);
    sel.assign(m.verts.size(), 0);
    m.touch();
}

bool extrudeSelectedFaces(Mesh& m, std::vector<char>& sel, Vec3* outNormal) {
    sel.resize(m.verts.size(), 0);
    std::vector<int> region;
    for (int f = 0; f < (int)m.faces.size(); ++f) {
        const auto& face = m.faces[f];
        if (face.size() < 3) continue;
        bool all = true;
        for (int v : face)
            if (!sel[v]) { all = false; break; }
        if (all) region.push_back(f);
    }
    if (region.empty()) return false;

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
        dup[v] = n;
        return n;
    };

    std::vector<std::vector<int>> sideFaces;
    for (int f : region) {
        normal += faceNormalRaw(m, m.faces[f]);
        const auto face = m.faces[f];  // copy: m.verts may grow below
        for (size_t i = 0; i < face.size(); ++i) {
            int a = face[i], b = face[(i + 1) % face.size()];
            if (!directed.count(directedKey(b, a))) sideFaces.push_back({a, b, copyOf(b), copyOf(a)});
        }
    }
    for (int f : region)
        for (int& v : m.faces[f]) v = copyOf(v);
    for (auto& sf : sideFaces) m.faces.push_back(std::move(sf));

    sel.assign(m.verts.size(), 0);
    for (const auto& kv : dup) sel[kv.second] = 1;

    std::vector<int> remap = removeUnusedVertices(m);
    std::vector<char> newSel(m.verts.size(), 0);
    for (size_t i = 0; i < remap.size(); ++i)
        if (remap[i] >= 0) newSel[remap[i]] = sel[i];
    sel.swap(newSel);

    if (outNormal) *outNormal = normalize(normal);
    m.touch();
    return true;
}

std::vector<std::pair<int, int>> uniqueEdges(const Mesh& m) {
    std::unordered_set<uint64_t> seen;
    std::vector<std::pair<int, int>> out;
    for (const auto& f : m.faces)
        for (size_t i = 0; i < f.size(); ++i) {
            int a = f[i], b = f[(i + 1) % f.size()];
            if (seen.insert(edgeKey(a, b)).second) out.push_back({a, b});
        }
    return out;
}

bool raycastMesh(const Mesh& m, Vec3 o, Vec3 d, float& tHit) {
    bool hit = false;
    float best = 1e30f;
    for (const auto& f : m.faces) {
        if (f.size() < 3) continue;
        const Vec3& a = m.verts[f[0]];
        for (size_t i = 1; i + 1 < f.size(); ++i) {
            float t;
            if (rayTriangle(o, d, a, m.verts[f[i]], m.verts[f[i + 1]], t) && t < best) {
                best = t;
                hit = true;
            }
        }
    }
    if (hit) tHit = best;
    return hit;
}

void buildRenderData(const Mesh& m, bool smooth, float smoothAngleDeg, std::vector<RenderVertex>& out) {
    out.clear();
    out.reserve(m.triangleCount() * 3);
    const size_t F = m.faces.size();
    std::vector<Vec3> rawN(F), unitN(F);
    for (size_t f = 0; f < F; ++f) {
        if (m.faces[f].size() < 3) continue;
        rawN[f] = faceNormalRaw(m, m.faces[f]);
        unitN[f] = normalize(rawN[f]);
    }

    std::vector<std::vector<int>> vertFaces;
    if (smooth) {
        vertFaces.resize(m.verts.size());
        for (size_t f = 0; f < F; ++f)
            for (int v : m.faces[f]) vertFaces[v].push_back((int)f);
    }
    const float cosLimit = std::cos(toRadians(smoothAngleDeg));

    std::vector<Vec3> cornerN;
    for (size_t f = 0; f < F; ++f) {
        const auto& face = m.faces[f];
        const size_t n = face.size();
        if (n < 3) continue;
        cornerN.assign(n, unitN[f]);
        if (smooth) {
            for (size_t i = 0; i < n; ++i) {
                Vec3 sum;
                for (int g : vertFaces[face[i]])
                    if (dot(unitN[g], unitN[f]) >= cosLimit) sum += rawN[g];  // area weighted
                Vec3 nn = normalize(sum);
                if (dot(nn, nn) > 0.5f) cornerN[i] = nn;
            }
        }
        for (size_t i = 1; i + 1 < n; ++i) {
            out.push_back({m.verts[face[0]], cornerN[0]});
            out.push_back({m.verts[face[i]], cornerN[i]});
            out.push_back({m.verts[face[i + 1]], cornerN[i + 1]});
        }
    }
}
