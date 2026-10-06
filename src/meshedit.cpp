#include "meshedit.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace meshedit {
namespace {

inline uint64_t directedKey(int a, int b) { return (uint64_t(uint32_t(a)) << 32) | uint32_t(b); }

// Linear map from positions in a face's plane to its UVs, from the face's
// first corner and the two edges spanning the largest area.
struct UvMap {
    bool ok = false;
    Vec3 du, dv;  // uv change per unit of world offset (gradients)
    Vec2 apply(Vec2 uv, Vec3 offset) const { return ok ? uv + Vec2(dot(du, offset), dot(dv, offset)) : uv; }
};
UvMap uvMapFor(const Mesh& m, int f) {
    UvMap map;
    if (!m.hasUVs()) return map;
    const auto& face = m.faces[f];
    const size_t n = face.size();
    const Vec3 p0 = m.verts[face[0]];
    const Vec2 t0 = m.uvs[f][0];
    float best = 0;
    for (size_t i = 1; i + 1 < n; ++i) {
        Vec3 e1 = m.verts[face[i]] - p0, e2 = m.verts[face[i + 1]] - p0;
        Vec2 s1 = m.uvs[f][i] - t0, s2 = m.uvs[f][i + 1] - t0;
        float a = length(cross(e1, e2));
        if (a <= best) continue;
        // Solve [e1 e2] (in the plane) for the gradients of u and v:
        // grad . e1 = s1, grad . e2 = s2, grad in span(e1, e2).
        float g11 = dot(e1, e1), g12 = dot(e1, e2), g22 = dot(e2, e2);
        float det = g11 * g22 - g12 * g12;
        if (std::fabs(det) < 1e-20f) continue;
        auto grad = [&](float a1, float a2) {
            float x = (a1 * g22 - a2 * g12) / det, y = (a2 * g11 - a1 * g12) / det;
            return e1 * x + e2 * y;
        };
        map.du = grad(s1.x, s2.x);
        map.dv = grad(s1.y, s2.y);
        map.ok = true;
        best = a;
    }
    return map;
}

}  // namespace

Edge makeEdge(int a, int b) { return a < b ? Edge(a, b) : Edge(b, a); }

CornerFans cornerFans(const Mesh& m, const std::vector<int>& facesIn) {
    CornerFans cf;
    std::unordered_set<int> seen;
    for (int f : facesIn)
        if (f >= 0 && f < (int)m.faces.size() && m.faces[f].size() >= 3 && seen.insert(f).second) cf.faces.push_back(f);
    int total = 0;
    for (int f : cf.faces) {
        cf.faceStart.push_back(total);
        total += (int)m.faces[f].size();
    }
    std::vector<int> parent(total);
    for (int i = 0; i < total; ++i) parent[i] = i;
    auto find = [&](int x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    };
    // Directed edge a -> b of a selected face -> (list index, corner of a).
    std::unordered_map<uint64_t, std::pair<int, int>> half;
    half.reserve((size_t)total * 2);
    for (int li = 0; li < (int)cf.faces.size(); ++li) {
        const auto& fv = m.faces[cf.faces[li]];
        for (size_t i = 0; i < fv.size(); ++i) half[directedKey(fv[i], fv[(i + 1) % fv.size()])] = {li, (int)i};
    }
    for (int li = 0; li < (int)cf.faces.size(); ++li) {
        const auto& fv = m.faces[cf.faces[li]];
        const int n = (int)fv.size();
        for (int i = 0; i < n; ++i) {
            auto it = half.find(directedKey(fv[(i + 1) % n], fv[i]));  // twin b -> a in another selected face
            if (it == half.end() || it->second.first == li) continue;
            const int tl = it->second.first, tc = it->second.second, tn = (int)m.faces[cf.faces[tl]].size();
            // a: corner i here, corner tc+1 there; b: corner i+1 here, corner tc there.
            parent[find(cf.faceStart[li] + i)] = find(cf.faceStart[tl] + (tc + 1) % tn);
            parent[find(cf.faceStart[li] + (i + 1) % n)] = find(cf.faceStart[tl] + tc);
        }
    }
    cf.fan.assign(total, -1);
    std::unordered_map<int, int> ids;
    for (int li = 0; li < (int)cf.faces.size(); ++li) {
        const auto& fv = m.faces[cf.faces[li]];
        for (int i = 0; i < (int)fv.size(); ++i) {
            const int root = find(cf.faceStart[li] + i);
            auto it = ids.find(root);
            if (it == ids.end()) {
                it = ids.emplace(root, cf.count++).first;
                cf.fanVertex.push_back(fv[i]);
            }
            cf.fan[cf.faceStart[li] + i] = it->second;
        }
    }
    return cf;
}

std::vector<Edge> edgesFromVerts(const Mesh& m, const std::vector<char>& vsel) {
    std::vector<Edge> out;
    if (vsel.size() != m.verts.size()) return out;
    for (const Edge& e : uniqueEdges(m))
        if (vsel[e.first] && vsel[e.second]) out.push_back(e);
    return out;
}

std::vector<char> vertsFromEdges(const Mesh& m, const std::vector<Edge>& edges) {
    std::vector<char> v(m.verts.size(), 0);
    for (const Edge& e : edges)
        if (e.first < (int)v.size() && e.second < (int)v.size()) v[e.first] = v[e.second] = 1;
    return v;
}

std::vector<char> vertsFromFaces(const Mesh& m, const std::vector<char>& fsel) {
    std::vector<char> v(m.verts.size(), 0);
    for (size_t f = 0; f < m.faces.size() && f < fsel.size(); ++f)
        if (fsel[f])
            for (int i : m.faces[f]) v[i] = 1;
    return v;
}

std::vector<char> facesFromVerts(const Mesh& m, const std::vector<char>& vsel) {
    std::vector<char> f(m.faces.size(), 0);
    if (vsel.size() != m.verts.size()) return f;
    for (size_t i = 0; i < m.faces.size(); ++i) {
        bool all = !m.faces[i].empty();
        for (int v : m.faces[i]) all = all && vsel[v];
        f[i] = all;
    }
    return f;
}

bool extrudeEdges(Mesh& m, const std::vector<Edge>& edges, std::vector<Edge>& newEdges, std::vector<char>& vsel) {
    newEdges.clear();
    if (edges.empty()) return false;
    const bool uvs = m.hasUVs(), weights = m.hasWeights();
    // Owner face (and corner) of each directed edge: looked up per selected
    // edge in the vertex -> faces index (no hash map of every half-edge).
    const VertexFaces vf(m);
    std::unordered_map<int, int> dup;
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
    bool any = false;
    for (const Edge& e : edges) {
        int a = e.first, b = e.second;
        // Prefer the face that has the edge as a boundary (only one owner).
        std::pair<int, int> ab, ba;
        const bool hasAb = vf.directed(a, b, &ab), hasBa = vf.directed(b, a, &ba);
        if (!hasAb && !hasBa) continue;
        if (!hasAb) {
            std::swap(a, b);
            ab = ba;
        }
        // The face runs a -> b: the new quad runs b -> a -> a' -> b'.
        struct {
            int face, corner;
        } o{ab.first, ab.second};
        int a2 = copyOf(a), b2 = copyOf(b);
        m.faces.push_back({b, a, a2, b2});
        if (uvs) {
            const auto& fu = m.uvs[o.face];
            const size_t n = fu.size();
            Vec2 ua = fu[o.corner], ub = fu[(o.corner + 1) % n];
            Vec2 d = ub - ua;
            Vec2 off(d.y * 0.25f, -d.x * 0.25f);  // a strip of UV space beside the edge
            m.uvs.push_back({ub, ua, ua + off, ub + off});
        }
        newEdges.push_back(makeEdge(a2, b2));
        any = true;
    }
    if (!any) return false;
    vsel.assign(m.verts.size(), 0);
    for (const auto& kv : dup) vsel[kv.second] = 1;
    m.validate();
    m.touch();
    return true;
}

bool insetFaces(Mesh& m, const std::vector<int>& faces, const InsetParams& p, std::vector<int>& innerFaces,
                std::vector<char>& vsel) {
    innerFaces.clear();
    if (faces.empty()) return false;
    const bool uvs = m.hasUVs(), weights = m.hasWeights();

    // Regions: the whole selection, or one per face.
    std::vector<std::vector<int>> regions;
    if (p.individual)
        for (int f : faces) regions.push_back({f});
    else
        regions.push_back(faces);

    std::vector<std::vector<int>> newFaces;
    std::vector<std::vector<Vec2>> newUVs;
    std::vector<int> moved;  // inner vertices of all regions (selected afterwards)

    for (const auto& regionIn : regions) {
        // Per fan of corners (not per vertex): a selection touching itself at
        // one vertex gets an inner copy per side and stays manifold.
        const CornerFans cf = cornerFans(m, regionIn);
        const std::vector<int>& region = cf.faces;
        if (region.empty()) continue;
        // Directed edges of the region; boundary = reverse not in the region.
        std::unordered_set<uint64_t> directed;
        for (int f : region) {
            const auto& face = m.faces[f];
            for (size_t i = 0; i < face.size(); ++i) directed.insert(directedKey(face[i], face[(i + 1) % face.size()]));
        }
        // Per-fan region normal (area weighted) and border neighbours.
        std::vector<Vec3> fanN(cf.count);
        std::vector<int> nextOf(cf.count, -1), prevOf(cf.count, -1);  // along the border, in face winding order
        for (int li = 0; li < (int)region.size(); ++li) {
            const auto& face = m.faces[region[li]];
            const int n = (int)face.size();
            Vec3 nrm = faceNormalRaw(m, face);
            for (int i = 0; i < n; ++i) {
                fanN[cf.of(li, i)] += nrm;
                int va = face[i], vb = face[(i + 1) % n];
                if (!directed.count(directedKey(vb, va))) {
                    nextOf[cf.of(li, i)] = vb;
                    prevOf[cf.of(li, (i + 1) % n)] = va;
                }
            }
        }
        for (Vec3& nrm : fanN) nrm = normalize(nrm);

        // New position of every fan: border fans get an inset copy, moved
        // inwards by `thickness` (perpendicular to their border edges); all
        // move by `depth` along the region normal.
        std::vector<int> inner(cf.count, -1);
        std::unordered_map<int, Vec3> target;
        for (int fan = 0; fan < cf.count; ++fan) {
            const int v = cf.fanVertex[fan];
            const Vec3 nrm = fanN[fan];
            Vec3 pos = m.verts[v];
            if (nextOf[fan] >= 0 || prevOf[fan] >= 0) {
                // A fan normally has a border edge on each side; on broken
                // input it can have one only (then that edge sets the direction).
                Vec3 ein = prevOf[fan] >= 0 ? normalize(pos - m.verts[prevOf[fan]]) : Vec3();
                Vec3 eout = nextOf[fan] >= 0 ? normalize(m.verts[nextOf[fan]] - pos) : Vec3();
                if (prevOf[fan] < 0) ein = eout;
                if (nextOf[fan] < 0) eout = ein;
                Vec3 n1 = normalize(cross(nrm, ein)), n2 = normalize(cross(nrm, eout));  // inward in-plane normals
                Vec3 d = n1 + n2;
                float len = length(d);
                d = len > 1e-6f ? d / len : n1;
                float scale = 1.0f / std::max(0.25f, dot(d, n1));  // keep the ring width constant at corners
                pos += d * (p.thickness * scale);
                int copy = (int)m.verts.size();
                Vec3 orig = m.verts[v];
                m.verts.push_back(orig);
                if (weights) {
                    BoneWeights w = m.weights[v];
                    m.weights.push_back(w);
                }
                inner[fan] = copy;
                target[copy] = pos + nrm * p.depth;
            } else {
                target[v] = pos + nrm * p.depth;  // interior vertex: only the depth offset
            }
        }

        // Ring faces along the border, then re-point the region's faces to
        // the inset copies (with their UVs shifted by the same offset).
        for (int li = 0; li < (int)region.size(); ++li) {
            const int f = region[li];
            const auto face = m.faces[f];
            const size_t n = face.size();
            UvMap map = uvMapFor(m, f);
            std::vector<Vec2> innerUV;
            if (uvs) {
                innerUV = m.uvs[f];
                for (size_t i = 0; i < n; ++i) {
                    const int fan = cf.of(li, (int)i);
                    if (inner[fan] < 0) continue;
                    Vec3 off = target[inner[fan]] - m.verts[face[i]];
                    off -= fanN[fan] * dot(off, fanN[fan]);  // in-plane part only
                    innerUV[i] = map.apply(m.uvs[f][i], off);
                }
            }
            for (size_t i = 0; i < n; ++i) {
                int va = face[i], vb = face[(i + 1) % n];
                if (directed.count(directedKey(vb, va))) continue;  // interior edge: no ring face
                newFaces.push_back({va, vb, inner[cf.of(li, (int)((i + 1) % n))], inner[cf.of(li, (int)i)]});
                if (uvs) newUVs.push_back({m.uvs[f][i], m.uvs[f][(i + 1) % n], innerUV[(i + 1) % n], innerUV[i]});
            }
            for (size_t i = 0; i < n; ++i) {
                const int c = inner[cf.of(li, (int)i)];
                if (c >= 0) m.faces[f][i] = c;
            }
            if (uvs) m.uvs[f] = innerUV;
        }
        for (const auto& kv : target) {
            m.verts[kv.first] = kv.second;
            moved.push_back(kv.first);
        }
    }

    // Dish: poke a centre vertex into each inner face.
    std::vector<int> inner(faces.begin(), faces.end());
    if (p.dish != 0.0f) {
        std::vector<int> fan;
        for (int f : faces) {
            const auto face = m.faces[f];
            const size_t n = face.size();
            Vec3 c = faceCenter(m, face), nrm = normalize(faceNormalRaw(m, face));
            int centre = (int)m.verts.size();
            m.verts.push_back(c + nrm * p.dish);
            if (weights) {
                BoneWeights w = m.weights[face[0]];
                m.weights.push_back(w);
            }
            moved.push_back(centre);
            Vec2 cu;
            if (uvs) {
                for (const Vec2& u : m.uvs[f]) cu += u;
                cu = cu / (float)n;
            }
            std::vector<Vec2> fu = uvs ? m.uvs[f] : std::vector<Vec2>();
            for (size_t i = 0; i < n; ++i) {
                std::vector<int> tri = {face[i], face[(i + 1) % n], centre};
                if (i == 0) {
                    m.faces[f] = tri;
                    if (uvs) m.uvs[f] = {fu[0], fu[1 % n], cu};
                    fan.push_back(f);
                } else {
                    newFaces.push_back(tri);
                    if (uvs) newUVs.push_back({fu[i], fu[(i + 1) % n], cu});
                    fan.push_back((int)(m.faces.size() + newFaces.size() - 1));
                }
            }
        }
        inner = fan;
    }

    for (size_t i = 0; i < newFaces.size(); ++i) {
        m.faces.push_back(std::move(newFaces[i]));
        if (uvs) m.uvs.push_back(std::move(newUVs[i]));
    }
    innerFaces = inner;
    vsel.assign(m.verts.size(), 0);
    for (int f : innerFaces)
        for (int v : m.faces[f]) vsel[v] = 1;
    m.validate();
    m.touch();
    return true;
}

}  // namespace meshedit
