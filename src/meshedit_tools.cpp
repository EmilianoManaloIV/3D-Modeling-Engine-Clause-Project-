// Topology tools that add vertices and faces: loops / rings, edge
// subdivision, loop cut, connect, poke, bevel, bridge, fill, push-in /
// punch-through, merge and join. See meshedit.h for what each one does.
//
// Most tools work on a half-edge view of the polygon mesh (one half-edge per
// face corner, FoCG 5e sec. 12.1.3 "winged-edge / half-edge structures"),
// build their new faces into separate arrays and write them back at the end,
// so the input mesh stays readable (positions, UVs) while they run.
#include "csg.h"
#include "meshedit_internal.h"
#include "polygon.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace meshedit {
using namespace detail;

namespace {

int addVertex(Mesh& m, Vec3 p, const BoneWeights* w) {
    int n = (int)m.verts.size();
    const BoneWeights copy = w ? *w : BoneWeights();  // `w` may point into m.weights
    m.verts.push_back(p);
    if (!m.weights.empty()) m.weights.push_back(copy);
    return n;
}

// New vertex at lerp(a, b, t), weights blended.
int addLerpVertex(Mesh& m, int a, int b, float t) {
    Vec3 p = lerp(m.verts[a], m.verts[b], t);
    if (m.hasWeights()) {
        BoneWeights w = blendWeights(m.weights[a], m.weights[b], t);
        return addVertex(m, p, &w);
    }
    return addVertex(m, p, nullptr);
}

Vec2 lerp2(Vec2 a, Vec2 b, float t) { return a + (b - a) * t; }

void selectFaceVerts(const Mesh& m, const std::vector<int>& faces, std::vector<char>& vsel) {
    vsel.assign(m.verts.size(), 0);
    for (int f : faces)
        if (f >= 0 && f < (int)m.faces.size())
            for (int v : m.faces[f]) vsel[v] = 1;
}

// Drops corners repeating the previous one (cyclically) and faces left with
// fewer than 3 corners. Keeps UVs in step. Returns the number of faces removed.
int removeDegenerate(Mesh& m) {
    const bool uvs = m.hasUVs();
    size_t w = 0;
    int removed = 0;
    for (size_t f = 0; f < m.faces.size(); ++f) {
        std::vector<int> face;
        std::vector<Vec2> fu;
        const auto& in = m.faces[f];
        for (size_t i = 0; i < in.size(); ++i) {
            if (!face.empty() && face.back() == in[i]) continue;
            face.push_back(in[i]);
            if (uvs) fu.push_back(m.uvs[f][i]);
        }
        while (face.size() > 1 && face.front() == face.back()) {
            face.pop_back();
            if (uvs) fu.pop_back();
        }
        if (face.size() < 3) {
            ++removed;
            continue;
        }
        m.faces[w] = std::move(face);
        if (uvs) m.uvs[w] = std::move(fu);
        ++w;
    }
    m.faces.resize(w);
    if (uvs) m.uvs.resize(w);
    return removed;
}

// Chains directed edges (u -> w) into vertex sequences. Fails if a vertex
// starts or ends more than one edge.
struct Chain {
    std::vector<int> v;
    bool closed = false;
};
bool chainEdges(const std::vector<std::pair<int, int>>& directed, std::vector<Chain>& out) {
    std::unordered_map<int, int> next, prev;
    for (const auto& e : directed) {
        if (next.count(e.first) || prev.count(e.second)) return false;
        next[e.first] = e.second;
        prev[e.second] = e.first;
    }
    std::unordered_set<int> used;
    // Open chains first: they start at a vertex without a predecessor.
    for (const auto& e : directed) {
        int s = e.first;
        if (prev.count(s) || used.count(s)) continue;
        Chain c;
        for (int v = s;;) {
            c.v.push_back(v);
            used.insert(v);
            auto it = next.find(v);
            if (it == next.end()) break;
            v = it->second;
        }
        out.push_back(c);
    }
    for (const auto& e : directed) {
        int s = e.first;
        if (used.count(s)) continue;
        Chain c;
        c.closed = true;
        int v = s;
        do {
            c.v.push_back(v);
            used.insert(v);
            v = next[v];
        } while (v != s && c.v.size() <= directed.size());
        out.push_back(c);
    }
    return true;
}

float meshDiagonal(const Mesh& m) {
    Vec3 lo, hi;
    return m.bounds(lo, hi) ? length(hi - lo) : 1.0f;
}

}  // namespace

// ============================================================================
// Selection helpers
// ============================================================================
std::vector<Edge> edgeRing(const Mesh& m, Edge seed) {
    std::vector<Edge> out;
    HalfEdges he(m);
    int h0 = he.find(seed.first, seed.second);
    if (h0 < 0) h0 = he.find(seed.second, seed.first);
    if (h0 < 0) return out;
    std::unordered_set<uint64_t> seen{edgeKey(seed)};
    out.push_back(makeEdge(seed.first, seed.second));
    for (int dir = 0; dir < 2; ++dir) {
        int cur = dir == 0 ? h0 : he.twin(h0);
        while (cur >= 0 && he.size(cur) == 4) {
            int o = he.start[he.face(cur)] + (he.corner(cur) + 2) % 4;
            uint64_t k = edgeKey(he.from(o), he.to(o));
            if (!seen.insert(k).second) break;  // closed ring
            out.push_back(makeEdge(he.from(o), he.to(o)));
            cur = he.twin(o);
        }
    }
    return out;
}

std::vector<Edge> edgeLoop(const Mesh& m, Edge seed) {
    std::vector<Edge> out;
    HalfEdges he(m);
    std::unordered_set<uint64_t> seen{edgeKey(seed)};
    out.push_back(makeEdge(seed.first, seed.second));
    // Next half-edge of the loop after `h` (which ends at the vertex to cross).
    auto step = [&](int h) -> int {
        if (he.twin(h) < 0) {  // border: the next border half-edge leaving to(h)
            int n = he.next(h);
            for (int guard = 0; guard < 64; ++guard) {
                int t = he.twin(n);
                if (t < 0) return n;
                n = he.next(t);
            }
            return -1;
        }
        // Interior: four quads around the vertex; take the edge opposite h.
        int n = he.next(h);
        if (he.size(h) != 4) return -1;
        int t1 = he.twin(n);
        if (t1 < 0 || he.size(t1) != 4) return -1;
        int candidate = he.next(t1);
        int t2 = he.twin(candidate);
        if (t2 < 0 || he.size(t2) != 4) return -1;
        int n3 = he.next(t2), t3 = he.twin(n3);
        if (t3 < 0 || he.size(t3) != 4) return -1;
        if (he.next(t3) != he.twin(h)) return -1;  // valence != 4
        return candidate;
    };
    for (int dir = 0; dir < 2; ++dir) {
        int a = dir == 0 ? seed.first : seed.second, b = dir == 0 ? seed.second : seed.first;
        int h = he.find(a, b);
        if (h < 0) {
            // Only the opposite half-edge exists: this is a border edge whose
            // face runs b -> a, so walk the border backwards from a.
            int g = he.find(b, a);
            if (g < 0) break;
            // Walk: previous border half-edge entering `from(g)`.
            int cur = g;
            for (size_t guard = 0; guard < m.faces.size() * 4 + 4; ++guard) {
                int p = he.prev(cur);
                int ok = -1;
                for (int k = 0; k < 64; ++k) {
                    int t = he.twin(p);
                    if (t < 0) {
                        ok = p;
                        break;
                    }
                    p = he.prev(t);
                }
                if (ok < 0) break;
                if (!seen.insert(edgeKey(he.from(ok), he.to(ok))).second) break;
                out.push_back(makeEdge(he.from(ok), he.to(ok)));
                cur = ok;
            }
            continue;
        }
        int cur = h;
        for (size_t guard = 0; guard < m.faces.size() * 4 + 4; ++guard) {
            int n = step(cur);
            if (n < 0) break;
            if (!seen.insert(edgeKey(he.from(n), he.to(n))).second) break;
            out.push_back(makeEdge(he.from(n), he.to(n)));
            cur = n;
        }
    }
    return out;
}

std::vector<int> deleteFaces(Mesh& m, const std::vector<char>& kill) {
    const bool uvs = m.hasUVs();
    size_t w = 0;
    for (size_t f = 0; f < m.faces.size(); ++f) {
        if (f < kill.size() && kill[f]) continue;
        if (w != f) {
            m.faces[w] = std::move(m.faces[f]);
            if (uvs) m.uvs[w] = std::move(m.uvs[f]);
        }
        ++w;
    }
    m.faces.resize(w);
    if (uvs) m.uvs.resize(w);
    std::vector<int> remap = removeUnusedVertices(m);
    m.validate();
    m.touch();
    return remap;
}

// ============================================================================
// Edge subdivision
// ============================================================================
bool subdivideEdges(Mesh& m, const std::vector<Edge>& edges, int cuts, std::vector<Edge>& newEdges,
                    std::vector<char>& vsel) {
    newEdges.clear();
    cuts = std::max(1, std::min(cuts, 64));
    if (edges.empty()) return false;
    const bool uvs = m.hasUVs();
    // New vertices per edge, ordered from the lower vertex index to the higher.
    std::unordered_map<uint64_t, std::vector<int>> inserted;
    std::vector<char> isNew;
    for (const Edge& e0 : edges) {
        Edge e = makeEdge(e0.first, e0.second);
        if (e.first == e.second || e.second >= (int)m.verts.size() || inserted.count(edgeKey(e))) continue;
        std::vector<int>& pts = inserted[edgeKey(e)];
        int prev = e.first;
        for (int k = 1; k <= cuts; ++k) {
            int v = addLerpVertex(m, e.first, e.second, (float)k / (cuts + 1));
            pts.push_back(v);
            newEdges.push_back(makeEdge(prev, v));
            prev = v;
        }
        newEdges.push_back(makeEdge(prev, e.second));
    }
    if (inserted.empty()) return false;
    bool any = false;
    for (size_t f = 0; f < m.faces.size(); ++f) {
        const auto& face = m.faces[f];
        const size_t n = face.size();
        std::vector<int> out;
        std::vector<Vec2> ou;
        bool changed = false;
        for (size_t i = 0; i < n; ++i) {
            int a = face[i], b = face[(i + 1) % n];
            out.push_back(a);
            if (uvs) ou.push_back(m.uvs[f][i]);
            auto it = inserted.find(edgeKey(a, b));
            if (it == inserted.end()) continue;
            changed = true;
            const auto& pts = it->second;
            for (int k = 0; k < cuts; ++k) {
                int idx = a < b ? k : cuts - 1 - k;
                out.push_back(pts[idx]);
                if (uvs) ou.push_back(lerp2(m.uvs[f][i], m.uvs[f][(i + 1) % n], (float)(k + 1) / (cuts + 1)));
            }
        }
        if (!changed) continue;
        any = true;
        m.faces[f] = std::move(out);
        if (uvs) m.uvs[f] = std::move(ou);
    }
    vsel.assign(m.verts.size(), 0);
    for (const auto& kv : inserted)
        for (int v : kv.second) vsel[v] = 1;
    m.validate();
    m.touch();
    return any || !inserted.empty();
}

// ============================================================================
// Loop cut
// ============================================================================
bool loopCut(Mesh& m, const std::vector<Edge>& seeds, const LoopCutParams& p, std::vector<Edge>& newEdges,
             std::vector<char>& vsel) {
    newEdges.clear();
    const int cuts = std::max(1, std::min(p.cuts, 64));
    if (seeds.empty()) return false;
    const bool uvs = m.hasUVs();
    std::unordered_map<uint64_t, int> sideA;  // ring edge -> its "A side" vertex (consistent along the ring)
    struct RingQuad {
        int face, corner;  // corner = start of the first ring edge
    };
    std::vector<RingQuad> quads;
    std::unordered_set<int> quadFaces;
    {
        HalfEdges he(m);
        for (const Edge& s : seeds) {
            int h0 = he.find(s.first, s.second);
            if (h0 < 0) h0 = he.find(s.second, s.first);
            if (h0 < 0 || sideA.count(edgeKey(s))) continue;
            sideA[edgeKey(s)] = he.from(h0);
            for (int dir = 0; dir < 2; ++dir) {
                // dir 0: the A side of `cur` is from(cur); dir 1: to(cur).
                int cur = dir == 0 ? h0 : he.twin(h0);
                while (cur >= 0 && he.size(cur) == 4 && !quadFaces.count(he.face(cur))) {
                    const int f = he.face(cur), c = he.corner(cur);
                    quadFaces.insert(f);
                    quads.push_back({f, c});
                    int o = he.start[f] + (c + 2) % 4;
                    uint64_t k = edgeKey(he.from(o), he.to(o));
                    int a = dir == 0 ? he.to(o) : he.from(o);
                    if (sideA.count(k)) break;  // closed ring
                    sideA[k] = a;
                    cur = he.twin(o);
                }
            }
        }
    }
    if (quads.empty()) return false;

    // New vertices on every ring edge, ordered from its A side.
    float tPos[64];
    for (int k = 0; k < cuts; ++k) tPos[k] = (float)(k + 1) / (cuts + 1);
    if (cuts == 1) tPos[0] = 0.5f + 0.49f * clampf(p.slide, -1.0f, 1.0f);
    std::unordered_map<uint64_t, std::vector<int>> pts;
    for (const auto& kv : sideA) {
        int a = (int)(kv.first >> 32), b = (int)(kv.first & 0xFFFFFFFFu);
        int from = kv.second, to = from == a ? b : a;
        std::vector<int>& list = pts[kv.first];
        for (int k = 0; k < cuts; ++k) list.push_back(addLerpVertex(m, from, to, tPos[k]));
    }
    // Points on edge (x -> y) ordered from x, with their parameter from x.
    auto along = [&](int x, int y, std::vector<int>& out, std::vector<float>& ts) {
        out.clear();
        ts.clear();
        uint64_t k = edgeKey(x, y);
        const auto& list = pts[k];
        const bool forward = sideA[k] == x;
        for (int i = 0; i < cuts; ++i) {
            int idx = forward ? i : cuts - 1 - i;
            out.push_back(list[idx]);
            ts.push_back(forward ? tPos[idx] : 1.0f - tPos[idx]);
        }
    };

    std::vector<std::vector<int>> addFaces;
    std::vector<std::vector<Vec2>> addUVs;
    std::vector<int> P, Q;
    std::vector<float> tp, tq;
    for (const RingQuad& rq : quads) {
        const std::vector<int> face = m.faces[rq.face];
        const std::vector<Vec2> fu = uvs ? m.uvs[rq.face] : std::vector<Vec2>();
        const int c = rq.corner;
        const int q0 = face[c], q1 = face[(c + 1) % 4], q2 = face[(c + 2) % 4], q3 = face[(c + 3) % 4];
        along(q0, q1, P, tp);
        along(q3, q2, Q, tq);
        std::vector<int> rowP{q0}, rowQ{q3};
        std::vector<Vec2> uvP, uvQ;
        if (uvs) {
            uvP.push_back(fu[c]);
            uvQ.push_back(fu[(c + 3) % 4]);
        }
        for (int k = 0; k < cuts; ++k) {
            rowP.push_back(P[k]);
            rowQ.push_back(Q[k]);
            if (uvs) {
                uvP.push_back(lerp2(fu[c], fu[(c + 1) % 4], tp[k]));
                uvQ.push_back(lerp2(fu[(c + 3) % 4], fu[(c + 2) % 4], tq[k]));
            }
            newEdges.push_back(makeEdge(P[k], Q[k]));
        }
        rowP.push_back(q1);
        rowQ.push_back(q2);
        if (uvs) {
            uvP.push_back(fu[(c + 1) % 4]);
            uvQ.push_back(fu[(c + 2) % 4]);
        }
        for (int k = 0; k <= cuts; ++k) {
            std::vector<int> nf{rowP[k], rowP[k + 1], rowQ[k + 1], rowQ[k]};
            std::vector<Vec2> nu;
            if (uvs) nu = {uvP[k], uvP[k + 1], uvQ[k + 1], uvQ[k]};
            if (k == 0) {
                m.faces[rq.face] = nf;
                if (uvs) m.uvs[rq.face] = nu;
            } else {
                addFaces.push_back(nf);
                if (uvs) addUVs.push_back(nu);
            }
        }
    }
    // Faces outside the ring that share a ring edge get the new vertices too.
    for (size_t f = 0; f < m.faces.size(); ++f) {
        if (quadFaces.count((int)f)) continue;
        const auto& face = m.faces[f];
        const size_t n = face.size();
        bool touches = false;
        for (size_t i = 0; i < n && !touches; ++i) touches = pts.count(edgeKey(face[i], face[(i + 1) % n])) > 0;
        if (!touches) continue;
        std::vector<int> out;
        std::vector<Vec2> ou;
        for (size_t i = 0; i < n; ++i) {
            int a = face[i], b = face[(i + 1) % n];
            out.push_back(a);
            if (uvs) ou.push_back(m.uvs[f][i]);
            if (!pts.count(edgeKey(a, b))) continue;
            along(a, b, P, tp);
            for (int k = 0; k < cuts; ++k) {
                out.push_back(P[k]);
                if (uvs) ou.push_back(lerp2(m.uvs[f][i], m.uvs[f][(i + 1) % n], tp[k]));
            }
        }
        m.faces[f] = std::move(out);
        if (uvs) m.uvs[f] = std::move(ou);
    }
    for (size_t i = 0; i < addFaces.size(); ++i) {
        m.faces.push_back(std::move(addFaces[i]));
        if (uvs) m.uvs.push_back(std::move(addUVs[i]));
    }
    std::sort(newEdges.begin(), newEdges.end());
    newEdges.erase(std::unique(newEdges.begin(), newEdges.end()), newEdges.end());
    vsel.assign(m.verts.size(), 0);
    for (const auto& kv : pts)
        for (int v : kv.second) vsel[v] = 1;
    m.validate();
    m.touch();
    return true;
}

// ============================================================================
// Connect vertices
// ============================================================================
int connectVertices(Mesh& m, const std::vector<char>& vsel, std::vector<Edge>& newEdges) {
    newEdges.clear();
    if (vsel.size() != m.verts.size()) return 0;
    const bool uvs = m.hasUVs();
    int split = 0;
    const size_t count = m.faces.size();
    for (size_t f = 0; f < count; ++f) {
        const std::vector<int> face = m.faces[f];
        const size_t n = face.size();
        int s[2], found = 0;
        for (size_t i = 0; i < n; ++i)
            if (vsel[face[i]]) {
                if (found < 2) s[found] = (int)i;
                ++found;
            }
        if (found != 2) continue;
        int i = s[0], j = s[1];
        if (j - i == 1 || (i == 0 && j == (int)n - 1)) continue;  // already an edge
        std::vector<int> a, b;
        std::vector<Vec2> ua, ub;
        for (int k = i; k <= j; ++k) {
            a.push_back(face[k]);
            if (uvs) ua.push_back(m.uvs[f][k]);
        }
        for (int k = j; k != i; k = (k + 1) % (int)n) {
            b.push_back(face[k]);
            if (uvs) ub.push_back(m.uvs[f][k]);
        }
        b.push_back(face[i]);
        if (uvs) ub.push_back(m.uvs[f][i]);
        m.faces[f] = a;
        if (uvs) m.uvs[f] = ua;
        m.faces.push_back(b);
        if (uvs) m.uvs.push_back(ub);
        newEdges.push_back(makeEdge(face[i], face[j]));
        ++split;
    }
    if (split) {
        m.validate();
        m.touch();
    }
    return split;
}

// ============================================================================
// Poke
// ============================================================================
int pokeFaces(Mesh& m, const std::vector<int>& faces, float offset, std::vector<int>& newFaces,
              std::vector<char>& vsel) {
    newFaces.clear();
    const bool uvs = m.hasUVs();
    std::vector<int> centres;
    for (int f : faces) {
        if (f < 0 || f >= (int)m.faces.size()) continue;
        const std::vector<int> face = m.faces[f];
        const size_t n = face.size();
        Vec3 c = faceCenter(m, face), nrm = normalize(faceNormalRaw(m, face));
        BoneWeights w;
        if (m.hasWeights()) {
            for (int v : face)
                for (int k = 0; k < 4; ++k)
                    if (m.weights[v].bone[k] >= 0) w.add(m.weights[v].bone[k], m.weights[v].w[k] / n);
            w.normalize();
        }
        int centre = addVertex(m, c + nrm * offset, &w);
        centres.push_back(centre);
        std::vector<Vec2> fu = uvs ? m.uvs[f] : std::vector<Vec2>();
        Vec2 cu;
        for (const Vec2& u : fu) cu += u;
        if (uvs) cu = cu / (float)n;
        for (size_t i = 0; i < n; ++i) {
            std::vector<int> tri{face[i], face[(i + 1) % n], centre};
            std::vector<Vec2> tu;
            if (uvs) tu = {fu[i], fu[(i + 1) % n], cu};
            if (i == 0) {
                m.faces[f] = tri;
                if (uvs) m.uvs[f] = tu;
                newFaces.push_back(f);
            } else {
                m.faces.push_back(tri);
                if (uvs) m.uvs.push_back(tu);
                newFaces.push_back((int)m.faces.size() - 1);
            }
        }
    }
    if (centres.empty()) return 0;
    selectFaceVerts(m, newFaces, vsel);
    m.validate();
    m.touch();
    return (int)centres.size();
}

// ============================================================================
// Bevel
// ============================================================================
bool bevel(Mesh& m, const std::vector<Edge>& edgesIn, const std::vector<char>& verts, const BevelParams& p,
           std::vector<int>& newFaces, std::vector<char>& vsel, float* usedWidth) {
    newFaces.clear();
    if (usedWidth) *usedWidth = 0;
    const int segments = std::max(1, std::min(p.segments, 32));
    const bool uvs = m.hasUVs();
    const int origVerts = (int)m.verts.size();
    HalfEdges he(m);

    // Beveled edges: manifold edges only (exactly two opposite half-edges).
    std::unordered_set<uint64_t> E;
    std::vector<char> BV(m.verts.size(), 0);
    if (p.vertexOnly) {
        for (size_t v = 0; v < verts.size() && v < BV.size(); ++v) BV[v] = verts[v];
    } else {
        for (const Edge& e : edgesIn) {
            if (e.first < 0 || e.second < 0 || e.first >= origVerts || e.second >= origVerts) continue;
            int h1 = he.find(e.first, e.second), h2 = he.find(e.second, e.first);
            if (h1 < 0 || h2 < 0 || he.face(h1) == he.face(h2)) continue;
            E.insert(edgeKey(e));
            BV[e.first] = BV[e.second] = 1;
        }
    }
    auto beveled = [&](int a, int b) { return E.count(edgeKey(a, b)) > 0; };
    // Open edges of the input are never patched. (The half-edge table only
    // reads its key map here, so it stays valid after faces are rebuilt.)
    auto isOrigBorder = [&](uint64_t e) {
        int a = (int)(e >> 32), b = (int)(e & 0xFFFFFFFFu);
        return a < origVerts && b < origVerts && (he.find(a, b) < 0) != (he.find(b, a) < 0);
    };
    bool anyVertex = false;
    for (char c : BV) anyVertex |= c != 0;
    if (!anyVertex) return false;

    // Clamp the width so that no edge around a beveled vertex collapses.
    // Offsets come from both ends of an edge (and from both sides of a
    // face), so half of the shortest edge of any face involved is the limit.
    float minLen = 1e30f;
    for (size_t f = 0; f < m.faces.size(); ++f) {
        const auto& face = m.faces[f];
        bool touches = false;
        for (int v : face) touches |= BV[v] != 0;
        if (!touches) continue;
        for (size_t i = 0; i < face.size(); ++i)
            minLen = std::min(minLen, 0.5f * length(m.verts[face[i]] - m.verts[face[(i + 1) % face.size()]]));
    }
    const float w = std::max(0.0f, std::min(p.width, minLen * 0.98f));
    if (usedWidth) *usedWidth = w;
    if (w <= 1e-7f) return false;

    // One new vertex per non-beveled edge leaving a beveled vertex: P(v, x).
    // (Flat tables indexed by vertex: these run for every corner of every
    // face around the bevel, so hash-node maps would dominate the cost.)
    FlatMap edgePoint;                   // directed (v, x) -> vertex
    edgePoint.reserve(E.size() * 8 + (p.vertexOnly ? verts.size() : 0) + 16);
    std::vector<uint64_t> pointEdge;     // vertex - origVerts -> its edge (undirected), ~0 = not a P vertex
    auto pointEdgeOf = [&](int v) {
        size_t k = (size_t)(v - origVerts);
        return v >= origVerts && k < pointEdge.size() ? pointEdge[k] : ~0ull;
    };
    auto P = [&](int v, int x) {
        uint64_t k = directedKey(v, x);
        int found = edgePoint.find(k);
        if (found >= 0) return found;
        Vec3 d = m.verts[x] - m.verts[v];
        float len = length(d);
        int id = addVertex(m, m.verts[v] + (len > 1e-12f ? d * (w / len) : Vec3()),
                           m.hasWeights() ? &m.weights[v] : nullptr);
        edgePoint.emplace(k, id);
        pointEdge.resize(id - origVerts + 1, ~0ull);
        pointEdge[id - origVerts] = edgeKey(v, x);
        return id;
    };

    // Rebuild every face touching a beveled vertex.
    std::vector<std::vector<int>> outFaces(m.faces.size());
    std::vector<std::vector<Vec2>> outUVs(uvs ? m.faces.size() : 0);
    std::vector<int> cornerVert(he.count(), -1);  // half-edge (corner) -> replacing vertex
    std::vector<Vec2> cornerUV(uvs ? he.count() : 0);
    std::vector<Vec2> repUV;                         // a UV for each vertex (patch faces use them)
    auto setRep = [&](int v, Vec2 uv) {
        if ((size_t)v >= repUV.size()) repUV.resize(v + 1 + v / 4);
        repUV[v] = uv;
    };
    std::vector<int> neitherCount(origVerts, 0);    // beveled vertex -> faces keeping it
    std::vector<int> keptBy(origVerts, -1);         // ... and (the last) one of those faces
    for (size_t f = 0; f < m.faces.size(); ++f) {
        bool touches = false;
        for (int v : m.faces[f]) touches |= BV[v] != 0;
        if (!touches) continue;
        const std::vector<int> face = m.faces[f];
        const size_t n = face.size();
        UvMap map = uvs ? uvMapFor(m, (int)f) : UvMap();
        const Vec3 N = normalize(faceNormalRaw(m, face));
        std::vector<int>& out = outFaces[f];
        std::vector<Vec2>* ou = uvs ? &outUVs[f] : nullptr;
        out.reserve(n * 2);
        if (ou) ou->reserve(n * 2);
        auto emit = [&](int v, Vec2 uv) {
            out.push_back(v);
            if (ou) ou->push_back(uv);
            if (uvs) setRep(v, uv);
        };
        for (size_t i = 0; i < n; ++i) {
            const int v = face[i], pv = face[(i + n - 1) % n], nv = face[(i + 1) % n];
            const Vec2 uvV = uvs ? m.uvs[f][i] : Vec2();
            const Vec2 uvP = uvs ? m.uvs[f][(i + n - 1) % n] : Vec2();
            const Vec2 uvN = uvs ? m.uvs[f][(i + 1) % n] : Vec2();
            if (!BV[v]) {
                emit(v, uvV);
                continue;
            }
            const int h = he.start[f] + (int)i;
            const bool inB = beveled(pv, v), outB = beveled(v, nv);
            auto edgeUV = [&](int x, Vec2 uvX) {
                float len = length(m.verts[x] - m.verts[v]);
                return lerp2(uvV, uvX, len > 1e-12f ? std::min(1.0f, w / len) : 0.0f);
            };
            if (inB && outB) {
                Vec3 din = normalize(m.verts[pv] - m.verts[v]), dout = normalize(m.verts[nv] - m.verts[v]);
                float s = length(cross(din, dout));
                Vec3 off = s > 0.05f ? (din + dout) * (w / s) : normalize(cross(N, dout)) * w;
                int c = addVertex(m, m.verts[v] + off, m.hasWeights() ? &m.weights[v] : nullptr);
                Vec2 cu = map.apply(uvV, off);
                cornerVert[h] = c;
                if (uvs) cornerUV[h] = cu;
                emit(c, cu);
            } else if (outB) {
                int c = P(v, pv);
                Vec2 cu = edgeUV(pv, uvP);
                cornerVert[h] = c;
                if (uvs) cornerUV[h] = cu;
                emit(c, cu);
            } else if (inB) {
                int c = P(v, nv);
                Vec2 cu = edgeUV(nv, uvN);
                cornerVert[h] = c;
                if (uvs) cornerUV[h] = cu;
                emit(c, cu);
            } else {
                emit(P(v, pv), edgeUV(pv, uvP));
                if (!p.vertexOnly) {
                    emit(v, uvV);
                    neitherCount[v]++;
                    keptBy[v] = (int)f;
                }
                emit(P(v, nv), edgeUV(nv, uvN));
            }
        }
    }

    // Strips along beveled edges (rounded with `segments`).
    std::vector<std::vector<int>> addFaces;
    std::vector<std::vector<Vec2>> addUVs;
    for (uint64_t k : E) {
        int a = (int)(k >> 32), b = (int)(k & 0xFFFFFFFFu);
        int h1 = he.find(a, b), h2 = he.find(b, a);
        const int A1 = cornerVert[h1], B1 = cornerVert[he.next(h1)];
        const int B2 = cornerVert[h2], A2 = cornerVert[he.next(h2)];
        if (A1 < 0 || B1 < 0 || A2 < 0 || B2 < 0) continue;
        // Profile rows across the strip at each end: quadratic Bezier through
        // the original vertex as control point.
        std::vector<int> rowA{A1}, rowB{B1};
        std::vector<Vec2> uA, uB;
        if (uvs) {
            uA.push_back(cornerUV[h1]);
            uB.push_back(cornerUV[he.next(h1)]);
        }
        for (int s = 1; s < segments; ++s) {
            float t = (float)s / segments;
            auto bez = [&](int x0, int ctrl, int x1) {
                Vec3 p0 = m.verts[x0], c = m.verts[ctrl], p1 = m.verts[x1];
                return p0 * ((1 - t) * (1 - t)) + c * (2 * (1 - t) * t) + p1 * (t * t);
            };
            rowA.push_back(addVertex(m, bez(A1, a, A2), m.hasWeights() ? &m.weights[a] : nullptr));
            rowB.push_back(addVertex(m, bez(B1, b, B2), m.hasWeights() ? &m.weights[b] : nullptr));
            if (uvs) {
                uA.push_back(lerp2(cornerUV[h1], cornerUV[he.next(h2)], t));
                uB.push_back(lerp2(cornerUV[he.next(h1)], cornerUV[h2], t));
            }
        }
        rowA.push_back(A2);
        rowB.push_back(B2);
        if (uvs) {
            uA.push_back(cornerUV[he.next(h2)]);
            uB.push_back(cornerUV[h2]);
            for (size_t s = 0; s < rowA.size(); ++s) {
                setRep(rowA[s], uA[s]);
                setRep(rowB[s], uB[s]);
            }
        }
        for (int s = 0; s < segments; ++s) {
            addFaces.push_back({rowB[s], rowA[s], rowA[s + 1], rowB[s + 1]});
            if (uvs) addUVs.push_back({uB[s], uA[s], uA[s + 1], uB[s + 1]});
        }
    }

    // Write the rebuilt faces back.
    std::vector<char> touched(outFaces.size(), 0);
    for (size_t f = 0; f < outFaces.size(); ++f) {
        if (outFaces[f].empty()) continue;
        touched[f] = 1;
        m.faces[f] = std::move(outFaces[f]);
        if (uvs) m.uvs[f] = std::move(outUVs[f]);
    }
    const int firstNew = (int)m.faces.size();
    for (size_t i = 0; i < addFaces.size(); ++i) {
        m.faces.push_back(std::move(addFaces[i]));
        if (uvs) m.uvs.push_back(std::move(addUVs[i]));
    }

    // Corner patches: the holes left around each beveled vertex are the
    // loops of border edges between bevel vertices that were not borders of
    // the original mesh. Each loop becomes one face.
    auto bevelVertex = [&](int v) { return v >= origVerts || BV[v]; };
    auto onOrigBorder = [&](int u, int x) {
        auto onEdge = [&](int v, uint64_t e) {
            if (v == (int)(e >> 32) || v == (int)(e & 0xFFFFFFFFu)) return true;
            return pointEdgeOf(v) == e;
        };
        for (int v : {u, x}) {
            uint64_t pe = pointEdgeOf(v);
            if (pe != ~0ull && isOrigBorder(pe) && onEdge(u, pe) && onEdge(x, pe))
                return true;
        }
        return isOrigBorder(edgeKey(u, x));
    };
    {
        // Hole borders only run through rebuilt and new faces: collect their
        // directed edges (sorted, so twins are found by binary search).
        std::vector<uint64_t> directed;
        auto addFaceEdges = [&](size_t f) {
            const auto& face = m.faces[f];
            for (size_t i = 0; i < face.size(); ++i) directed.push_back(directedKey(face[i], face[(i + 1) % face.size()]));
        };
        for (size_t f = 0; f < touched.size(); ++f)
            if (touched[f]) addFaceEdges(f);
        for (size_t f = touched.size(); f < m.faces.size(); ++f) addFaceEdges(f);
        std::sort(directed.begin(), directed.end());
        directed.erase(std::unique(directed.begin(), directed.end()), directed.end());
        std::vector<std::pair<int, int>> holeEdges;
        for (uint64_t k : directed) {
            int u = (int)(k >> 32), x = (int)(k & 0xFFFFFFFFu);
            if (std::binary_search(directed.begin(), directed.end(), directedKey(x, u))) continue;
            if (!bevelVertex(u) || !bevelVertex(x) || onOrigBorder(u, x)) continue;
            holeEdges.push_back({x, u});  // the patch runs the other way
        }
        std::vector<Chain> chains;
        if (chainEdges(holeEdges, chains)) {
            for (const Chain& c : chains) {
                if (!c.closed || c.v.size() < 3) continue;
                // A patch with one kept original corner merges into the only
                // face that still uses it (the corner of that face is cut).
                if (c.v.size() == 3) {
                    int keep = -1;
                    for (int v : c.v)
                        if (v < origVerts && neitherCount[v] == 1) keep = v;
                    if (keep >= 0 && keptBy[keep] >= 0) {
                        const int kf = keptBy[keep];
                        auto& face = m.faces[kf];
                        auto it = std::find(face.begin(), face.end(), keep);
                        if (it != face.end()) {
                            size_t idx = it - face.begin();
                            face.erase(it);
                            if (uvs) m.uvs[kf].erase(m.uvs[kf].begin() + idx);
                            continue;
                        }
                    }
                }
                m.faces.push_back(c.v);
                if (uvs) {
                    std::vector<Vec2> pu;
                    for (int v : c.v) pu.push_back((size_t)v < repUV.size() ? repUV[v] : Vec2());
                    m.uvs.push_back(pu);
                }
            }
        }
    }
    for (int f = firstNew; f < (int)m.faces.size(); ++f) newFaces.push_back(f);

    removeDegenerate(m);
    std::vector<int> remap = removeUnusedVertices(m);
    (void)remap;
    m.validate();
    m.touch();
    // Face indices of the new faces are stable (degenerate removal only
    // drops collapsed faces, which bevel does not create in practice).
    std::vector<int> valid;
    for (int f : newFaces)
        if (f < (int)m.faces.size()) valid.push_back(f);
    newFaces = valid;
    selectFaceVerts(m, newFaces, vsel);
    return true;
}

// ============================================================================
// Bridge
// ============================================================================
namespace {

// Joins two loops x (first) and y (second), both given in the direction of
// the faces that own their edges. y is walked backwards so the new faces
// continue both surfaces with a consistent orientation.
void bridgeLoops(Mesh& m, std::vector<int> x, std::vector<int> y, bool closed, const BridgeParams& p,
                 std::vector<int>& newFaces) {
    const bool uvs = m.hasUVs();
    std::reverse(y.begin(), y.end());
    const int n1 = (int)x.size(), n2 = (int)y.size();
    if (closed) {
        // Best rotation of y: smallest summed distance to x.
        int best = 0;
        float bestD = 1e30f;
        for (int k = 0; k < n2; ++k) {
            float d = 0;
            for (int i = 0; i < n1; ++i) {
                int j = (int)std::lround((double)i * n2 / n1) + k;
                d += length(m.verts[x[i]] - m.verts[y[j % n2]]);
            }
            if (d < bestD) bestD = d, best = k;
        }
        best = ((best + p.twist) % n2 + n2) % n2;
        std::rotate(y.begin(), y.begin() + best, y.end());
    } else if (length(m.verts[x[0]] - m.verts[y[0]]) + length(m.verts[x[n1 - 1]] - m.verts[y[n2 - 1]]) >
               length(m.verts[x[0]] - m.verts[y[n2 - 1]]) + length(m.verts[x[n1 - 1]] - m.verts[y[0]])) {
        // Open chains run opposite ways already: keep y's original order.
        std::reverse(y.begin(), y.end());
    }
    auto addFace = [&](std::vector<int> f, std::vector<Vec2> u) {
        m.faces.push_back(std::move(f));
        if (uvs) m.uvs.push_back(std::move(u));
        newFaces.push_back((int)m.faces.size() - 1);
    };
    if (n1 == n2) {
        const int segs = std::max(1, std::min(p.segments, 64));
        std::vector<std::vector<int>> rings{x};
        for (int s = 1; s < segs; ++s) {
            std::vector<int> r;
            for (int i = 0; i < n1; ++i) r.push_back(addLerpVertex(m, x[i], y[i], (float)s / segs));
            rings.push_back(r);
        }
        rings.push_back(y);
        const int spans = closed ? n1 : n1 - 1;
        for (int s = 0; s < segs; ++s)
            for (int i = 0; i < spans; ++i) {
                int i1 = (i + 1) % n1;
                float u0 = (float)i / n1, u1 = (float)(i + 1) / n1, v0 = (float)s / segs, v1 = (float)(s + 1) / segs;
                addFace({rings[s][i1], rings[s][i], rings[s + 1][i], rings[s + 1][i1]},
                        {{u1, v0}, {u0, v0}, {u0, v1}, {u1, v1}});
            }
        return;
    }
    // Different sizes: walk both loops by their parameter, emitting a
    // triangle whenever only one side advances and a quad when both do.
    int i = 0, j = 0;
    const int e1 = closed ? n1 : n1 - 1, e2 = closed ? n2 : n2 - 1;
    while (i < e1 || j < e2) {
        float ti = i < e1 ? (float)(i + 1) / e1 : 2.0f, tj = j < e2 ? (float)(j + 1) / e2 : 2.0f;
        int xi = x[i % n1], xi1 = x[(i + 1) % n1], yj = y[j % n2], yj1 = y[(j + 1) % n2];
        Vec2 ux0((float)i / e1, 0), ux1(std::min(ti, 1.0f), 0), uy0((float)j / e2, 1), uy1(std::min(tj, 1.0f), 1);
        if (std::fabs(ti - tj) < 1e-4f) {
            addFace({xi1, xi, yj, yj1}, {ux1, ux0, uy0, uy1});
            ++i, ++j;
        } else if (ti < tj) {
            addFace({xi1, xi, yj}, {ux1, ux0, uy0});
            ++i;
        } else {
            addFace({xi, yj, yj1}, {ux0, uy0, uy1});
            ++j;
        }
    }
}

bool bridgeChains(Mesh& m, std::vector<Chain>& chains, const BridgeParams& p, std::vector<int>& newFaces,
                  std::string& error) {
    if (chains.size() != 2) {
        error = chains.size() < 2 ? "Bridge needs two separate edge loops (or two face regions)"
                                  : "Bridge works on exactly two edge loops; " + std::to_string(chains.size()) +
                                        " are selected";
        return false;
    }
    if (chains[0].closed != chains[1].closed) {
        error = "Bridge: one loop is closed and the other is open";
        return false;
    }
    for (int v : chains[0].v)
        if (std::find(chains[1].v.begin(), chains[1].v.end(), v) != chains[1].v.end()) {
            error = "Bridge: the two loops share a vertex";
            return false;
        }
    const size_t minSize = chains[0].closed ? 3 : 2;
    if (chains[0].v.size() < minSize || chains[1].v.size() < minSize) {
        error = "Bridge: each loop needs at least " + std::to_string(minSize) + " vertices";
        return false;
    }
    if (!m.hasUVs()) m.uvs.clear();
    bridgeLoops(m, chains[0].v, chains[1].v, chains[0].closed, p, newFaces);
    return true;
}

}  // namespace

bool bridgeEdges(Mesh& m, const std::vector<Edge>& edges, const BridgeParams& p, std::vector<int>& newFaces,
                 std::vector<char>& vsel, std::string& error) {
    newFaces.clear();
    std::vector<std::pair<int, int>> directed;
    {
        HalfEdges he(m);
        for (const Edge& e : edges) {
            int h1 = he.find(e.first, e.second), h2 = he.find(e.second, e.first);
            if (h1 >= 0 && h2 >= 0) {
                error = "Bridge needs border edges (edges with one face). Select faces to bridge closed surfaces";
                return false;
            }
            if (h1 >= 0) directed.push_back({e.first, e.second});
            else if (h2 >= 0) directed.push_back({e.second, e.first});
        }
    }
    std::vector<Chain> chains;
    if (directed.empty()) {
        error = "Select the border edges of two openings";
        return false;
    }
    if (!chainEdges(directed, chains)) {
        error = "Bridge: the selected edges branch";
        return false;
    }
    if (!bridgeChains(m, chains, p, newFaces, error)) return false;
    m.validate();
    m.touch();
    selectFaceVerts(m, newFaces, vsel);
    return true;
}

bool bridgeFaces(Mesh& m, const std::vector<int>& faces, const BridgeParams& p, std::vector<int>& newFaces,
                 std::vector<char>& vsel, std::string& error) {
    newFaces.clear();
    std::vector<char> inSel(m.faces.size(), 0);
    for (int f : faces)
        if (f >= 0 && f < (int)m.faces.size()) inSel[f] = 1;
    std::vector<std::pair<int, int>> directed;
    {
        HalfEdges he(m);
        // Regions = connected components of the selected faces.
        std::vector<int> comp(m.faces.size(), -1);
        int regions = 0;
        for (size_t f0 = 0; f0 < m.faces.size(); ++f0) {
            if (!inSel[f0] || comp[f0] >= 0) continue;
            std::vector<int> stack{(int)f0};
            comp[f0] = regions;
            while (!stack.empty()) {
                int f = stack.back();
                stack.pop_back();
                for (int h = he.start[f]; h < he.start[f + 1]; ++h) {
                    int t = he.twin(h);
                    if (t < 0) continue;
                    int g = he.face(t);
                    if (inSel[g] && comp[g] < 0) {
                        comp[g] = regions;
                        stack.push_back(g);
                    }
                }
            }
            ++regions;
        }
        if (regions != 2) {
            error = regions < 2 ? "Bridge: select two separate groups of faces"
                                : "Bridge: " + std::to_string(regions) + " separate face groups are selected; select two";
            return false;
        }
        // After deleting the regions, the hole borders run like the remaining
        // faces: the reverse of each region's border half-edges.
        for (int h = 0; h < he.count(); ++h) {
            if (!inSel[he.face(h)]) continue;
            int t = he.twin(h);
            if (t >= 0 && inSel[he.face(t)]) continue;
            directed.push_back({he.to(h), he.from(h)});
        }
    }
    std::vector<Chain> chains;
    if (!chainEdges(directed, chains)) {
        error = "Bridge: a selected region touches itself at a vertex";
        return false;
    }
    if (chains.size() != 2) {
        error = "Bridge: each face group must have one border loop (no holes inside it)";
        return false;
    }
    // Remove the faces without renumbering vertices yet.
    {
        const bool uvs = m.hasUVs();
        size_t w = 0;
        for (size_t f = 0; f < m.faces.size(); ++f) {
            if (inSel[f]) continue;
            if (w != f) {  // (self move-assignment would empty the vector)
                m.faces[w] = std::move(m.faces[f]);
                if (uvs) m.uvs[w] = std::move(m.uvs[f]);
            }
            ++w;
        }
        m.faces.resize(w);
        if (uvs) m.uvs.resize(w);
    }
    if (!bridgeChains(m, chains, p, newFaces, error)) return false;
    removeUnusedVertices(m);
    m.validate();
    m.touch();
    selectFaceVerts(m, newFaces, vsel);
    return true;
}

// ============================================================================
// Fill
// ============================================================================
bool fillFace(Mesh& m, const std::vector<Edge>& edges, const std::vector<char>& verts, std::vector<int>& newFaces,
              std::vector<char>& vsel, std::string& error) {
    newFaces.clear();
    std::vector<int> loop;
    std::vector<std::pair<int, int>> directed;
    {
        HalfEdges he(m);
        for (const Edge& e : edges) {
            int h1 = he.find(e.first, e.second), h2 = he.find(e.second, e.first);
            if (h1 >= 0 && h2 >= 0) continue;  // interior edges can't border a new face
            if (h1 >= 0) directed.push_back({e.second, e.first});
            else if (h2 >= 0) directed.push_back({e.first, e.second});
        }
    }
    std::vector<Chain> chains;
    if (!directed.empty() && chainEdges(directed, chains) && chains.size() == 1 && chains[0].closed &&
        chains[0].v.size() >= 3) {
        loop = chains[0].v;
    } else {
        // Free vertices: order them around their centre in their best plane.
        std::vector<int> vs;
        for (size_t v = 0; v < verts.size() && v < m.verts.size(); ++v)
            if (verts[v]) vs.push_back((int)v);
        if (vs.size() < 3) {
            error = "Fill: select a closed loop of border edges, or 3+ vertices";
            return false;
        }
        if (vs.size() > 64) {
            error = "Fill: too many loose vertices (select a border loop instead)";
            return false;
        }
        Vec3 c;
        for (int v : vs) c += m.verts[v];
        c = c / (float)vs.size();
        Vec3 n;  // Newell normal of the points in selection order is unreliable; use the covariance-free trick:
        for (size_t i = 0; i < vs.size(); ++i)
            for (size_t j = i + 1; j < vs.size(); ++j) {
                Vec3 cr = cross(m.verts[vs[i]] - c, m.verts[vs[j]] - c);
                if (dot(cr, n) < 0) cr = -cr;
                n += cr;
            }
        n = normalize(n);
        if (length(n) < 0.5f) {
            error = "Fill: the vertices are on a line";
            return false;
        }
        Vec3 u = normalize(m.verts[vs[0]] - c);
        if (length(u) < 0.5f) u = normalize(cross(n, std::fabs(n.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0)));
        Vec3 v2 = cross(n, u);
        std::sort(vs.begin(), vs.end(), [&](int a, int b) {
            Vec3 da = m.verts[a] - c, db = m.verts[b] - c;
            return std::atan2(dot(da, v2), dot(da, u)) < std::atan2(dot(db, v2), dot(db, u));
        });
        // Face the outside: away from the mesh centre (or towards +n if unknown).
        Vec3 lo, hi;
        Vec3 meshC = m.bounds(lo, hi) ? (lo + hi) * 0.5f : c - n;
        if (dot(n, c - meshC) < 0) std::reverse(vs.begin(), vs.end());
        loop = vs;
    }
    const bool uvs = m.hasUVs();
    if (!uvs) m.uvs.clear();
    m.faces.push_back(loop);
    if (uvs) {
        std::vector<Vec2> u;
        for (size_t i = 0; i < loop.size(); ++i) {
            float a = 2 * kPi * i / loop.size();
            u.push_back({0.5f + 0.5f * std::cos(a), 0.5f + 0.5f * std::sin(a)});
        }
        m.uvs.push_back(u);
    }
    newFaces.push_back((int)m.faces.size() - 1);
    m.validate();
    m.touch();
    selectFaceVerts(m, newFaces, vsel);
    return true;
}

// ============================================================================
// Push in / punch through
// ============================================================================
bool pushIn(Mesh& m, const std::vector<int>& faces, const PushParams& p, std::vector<int>& innerFaces,
            std::vector<char>& vsel, std::string& error) {
    innerFaces.clear();
    if (faces.empty()) {
        error = "Select faces to push in";
        return false;
    }
    // Region normal (area weighted). A closed or strongly curved selection
    // (e.g. every face of a cube) has none: push each face in on its own.
    Vec3 n, absSum;
    for (int f : faces) {
        Vec3 fn = faceNormalRaw(m, m.faces[f]);
        n += fn;
        absSum += Vec3(std::fabs(fn.x), std::fabs(fn.y), std::fabs(fn.z));
    }
    const bool individual = length(n) < 0.5f * length(absSum) * 0.5f;
    n = normalize(n);
    if (individual && p.through) {
        error = "Punch: the selected faces point in different directions - select faces on one side";
        return false;
    }
    std::vector<int> inner = faces;
    if (p.width > 0.0f) {
        InsetParams ip;
        ip.thickness = p.width;
        ip.individual = individual;
        if (!insetFaces(m, faces, ip, inner, vsel)) return false;
    }
    if (individual) {
        for (int f : inner) {
            Vec3 fn = normalize(faceNormalRaw(m, m.faces[f]));
            std::vector<char> one;
            Vec3 en;
            if (!extrudeFaces(m, {f}, one, &en)) continue;
            for (size_t v = 0; v < one.size(); ++v)
                if (one[v]) m.verts[v] -= fn * p.depth;
        }
        selectFaceVerts(m, inner, vsel);
        innerFaces = inner;
        m.touch();
        return true;
    }
    if (!p.through) {
        Vec3 en;
        if (!extrudeFaces(m, inner, vsel, &en)) return false;
        for (size_t v = 0; v < vsel.size(); ++v)
            if (vsel[v]) m.verts[v] -= n * p.depth;
        innerFaces = inner;
        m.touch();
        return true;
    }
    return punchThrough(m, inner, n, vsel, error);
}

// ============================================================================
// Punch through
// ============================================================================
// The hole is the prism swept by the region's outline along -n through the
// whole mesh.
//   - Light meshes get an exact Boolean difference with that prism (BSP CSG,
//     csg.h): the far side is cut precisely along the outline.
//   - Dense meshes (where BSP CSG is quadratic and takes minutes) snap the far
//     side to existing edges instead: faces whose centre projects inside the
//     outline are removed, and the entry outline is bridged to the hole that
//     leaves on the far side. On a dense surface the snapped outline is within
//     one edge length of the exact one, and nothing is ever sliced into
//     slivers, so the result stays watertight.
namespace {
bool pointInPolygon(Vec2 p, const std::vector<Vec2>& poly) {
    bool inside = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const Vec2 a = poly[i], b = poly[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) inside = !inside;
    }
    return inside;
}

// Every edge used once in each direction.
bool isWatertight(const Mesh& m) {
    HalfEdges he(m);
    for (int h = 0; h < he.count(); ++h)
        if (he.twin(h) < 0 || he.find(he.from(h), he.to(h)) != h) return false;
    return true;
}

bool punchCSG(Mesh& m, const std::vector<int>& inner, Vec3 n, std::string& error) {
    const float diag = meshDiagonal(m);
    const float up = 1e-3f * diag + 1e-5f, down = 2.0f * diag + 1.0f;
    Mesh prism;
    std::unordered_map<int, int> top, bottom;
    auto vtx = [&](std::unordered_map<int, int>& map, int v, float off) {
        auto it = map.find(v);
        if (it != map.end()) return it->second;
        int id = (int)prism.verts.size();
        prism.verts.push_back(m.verts[v] + n * off);
        map[v] = id;
        return id;
    };
    std::unordered_set<uint64_t> directed;
    for (int f : inner) {
        const auto& face = m.faces[f];
        for (size_t i = 0; i < face.size(); ++i) directed.insert(directedKey(face[i], face[(i + 1) % face.size()]));
    }
    for (int f : inner) {
        const auto& face = m.faces[f];
        std::vector<int> t, b;
        for (int v : face) {
            t.push_back(vtx(top, v, up));
            b.push_back(vtx(bottom, v, -down));
        }
        std::reverse(b.begin(), b.end());
        prism.faces.push_back(t);
        prism.faces.push_back(b);
        for (size_t i = 0; i < face.size(); ++i) {
            int a = face[i], c = face[(i + 1) % face.size()];
            if (directed.count(directedKey(c, a))) continue;
            prism.faces.push_back({vtx(top, c, up), vtx(top, a, up), vtx(bottom, a, -down), vtx(bottom, c, -down)});
        }
    }
    prism.touch();
    csg::Result r = csg::apply(m, prism, csg::Op::Difference);
    if (r.mesh.faces.empty()) {
        error = "Punch through removed everything (is the mesh closed?)";
        return false;
    }
    poly::cleanup(r.mesh);
    poly::triangulateMesh(r.mesh, 64);
    m = std::move(r.mesh);
    return true;
}
}  // namespace

bool punchThrough(Mesh& m, const std::vector<int>& region, Vec3 n, std::vector<char>& vsel, std::string& error) {
    vsel.assign(m.verts.size(), 0);
    // Outline of the region, counter-clockwise seen from +n.
    std::vector<std::pair<int, int>> border;
    std::vector<char> inRegion(m.faces.size(), 0);
    for (int f : region) inRegion[f] = 1;
    {
        std::unordered_set<uint64_t> directed;
        for (int f : region) {
            const auto& face = m.faces[f];
            for (size_t i = 0; i < face.size(); ++i) directed.insert(directedKey(face[i], face[(i + 1) % face.size()]));
        }
        for (int f : region) {
            const auto& face = m.faces[f];
            for (size_t i = 0; i < face.size(); ++i) {
                int a = face[i], b = face[(i + 1) % face.size()];
                if (!directed.count(directedKey(b, a))) border.push_back({a, b});
            }
        }
    }
    std::vector<Chain> chains;
    if (!chainEdges(border, chains) || chains.size() != 1 || !chains[0].closed || chains[0].v.size() < 3) {
        error = "Punch: the selected faces must form one region without holes";
        return false;
    }
    const std::vector<int> loop = chains[0].v;
    const Vec3 u = normalize(cross(n, std::fabs(n.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0)));
    const Vec3 w = cross(n, u);
    auto to2D = [&](Vec3 p) { return Vec2(dot(p, u), dot(p, w)); };
    std::vector<Vec2> P2;
    for (int v : loop) P2.push_back(to2D(m.verts[v]));
    float entryDepth = 1e30f;
    for (int v : loop) entryDepth = std::min(entryDepth, dot(m.verts[v], n));

    // Far side: faces below the entry whose centre projects inside the outline.
    std::vector<char> farKill(m.faces.size(), 0);
    int farCount = 0;
    for (size_t f = 0; f < m.faces.size(); ++f) {
        if (inRegion[f]) continue;
        Vec3 c = faceCenter(m, m.faces[f]);
        if (dot(c, n) < entryDepth && pointInPolygon(to2D(c), P2)) farKill[f] = 1, ++farCount;
    }
    // Light mesh: the exact Boolean cut, kept if it comes out watertight (BSP
    // splitting can leave cracks on curved, triangulated far sides).
    if (m.faces.size() <= 3000 || farCount == 0) {
        Mesh exact = m;
        std::string csgError;
        const bool ok = punchCSG(exact, region, n, csgError);
        if (ok && (farCount == 0 || isWatertight(exact))) {
            m = std::move(exact);
            vsel.assign(m.verts.size(), 0);
            return true;
        }
        if (farCount == 0) {
            error = csgError.empty() ? "Punch: nothing on the far side to cut through" : csgError;
            return false;
        }
    }
    // Remove the far faces and find the hole they leave (in the direction of
    // the remaining faces, like the entry outline after removing the region).
    std::vector<std::pair<int, int>> farBorder;
    {
        HalfEdges he(m);
        for (int h = 0; h < he.count(); ++h) {
            if (!farKill[he.face(h)]) continue;
            int t = he.twin(h);
            if (t >= 0 && farKill[he.face(t)]) continue;
            farBorder.push_back({he.to(h), he.from(h)});
        }
    }
    std::vector<Chain> farChains;
    if (!chainEdges(farBorder, farChains) || farChains.size() != 1 || !farChains[0].closed) {
        error = farChains.size() > 1 ? "Punch: the hole would pass through more than one far surface"
                                     : "Punch: the far side of the hole is not a simple loop";
        return false;
    }
    std::vector<char> kill(m.faces.size(), 0);
    for (size_t f = 0; f < kill.size(); ++f) kill[f] = inRegion[f] || farKill[f];
    // Entry loop in the direction of the remaining faces = reversed outline.
    std::vector<int> entry(loop.rbegin(), loop.rend());
    {
        const bool uvs = m.hasUVs();
        size_t wi = 0;
        for (size_t f = 0; f < m.faces.size(); ++f) {
            if (kill[f]) continue;
            if (wi != f) {
                m.faces[wi] = std::move(m.faces[f]);
                if (uvs) m.uvs[wi] = std::move(m.uvs[f]);
            }
            ++wi;
        }
        m.faces.resize(wi);
        if (uvs) m.uvs.resize(wi);
    }
    std::vector<int> walls;
    bridgeLoops(m, entry, farChains[0].v, true, BridgeParams(), walls);
    removeUnusedVertices(m);
    m.validate();
    vsel.assign(m.verts.size(), 0);
    for (int f : walls)
        for (int v : m.faces[f]) vsel[v] = 1;
    return true;
}

// ============================================================================
// Merge / join
// ============================================================================
int mergeAtCenter(Mesh& m, std::vector<char>& vsel) {
    if (vsel.size() != m.verts.size()) return 0;
    int keep = -1, count = 0;
    Vec3 c;
    for (size_t v = 0; v < vsel.size(); ++v)
        if (vsel[v]) {
            if (keep < 0) keep = (int)v;
            c += m.verts[v];
            ++count;
        }
    if (count < 2) return 0;
    m.verts[keep] = c / (float)count;
    for (auto& f : m.faces)
        for (int& v : f)
            if (vsel[v]) v = keep;
    removeDegenerate(m);
    std::vector<int> remap = removeUnusedVertices(m);
    m.validate();
    m.touch();
    vsel.assign(m.verts.size(), 0);
    if (keep < (int)remap.size() && remap[keep] >= 0) vsel[remap[keep]] = 1;
    return count - 1;
}

void appendMesh(Mesh& dst, const Mesh& src, const Mat4& xf) {
    const int base = (int)dst.verts.size();
    const bool dstUV = dst.hasUVs(), srcUV = src.hasUVs();
    const bool keepUV = dstUV || srcUV;
    const bool keepW = (dst.verts.empty() || dst.hasWeights()) && src.hasWeights();
    if (keepUV && !dstUV) {
        dst.uvs.clear();
        for (const auto& f : dst.faces) dst.uvs.push_back(std::vector<Vec2>(f.size()));
    }
    if (!keepW) dst.weights.clear();
    for (const Vec3& v : src.verts) dst.verts.push_back(transformPoint(xf, v));
    if (keepW) dst.weights.insert(dst.weights.end(), src.weights.begin(), src.weights.end());
    const bool flip = dot(cross(transformDir(xf, {1, 0, 0}), transformDir(xf, {0, 1, 0})), transformDir(xf, {0, 0, 1})) < 0;
    for (size_t f = 0; f < src.faces.size(); ++f) {
        std::vector<int> face = src.faces[f];
        for (int& v : face) v += base;
        std::vector<Vec2> u = srcUV ? src.uvs[f] : std::vector<Vec2>(face.size());
        if (flip) {  // mirrored transform: keep the faces pointing outwards
            std::reverse(face.begin(), face.end());
            std::reverse(u.begin(), u.end());
        }
        dst.faces.push_back(std::move(face));
        if (keepUV) dst.uvs.push_back(std::move(u));
    }
    dst.validate();
    dst.touch();
}

}  // namespace meshedit
