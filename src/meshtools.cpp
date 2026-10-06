#include "meshtools.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

namespace meshedit {
namespace {

inline uint64_t ukey(int a, int b) {
    if (a > b) std::swap(a, b);
    return (uint64_t(uint32_t(a)) << 32) | uint32_t(b);
}
inline uint64_t dkey(int a, int b) { return (uint64_t(uint32_t(a)) << 32) | uint32_t(b); }
inline Vec2 lerp2(Vec2 a, Vec2 b, float t) { return a + (b - a) * t; }

BoneWeights mixWeights(const BoneWeights& a, const BoneWeights& b, float t) {
    BoneWeights r;
    for (int k = 0; k < 4; ++k) {
        if (a.bone[k] >= 0 && a.w[k] > 0) r.add(a.bone[k], a.w[k] * (1.0f - t));
        if (b.bone[k] >= 0 && b.w[k] > 0) r.add(b.bone[k], b.w[k] * t);
    }
    r.normalize();
    return r;
}

// Appends vertices / faces while keeping the optional attribute arrays in
// step (capture the flags before the first append: pushing a vertex makes
// hasWeights() false until its weights follow).
struct Builder {
    Mesh& m;
    bool uvs, weights;
    explicit Builder(Mesh& mesh) : m(mesh), uvs(mesh.hasUVs()), weights(mesh.hasWeights()) {}
    int vertex(Vec3 p, const BoneWeights& w = BoneWeights()) {
        m.verts.push_back(p);
        if (weights) m.weights.push_back(w);
        return (int)m.verts.size() - 1;
    }
    int vertexLike(Vec3 p, int like) {
        BoneWeights w = weights && like >= 0 ? m.weights[like] : BoneWeights();
        return vertex(p, w);
    }
    int vertexMix(Vec3 p, int a, int b, float t) {
        BoneWeights w = weights ? mixWeights(m.weights[a], m.weights[b], t) : BoneWeights();
        return vertex(p, w);
    }
    // Repeated corners (two loops sharing a vertex, a face that lists a
    // vertex twice) are dropped; a face left with < 3 corners is not added
    // and -1 is returned (pushFace skips it).
    int face(std::vector<int> f, std::vector<Vec2> uv) {
        uv.resize(f.size());
        {
            std::vector<int> cf;
            std::vector<Vec2> cu;
            for (size_t i = 0; i < f.size(); ++i) {
                if (!cf.empty() && cf.back() == f[i]) continue;
                cf.push_back(f[i]);
                cu.push_back(uv[i]);
            }
            while (cf.size() > 1 && cf.front() == cf.back()) cf.pop_back(), cu.pop_back();
            if (cf.size() < 3) return -1;
            f.swap(cf);
            uv.swap(cu);
        }
        if (uvs) {
            uv.resize(f.size());
            m.uvs.push_back(std::move(uv));
        }
        m.faces.push_back(std::move(f));
        return (int)m.faces.size() - 1;
    }
    // Same cleaning for a rewritten face; one that collapses is remembered
    // in `dead` and removed by finish().
    std::vector<int> dead;
    void setFace(int f, std::vector<int> corners, std::vector<Vec2> uv) {
        uv.resize(corners.size());
        std::vector<int> cf;
        std::vector<Vec2> cu;
        for (size_t i = 0; i < corners.size(); ++i) {
            if (!cf.empty() && cf.back() == corners[i]) continue;
            cf.push_back(corners[i]);
            cu.push_back(uv[i]);
        }
        while (cf.size() > 1 && cf.front() == cf.back()) cf.pop_back(), cu.pop_back();
        if (cf.size() < 3) dead.push_back(f);
        if (uvs) m.uvs[f] = std::move(cu);
        m.faces[f] = std::move(cf);
    }
    // Removes the collapsed faces; `faces` (indices) is remapped.
    void finish(std::vector<int>* faces = nullptr);
    Vec2 uv(int f, int corner) const { return uvs ? m.uvs[f][corner] : Vec2(); }
};

// Directed edge lookups (kept as a small wrapper for the bridge / fill code).
struct HalfEdges {
    VertexFaces vf;
    mutable std::pair<int, int> hit;
    explicit HalfEdges(const Mesh& m) : vf(m) {}
    const std::pair<int, int>* find(int a, int b) const { return vf.directed(a, b, &hit) ? &hit : nullptr; }
};

void dropFaces(Mesh& m, const std::vector<char>& kill);  // below
inline void pushFace(std::vector<int>& list, int f) {
    if (f >= 0) list.push_back(f);
}

void Builder::finish(std::vector<int>* faces) {
    if (dead.empty()) return;
    std::vector<char> kill(m.faces.size(), 0);
    for (int f : dead) kill[f] = 1;
    std::vector<int> newIndex(m.faces.size(), -1);
    for (int f = 0, w = 0; f < (int)m.faces.size(); ++f)
        if (!kill[f]) newIndex[f] = w++;
    if (faces) {
        std::vector<int> out;
        for (int f : *faces)
            if (f >= 0 && f < (int)newIndex.size() && newIndex[f] >= 0) out.push_back(newIndex[f]);
        faces->swap(out);
    }
    dropFaces(m, kill);
    dead.clear();
}

void selectFaces(const Mesh& m, const std::vector<int>& faces, std::vector<char>& vsel) {
    vsel.assign(m.verts.size(), 0);
    for (int f : faces)
        if (f >= 0 && f < (int)m.faces.size())
            for (int v : m.faces[f]) vsel[v] = 1;
}

// Joins edges into chains. Fails if a vertex has more than two selected edges.
struct Chain {
    std::vector<int> verts;
    bool closed = false;
};
bool chainEdges(const std::vector<Edge>& edges, std::vector<Chain>& chains, std::string& error) {
    chains.clear();
    std::unordered_map<int, std::vector<int>> adj;
    std::unordered_set<uint64_t> seen;
    for (const Edge& e : edges) {
        if (e.first == e.second || !seen.insert(ukey(e.first, e.second)).second) continue;
        adj[e.first].push_back(e.second);
        adj[e.second].push_back(e.first);
    }
    for (const auto& kv : adj)
        if (kv.second.size() > 2) {
            error = "The selected edges branch (a vertex has 3+ selected edges)";
            return false;
        }
    std::unordered_set<int> used;
    auto walk = [&](int start) {
        Chain c;
        int prev = -1, cur = start;
        while (true) {
            c.verts.push_back(cur);
            used.insert(cur);
            int next = -1;
            for (int n : adj[cur])
                if (n != prev && !used.count(n)) {
                    next = n;
                    break;
                }
            if (next < 0) {
                // Closed if the last vertex links back to the start.
                if (c.verts.size() > 2)
                    for (int n : adj[cur])
                        if (n == start && prev != start) c.closed = true;
                break;
            }
            prev = cur;
            cur = next;
        }
        chains.push_back(std::move(c));
    };
    for (const auto& kv : adj)  // open chains start at an end
        if (kv.second.size() == 1 && !used.count(kv.first)) walk(kv.first);
    for (const auto& kv : adj)
        if (!used.count(kv.first)) walk(kv.first);
    return true;
}

// Orients a chain so that the faces along it run v[i] -> v[i+1] (i.e. the
// faces lie on the chain's left, the hole / open side on its right).
// Returns 1 if oriented by faces, 0 if the chain has no faces (wire), -1 if
// some of its edges have faces on both sides.
int orientByFaces(const HalfEdges& he, Chain& c) {
    int forward = 0, backward = 0, both = 0;
    const size_t n = c.verts.size(), edges = c.closed ? n : n - 1;
    for (size_t i = 0; i < edges; ++i) {
        int a = c.verts[i], b = c.verts[(i + 1) % n];
        bool f = he.find(a, b) != nullptr, r = he.find(b, a) != nullptr;
        if (f && r) ++both;
        else if (f) ++forward;
        else if (r) ++backward;
    }
    if (both > 0 && forward + backward == 0) return -1;
    if (backward > forward) std::reverse(c.verts.begin(), c.verts.end());
    return forward + backward > 0 ? 1 : 0;
}

std::vector<float> arcParams(const Mesh& m, const std::vector<int>& v, bool closed) {
    const size_t n = v.size();
    std::vector<float> t(n, 0.0f);
    float total = 0;
    for (size_t i = 1; i < n; ++i) {
        total += length(m.verts[v[i]] - m.verts[v[i - 1]]);
        t[i] = total;
    }
    if (closed && n > 1) total += length(m.verts[v[0]] - m.verts[v[n - 1]]);
    if (total <= 1e-12f) {
        for (size_t i = 0; i < n; ++i) t[i] = n > 1 ? (float)i / (closed ? n : n - 1) : 0.0f;
        return t;
    }
    for (float& x : t) x /= total;
    return t;
}

// Point at arc parameter s (0..1) along a polyline.
Vec3 pointAt(const Mesh& m, const std::vector<int>& v, const std::vector<float>& t, bool closed, float s) {
    const size_t n = v.size();
    if (n == 1) return m.verts[v[0]];
    for (size_t i = 0; i < n; ++i) {
        size_t j = i + 1;
        float t1;
        if (j == n) {
            if (!closed) break;
            j = 0;
            t1 = 1.0f;
        } else {
            t1 = t[j];
        }
        if (s <= t1 + 1e-7f) {
            float span = t1 - t[i];
            float k = span > 1e-12f ? (s - t[i]) / span : 0.0f;
            return lerp(m.verts[v[i]], m.verts[v[j]], clampf(k, 0.0f, 1.0f));
        }
    }
    return m.verts[v[closed ? 0 : n - 1]];
}

// Faces between two rings X and Y (indices into m.verts): every face uses
// X[i+1] -> X[i] and Y[j] -> Y[j+1], so the result continues faces that run
// X[i] -> X[i+1] on one side and Y[j+1] -> Y[j] on the other. Equal counts
// give quads; otherwise the rings are zipped by arc length into triangles
// and quads.
void zipRings(Builder& b, const std::vector<int>& X, const std::vector<int>& Y, bool closed, float vX, float vY,
              std::vector<int>& newFaces) {
    const Mesh& m = b.m;
    const size_t nx = X.size(), ny = Y.size();
    if (nx < 1 || ny < 1 || (nx == 1 && ny == 1)) return;
    std::vector<float> tx = arcParams(m, X, closed), ty = arcParams(m, Y, closed);
    auto ux = [&](size_t i) { return i >= nx ? 1.0f : tx[i]; };
    auto uy = [&](size_t j) { return j >= ny ? 1.0f : ty[j]; };
    const size_t endX = closed ? nx : nx - 1, endY = closed ? ny : ny - 1;
    size_t i = 0, j = 0;
    auto xi = [&](size_t k) { return X[k % nx]; };
    auto yj = [&](size_t k) { return Y[k % ny]; };
    while (i < endX || j < endY) {
        const float nextX = i < endX ? ux(i + 1) : 2.0f, nextY = j < endY ? uy(j + 1) : 2.0f;
        if (nx == ny || std::fabs(nextX - nextY) < 1e-4f) {
            if (i >= endX || j >= endY) {  // only one side left (unequal open chains)
                if (i < endX) {
                    pushFace(newFaces, b.face({xi(i + 1), xi(i), yj(j)}, {{ux(i + 1), vX}, {ux(i), vX}, {uy(j), vY}}));
                    ++i;
                } else {
                    pushFace(newFaces, b.face({xi(i), yj(j), yj(j + 1)}, {{ux(i), vX}, {uy(j), vY}, {uy(j + 1), vY}}));
                    ++j;
                }
                continue;
            }
            pushFace(newFaces, b.face({xi(i + 1), xi(i), yj(j), yj(j + 1)},
                                      {{ux(i + 1), vX}, {ux(i), vX}, {uy(j), vY}, {uy(j + 1), vY}}));
            ++i, ++j;
        } else if (nextX < nextY) {
            pushFace(newFaces, b.face({xi(i + 1), xi(i), yj(j)}, {{ux(i + 1), vX}, {ux(i), vX}, {uy(j), vY}}));
            ++i;
        } else {
            pushFace(newFaces, b.face({xi(i), yj(j), yj(j + 1)}, {{ux(i), vX}, {uy(j), vY}, {uy(j + 1), vY}}));
            ++j;
        }
    }
}

// Bridges two oriented loops A and B (faces already run A[i] -> A[i+1] and
// B[j] -> B[j+1]); B is walked backwards so the tube's faces agree with both.
void bridgeOriented(Mesh& m, std::vector<int> A, std::vector<int> B, bool closed, bool flipAllowed,
                    const BridgeParams& p, std::vector<int>& newFaces) {
    Builder b(m);
    std::vector<int> C(B.rbegin(), B.rend());
    auto cost = [&](const std::vector<int>& c, int off) {
        float s = 0;
        const size_t n = A.size();
        for (size_t i = 0; i < n; ++i) {
            float t = n > 1 ? (float)i / (float)n : 0.0f;
            size_t j = closed ? ((size_t)std::lround(t * c.size()) + off) % c.size()
                              : std::min(c.size() - 1, (size_t)std::lround((float)i / std::max<size_t>(1, n - 1) * (c.size() - 1)));
            Vec3 d = m.verts[A[i]] - m.verts[c[j]];
            s += dot(d, d);
        }
        return s;
    };
    auto bestOffset = [&](const std::vector<int>& c, float& best) {
        int off = 0;
        best = 1e30f;
        const int tries = closed ? (int)c.size() : 1;
        for (int o = 0; o < tries; ++o) {
            float s = cost(c, o);
            if (s < best) best = s, off = o;
        }
        return off;
    };
    float bestC;
    int off = bestOffset(C, bestC);
    if (flipAllowed) {  // wire chains have no preferred direction
        float bestB;
        int offB = bestOffset(B, bestB);
        if (bestB < bestC) C = B, off = offB;
    }
    if (closed && !C.empty()) {
        off = ((off + p.twist) % (int)C.size() + (int)C.size()) % (int)C.size();
        std::rotate(C.begin(), C.begin() + off, C.end());
    }
    // Intermediate rings (linear), sampled at A's arc parameters.
    const int segs = std::max(1, std::min(p.segments, 256));
    std::vector<float> ta = arcParams(m, A, closed), tc = arcParams(m, C, closed);
    std::vector<int> prev = A;
    for (int s = 1; s < segs; ++s) {
        const float k = (float)s / segs;
        std::vector<int> ring;
        for (size_t i = 0; i < A.size(); ++i) {
            Vec3 pc = pointAt(m, C, tc, closed, ta[i]);
            Vec3 pa = m.verts[A[i]];
            ring.push_back(b.vertexLike(lerp(pa, pc, k), A[i]));
        }
        zipRings(b, prev, ring, closed, (float)(s - 1) / segs, k, newFaces);
        prev = std::move(ring);
    }
    zipRings(b, prev, C, closed, (float)(segs - 1) / segs, 1.0f, newFaces);
}

// Ray against a face (fan triangulation), returns t.
bool rayFace(const Mesh& m, int f, Vec3 o, Vec3 d, float& tHit) {
    const auto& face = m.faces[f];
    bool hit = false;
    float best = 1e30f;
    for (size_t i = 1; i + 1 < face.size(); ++i) {
        Vec3 a = m.verts[face[0]], e1 = m.verts[face[i]] - a, e2 = m.verts[face[i + 1]] - a;
        Vec3 pv = cross(d, e2);
        float det = dot(e1, pv);
        if (std::fabs(det) < 1e-12f) continue;
        float inv = 1.0f / det;
        Vec3 tv = o - a;
        float u = dot(tv, pv) * inv;
        if (u < -1e-5f || u > 1 + 1e-5f) continue;
        Vec3 qv = cross(tv, e1);
        float v = dot(d, qv) * inv;
        if (v < -1e-5f || u + v > 1 + 1e-5f) continue;
        float t = dot(e2, qv) * inv;
        if (t > 1e-6f && t < best) best = t, hit = true;
    }
    if (hit) tHit = best;
    return hit;
}

}  // namespace

// ============================================================================
// Adding vertices
// ============================================================================
bool subdivideEdges(Mesh& m, const std::vector<Edge>& edges, int cuts, bool connect, std::vector<char>& vsel) {
    cuts = std::max(1, std::min(cuts, 64));
    Builder b(m);
    const int nv = (int)m.verts.size();
    std::unordered_map<uint64_t, int> firstNew;  // undirected edge -> first new vertex (ordered low -> high index)
    for (Edge e : edges) {
        e = makeEdge(e.first, e.second);
        if (e.first == e.second || e.first < 0 || e.second >= nv) continue;
        const uint64_t k = ukey(e.first, e.second);
        if (firstNew.count(k)) continue;
        firstNew[k] = (int)m.verts.size();
        for (int c = 1; c <= cuts; ++c) {
            float t = (float)c / (cuts + 1);
            b.vertexMix(lerp(m.verts[e.first], m.verts[e.second], t), e.first, e.second, t);
        }
    }
    if (firstNew.empty()) return false;
    const int newStart = nv;
    // Only faces around the split edges change.
    std::vector<int> touched;
    {
        VertexFaces vf(m);
        for (const auto& kv : firstNew) {
            const int a = (int)(kv.first >> 32);
            for (int k = vf.start[a]; k < vf.start[a + 1]; ++k) touched.push_back(vf.list[k]);
        }
        std::sort(touched.begin(), touched.end());
        touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
    }
    // A face with two cut edges is split by chords between their new points.
    // Two faces can ask for the same chord (around a vertex with only two
    // edges): that would make a zero-volume fin, so such faces - and faces
    // that pass a vertex twice - only get the new points (found by the fuzzer).
    std::unordered_map<uint64_t, int> chordUse;
    auto chordOf = [&](const std::vector<int>& face) -> uint64_t {
        const size_t n = face.size();
        int ends[2], found = 0;
        for (size_t i = 0; i < n; ++i) {
            int a = face[i], c = face[(i + 1) % n];
            auto it = firstNew.find(ukey(a, c));
            if (it == firstNew.end()) continue;
            if (found < 2) {
                // first new point of the first cut edge / last of the second, in face order
                ends[found] = found == 0 ? (a < c ? it->second : it->second + cuts - 1)
                                         : (a < c ? it->second + cuts - 1 : it->second);
            }
            ++found;
        }
        return found == 2 ? ukey(ends[0], ends[1]) : ~0ull;
    };
    if (connect)
        for (int tf : touched) {
            uint64_t k = chordOf(m.faces[tf]);
            if (k != ~0ull) chordUse[k]++;
        }
    for (int tf : touched) {
        const size_t f = (size_t)tf;
        const std::vector<int> face = m.faces[f];
        const size_t n = face.size();
        bool simple = true;
        for (size_t i = 0; i < n && simple; ++i)
            for (size_t j = i + 1; j < n; ++j)
                if (face[i] == face[j]) {
                    simple = false;
                    break;
                }
        const uint64_t chord = connect ? chordOf(face) : ~0ull;
        const bool mayConnect = connect && simple && (chord == ~0ull || chordUse[chord] == 1);
        std::vector<int> L;
        std::vector<Vec2> LU;
        std::vector<int> runs;  // position in L where each cut run starts
        for (size_t i = 0; i < n; ++i) {
            int a = face[i], c = face[(i + 1) % n];
            L.push_back(a);
            LU.push_back(b.uv((int)f, (int)i));
            auto it = firstNew.find(ukey(a, c));
            if (it == firstNew.end()) continue;
            runs.push_back((int)L.size());
            for (int k = 1; k <= cuts; ++k) {
                int idx = a < c ? it->second + (k - 1) : it->second + (cuts - k);
                L.push_back(idx);
                LU.push_back(lerp2(b.uv((int)f, (int)i), b.uv((int)f, (int)((i + 1) % n)), (float)k / (cuts + 1)));
            }
        }
        if (runs.empty()) continue;
        const int Ls = (int)L.size();
        auto piece = [&](int from, int to) {  // L[from..to] cyclically, inclusive
            std::vector<int> pv;
            std::vector<Vec2> pu;
            for (int k = from;; k = (k + 1) % Ls) {
                pv.push_back(L[k]);
                pu.push_back(LU[k]);
                if (k == to) break;
            }
            return std::make_pair(pv, pu);
        };
        if (mayConnect && runs.size() == 2) {
            const int i1 = runs[0], j1 = runs[1], c = cuts;
            std::vector<std::pair<std::vector<int>, std::vector<Vec2>>> pieces;
            pieces.push_back(piece((j1 + c - 1) % Ls, i1));
            for (int k = 1; k < c; ++k)
                pieces.push_back({{L[i1 + k - 1], L[i1 + k], L[j1 + c - 1 - k], L[j1 + c - k]},
                                  {LU[i1 + k - 1], LU[i1 + k], LU[j1 + c - 1 - k], LU[j1 + c - k]}});
            pieces.push_back(piece(i1 + c - 1, j1));
            b.setFace((int)f, pieces[0].first, pieces[0].second);
            for (size_t k = 1; k < pieces.size(); ++k) b.face(pieces[k].first, pieces[k].second);
        } else if (mayConnect && cuts == 1 && runs.size() == n && n >= 3) {
            // Every edge cut once: quads around a new centre vertex.
            Vec3 centre;
            Vec2 cu;
            for (size_t i = 0; i < n; ++i) {
                centre += m.verts[face[i]];
                cu = cu + b.uv((int)f, (int)i);
            }
            centre = centre / (float)n;
            cu = cu / (float)n;
            int ci = b.vertexLike(centre, face[0]);
            // L = c0 m0 c1 m1 ... ; corner i at 2i, its following midpoint at 2i+1.
            for (size_t i = 0; i < n; ++i) {
                int c0 = (int)(2 * i), mNext = (int)(2 * i + 1), mPrev = (int)((2 * i + 2 * n - 1) % (2 * n));
                std::vector<int> q = {L[c0], L[mNext], ci, L[mPrev]};
                std::vector<Vec2> qu = {LU[c0], LU[mNext], cu, LU[mPrev]};
                if (i == 0) b.setFace((int)f, q, qu);
                else b.face(q, qu);
            }
        } else {
            b.setFace((int)f, L, LU);
        }
    }
    b.finish();
    vsel.assign(m.verts.size(), 0);
    for (int v = newStart; v < (int)m.verts.size(); ++v) vsel[v] = 1;
    m.validate();
    m.touch();
    return true;
}

std::vector<Edge> edgeRing(const Mesh& m, Edge start) {
    start = makeEdge(start.first, start.second);
    VertexFaces vf(m);
    std::vector<Edge> ring = {start};
    std::unordered_set<uint64_t> visited = {ukey(start.first, start.second)};
    const auto first = vf.owners(start.first, start.second);
    if (first.empty()) return {};
    std::vector<std::pair<int, int>> own;
    for (const auto& startFace : first) {
        Edge cur = start;
        int face = startFace.first;
        for (int guard = 0; guard < 10000000; ++guard) {
            const auto& fv = m.faces[face];
            if (fv.size() != 4) break;
            int corner = -1;
            for (int i = 0; i < 4; ++i)
                if (makeEdge(fv[i], fv[(i + 1) % 4]) == cur) corner = i;
            if (corner < 0) break;
            Edge opp = makeEdge(fv[(corner + 2) % 4], fv[(corner + 3) % 4]);
            if (!visited.insert(ukey(opp.first, opp.second)).second) break;
            ring.push_back(opp);
            vf.owners(opp.first, opp.second, own);
            if (own.size() != 2) break;
            int next = own[0].first == face ? own[1].first : own[0].first;
            if (next == face) break;
            cur = opp;
            face = next;
        }
    }
    return ring;
}

std::vector<Edge> edgeLoop(const Mesh& m, Edge start) {
    start = makeEdge(start.first, start.second);
    VertexFaces vf(m);
    std::vector<Edge> loop = {start};
    std::unordered_set<uint64_t> visited = {ukey(start.first, start.second)};
    // Side edge at `v` of face f next to edge (u, v).
    auto sideEdge = [&](int f, int u, int v) {
        const auto& fv = m.faces[f];
        const int n = (int)fv.size();
        for (int i = 0; i < n; ++i)
            if (fv[i] == v) {
                int p = fv[(i + n - 1) % n], q = fv[(i + 1) % n];
                return p == u ? q : p;
            }
        return -1;
    };
    std::vector<std::pair<int, int>> own;
    std::vector<int> nb;
    for (int dir = 0; dir < 2; ++dir) {
        int u = dir ? start.second : start.first, v = dir ? start.first : start.second;
        for (int guard = 0; guard < 10000000; ++guard) {
            vf.owners(u, v, own);
            if (own.size() != 2) break;
            // Neighbours of v from the faces around it.
            nb.clear();
            for (int k = vf.start[v]; k < vf.start[v + 1]; ++k) {
                const auto& fv = m.faces[vf.list[k]];
                const int n = (int)fv.size();
                for (int i = 0; i < n; ++i)
                    if (fv[i] == v) {
                        nb.push_back(fv[(i + n - 1) % n]);
                        nb.push_back(fv[(i + 1) % n]);
                    }
            }
            std::sort(nb.begin(), nb.end());
            nb.erase(std::unique(nb.begin(), nb.end()), nb.end());
            if (nb.size() != 4) break;
            int s1 = sideEdge(own[0].first, u, v), s2 = sideEdge(own[1].first, u, v);
            int next = -1, count = 0;
            for (int w : nb)
                if (w != u && w != s1 && w != s2) next = w, ++count;
            if (count != 1) break;
            if (!visited.insert(ukey(v, next)).second) break;
            loop.push_back(makeEdge(v, next));
            u = v;
            v = next;
        }
    }
    return loop;
}

bool loopCut(Mesh& m, Edge edge, int cuts, std::vector<char>& vsel) {
    std::vector<Edge> ring = edgeRing(m, edge);
    if (ring.empty()) return false;
    return subdivideEdges(m, ring, cuts, true, vsel);
}

int connectVertices(Mesh& m, const std::vector<char>& vsel) {
    if (vsel.size() != m.verts.size()) return 0;
    Builder b(m);
    int splits = 0;
    const size_t faceCount = m.faces.size();
    // A chord must join two different vertices that are not joined yet - by
    // an existing edge, or by a chord made in another face during this call.
    // Otherwise the edge would end up on 3+ faces (found by the fuzzer).
    const VertexFaces vf(m);
    std::unordered_set<uint64_t> created;
    for (size_t f = 0; f < faceCount; ++f) {
        int selected = 0;
        for (int v : m.faces[f]) selected += vsel[v] ? 1 : 0;
        if (selected < 2) continue;  // most faces: no copy, no allocation
        const std::vector<int> face = m.faces[f];
        const int n = (int)face.size();
        std::vector<int> s;
        for (int i = 0; i < n; ++i)
            if (vsel[face[i]]) s.push_back(i);
        const int k = (int)s.size();
        auto adjacent = [&](int a, int c) { return (a + 1) % n == c || (c + 1) % n == a; };
        auto chordOk = [&](int a, int c) {
            const int va = face[a], vc = face[c];
            return va != vc && !created.count(ukey(va, vc)) && vf.owners(va, vc).empty();
        };
        // Which consecutive pairs of selected corners get a chord.
        std::vector<char> cut(k, 0);
        for (int i = 0; i < k; ++i) {
            const int a = s[i], c = s[(i + 1) % k];
            cut[i] = a != c && !adjacent(a, c) && chordOk(a, c);
        }
        if (k == 2) cut[1] = cut[0];  // one chord, two arcs
        std::vector<std::vector<int>> pv;
        std::vector<std::vector<Vec2>> pu;
        std::vector<int> core;
        std::vector<Vec2> coreU;
        for (int i = 0; i < k; ++i) {
            int a = s[i], c = s[(i + 1) % k];
            if (k == 2 && i == 1) {  // two corners: the second arc closes the face
                if (!cut[0]) break;
            }
            if (cut[i]) {
                created.insert(ukey(face[a], face[c]));
                std::vector<int> v;
                std::vector<Vec2> u;
                for (int j = a;; j = (j + 1) % n) {
                    v.push_back(face[j]);
                    u.push_back(b.uv((int)f, j));
                    if (j == c) break;
                }
                pv.push_back(v);
                pu.push_back(u);
                core.push_back(face[a]);
                coreU.push_back(b.uv((int)f, a));
            } else {
                for (int j = a; j != c; j = (j + 1) % n) {
                    core.push_back(face[j]);
                    coreU.push_back(b.uv((int)f, j));
                }
            }
        }
        if (pv.empty()) continue;
        if (k == 2) {  // exactly two pieces
            if (pv.size() == 1) {
                // Second arc (from s[1] back to s[0]).
                std::vector<int> v;
                std::vector<Vec2> u;
                for (int j = s[1];; j = (j + 1) % n) {
                    v.push_back(face[j]);
                    u.push_back(b.uv((int)f, j));
                    if (j == s[0]) break;
                }
                pv.push_back(v);
                pu.push_back(u);
            }
            core.clear();
        }
        bool first = true;
        auto emit = [&](std::vector<int> v, std::vector<Vec2> u) {
            if (v.size() < 3) return;
            if (first) b.setFace((int)f, v, u), first = false;
            else b.face(v, u);
        };
        for (size_t i = 0; i < pv.size(); ++i) emit(pv[i], pu[i]);
        emit(core, coreU);
        ++splits;
    }
    if (splits) {
        b.finish();
        m.validate();
        m.touch();
    }
    return splits;
}

bool pokeFaces(Mesh& m, const std::vector<int>& faces, float offset, std::vector<int>& newFaces,
               std::vector<char>& vsel) {
    newFaces.clear();
    Builder b(m);
    for (int f : faces) {
        if (f < 0 || f >= (int)m.faces.size()) continue;
        const std::vector<int> face = m.faces[f];
        const size_t n = face.size();
        Vec3 c = faceCenter(m, face) + normalize(faceNormalRaw(m, face)) * offset;
        Vec2 cu;
        for (size_t i = 0; i < n; ++i) cu = cu + b.uv(f, (int)i);
        cu = cu / (float)n;
        std::vector<Vec2> fu(n);
        for (size_t i = 0; i < n; ++i) fu[i] = b.uv(f, (int)i);
        int ci = b.vertexLike(c, face[0]);
        for (size_t i = 0; i < n; ++i) {
            std::vector<int> tri = {face[i], face[(i + 1) % n], ci};
            std::vector<Vec2> tu = {fu[i], fu[(i + 1) % n], cu};
            if (i == 0) {
                b.setFace(f, tri, tu);
                newFaces.push_back(f);
            } else {
                pushFace(newFaces, b.face(tri, tu));
            }
        }
    }
    b.finish(&newFaces);
    if (newFaces.empty()) return false;
    selectFaces(m, newFaces, vsel);
    m.validate();
    m.touch();
    return true;
}

// ============================================================================
// Bevel
// ============================================================================
bool bevel(Mesh& m, const std::vector<Edge>& edgesIn, const std::vector<int>& vertsIn, const BevelParams& p,
           std::vector<int>& newFaces, std::vector<char>& vsel) {
    newFaces.clear();
    if (!(p.width > 0.0f) || !std::isfinite(p.width)) return false;
    const int nv = (int)m.verts.size();
    VertexFaces vf(m);
    std::unordered_set<uint64_t> E;
    std::unordered_map<uint64_t, std::vector<std::pair<int, int>>> ef;  // owners of the beveled edges only
    for (const Edge& e : edgesIn) {
        if (e.first == e.second || e.first < 0 || e.second < 0 || e.first >= nv || e.second >= nv) continue;
        auto own = vf.owners(e.first, e.second);
        if (own.size() == 2) {
            E.insert(ukey(e.first, e.second));
            ef[ukey(e.first, e.second)] = std::move(own);
        }
    }
    std::vector<char> V(nv, 0);
    const bool vertexMode = E.empty();
    if (vertexMode)
        for (int v : vertsIn)
            if (v >= 0 && v < nv) V[v] = 1;
    std::vector<char> affected(nv, 0);
    bool any = false;
    for (uint64_t k : E) affected[k >> 32] = affected[k & 0xFFFFFFFFu] = 1, any = true;
    for (int v = 0; v < nv; ++v)
        if (V[v]) affected[v] = 1, any = true;
    if (!any) return false;

    Builder b(m);
    // Open-boundary half-edges between affected vertices (never filled as holes).
    std::unordered_set<uint64_t> originalBoundary;
    std::vector<int> touchedFaces;  // faces with an affected corner: the only ones that change
    {
        std::vector<std::pair<int, int>> own;
        for (int v = 0; v < nv; ++v) {
            if (!affected[v]) continue;
            for (int k = vf.start[v]; k < vf.start[v + 1]; ++k) {
                const int f = vf.list[k];
                touchedFaces.push_back(f);
                const auto& fv = m.faces[f];
                const size_t n = fv.size();
                for (size_t i = 0; i < n; ++i) {
                    const int x = fv[i], y = fv[(i + 1) % n];
                    if (x != v || !affected[y]) continue;
                    vf.owners(x, y, own);
                    if (own.size() == 1) originalBoundary.insert(dkey(x, y));
                }
            }
        }
        std::sort(touchedFaces.begin(), touchedFaces.end());
        touchedFaces.erase(std::unique(touchedFaces.begin(), touchedFaces.end()), touchedFaces.end());
    }

    auto slideDist = [&](int v, int u) {
        float len = length(m.verts[u] - m.verts[v]);
        float d = p.width;
        if (p.clamp) d = std::min(d, (affected[u] ? 0.5f : 0.9f) * len);
        return d;
    };
    std::unordered_map<uint64_t, int> slide;  // (v, u): point on edge v-u near v
    std::unordered_map<uint64_t, int> inner;  // (v, face)
    auto slidePoint = [&](int v, int u) {
        auto it = slide.find(dkey(v, u));
        if (it != slide.end()) return it->second;
        Vec3 dir = normalize(m.verts[u] - m.verts[v]);
        int idx = b.vertexLike(m.verts[v] + dir * slideDist(v, u), v);
        slide[dkey(v, u)] = idx;
        return idx;
    };
    const int faceCount = (int)m.faces.size();
    // Pass A: create the slide / inner vertices the beveled corners need.
    for (int f : touchedFaces) {
        const auto& face = m.faces[f];
        const int n = (int)face.size();
        for (int i = 0; i < n; ++i) {
            const int v = face[i];
            if (!affected[v]) continue;
            const int u = face[(i + n - 1) % n], w = face[(i + 1) % n];
            const bool inB = E.count(ukey(u, v)) > 0, outB = E.count(ukey(v, w)) > 0;
            if (V[v]) {
                slidePoint(v, u);
                slidePoint(v, w);
            } else if (inB && outB) {
                Vec3 du = normalize(m.verts[u] - m.verts[v]), dw = normalize(m.verts[w] - m.verts[v]);
                Vec3 pos = m.verts[v] + du * slideDist(v, u) + dw * slideDist(v, w);
                inner[dkey(v, f)] = b.vertexLike(pos, v);
            } else if (inB) {
                slidePoint(v, w);
            } else if (outB) {
                slidePoint(v, u);
            }
        }
    }
    // Pass B: new corner lists (with UVs) for every face touching a beveled vertex.
    std::unordered_map<int, Vec2> uvOf;
    std::vector<std::vector<int>> repl(faceCount);       // per face: flattened corners
    std::vector<std::vector<int>> replStart(faceCount);  // per face: first index in repl of each original corner
    std::vector<std::vector<Vec2>> replUV(faceCount);
    for (int f : touchedFaces) {
        const std::vector<int>& face = m.faces[f];
        const int n = (int)face.size();
        auto& L = repl[f];
        auto& LU = replUV[f];
        for (int i = 0; i < n; ++i) {
            const int v = face[i], ip = (i + n - 1) % n, in = (i + 1) % n;
            const int u = face[ip], w = face[in];
            replStart[f].push_back((int)L.size());
            const Vec2 uvV = b.uv(f, i), uvU = b.uv(f, ip), uvW = b.uv(f, in);
            auto pushSlide = [&](int other, Vec2 uvOther) {
                auto it = slide.find(dkey(v, other));
                float len = length(m.verts[other] - m.verts[v]);
                float t = len > 1e-12f ? slideDist(v, other) / len : 0.0f;
                L.push_back(it->second);
                LU.push_back(lerp2(uvV, uvOther, t));
            };
            if (!affected[v]) {
                L.push_back(v);
                LU.push_back(uvV);
                continue;
            }
            const bool inB = E.count(ukey(u, v)) > 0, outB = E.count(ukey(v, w)) > 0;
            if (V[v]) {
                pushSlide(u, uvU);
                pushSlide(w, uvW);
            } else if (inB && outB) {
                float lu = length(m.verts[u] - m.verts[v]), lw = length(m.verts[w] - m.verts[v]);
                float tu = lu > 1e-12f ? slideDist(v, u) / lu : 0.0f, tw = lw > 1e-12f ? slideDist(v, w) / lw : 0.0f;
                L.push_back(inner[dkey(v, f)]);
                LU.push_back(uvV + (uvU - uvV) * tu + (uvW - uvV) * tw);
            } else if (inB) {
                pushSlide(w, uvW);
            } else if (outB) {
                pushSlide(u, uvU);
            } else {
                const bool su = slide.count(dkey(v, u)) > 0, sw = slide.count(dkey(v, w)) > 0;
                if (su) pushSlide(u, uvU);
                if (!su || !sw) {
                    L.push_back(v);
                    LU.push_back(uvV);
                }
                if (sw) pushSlide(w, uvW);
            }
        }
    }
    // Strips along the beveled edges (computed before faces are replaced).
    struct Strip {
        std::vector<int> f;
        std::vector<Vec2> u;
    };
    std::vector<Strip> strips;
    const int segs = std::max(1, std::min(p.segments, 64));
    for (uint64_t k : E) {
        const auto& owners = ef[k];
        // F1 runs a -> b, F2 runs b -> a.
        const int f1 = owners[0].first, c1 = owners[0].second;
        const int f2 = owners[1].first, c2 = owners[1].second;
        const int n1 = (int)m.faces[f1].size(), n2 = (int)m.faces[f2].size();
        const int a = m.faces[f1][c1];
        auto cornerOf = [&](int f, int c) { return replStart[f][c]; };
        const int ix1 = cornerOf(f1, c1), iy1 = cornerOf(f1, (c1 + 1) % n1);
        const int iy2 = cornerOf(f2, c2), ix2 = cornerOf(f2, (c2 + 1) % n2);
        const int x1 = repl[f1][ix1], y1 = repl[f1][iy1], y2 = repl[f2][iy2], x2 = repl[f2][ix2];
        const Vec2 ux1 = replUV[f1][ix1], uy1 = replUV[f1][iy1], uy2 = replUV[f2][iy2], ux2 = replUV[f2][ix2];
        const int bv = m.faces[f1][(c1 + 1) % n1];
        // Profile points at each end: x1 .. x2 at a, y1 .. y2 at b.
        auto profile = [&](int s, int e, int corner, std::vector<int>& out) {
            if (s == e) {  // both sides slide to one point: the profile is that point
                out.assign(segs + 1, s);
                return;
            }
            out.assign(segs + 1, -1);
            out[0] = s;
            out[segs] = e;
            Vec3 ps = m.verts[s], pe = m.verts[e], mid = (ps + pe) * 0.5f;
            Vec3 ctrl = mid + (m.verts[corner] - mid) * (2.0f * clampf(p.profile, 0.0f, 1.0f));
            for (int j = 1; j < segs; ++j) {
                float t = (float)j / segs;
                Vec3 q = ps * ((1 - t) * (1 - t)) + ctrl * (2 * t * (1 - t)) + pe * (t * t);
                out[j] = b.vertexLike(q, corner);
            }
        };
        std::vector<int> Pa, Pb;
        profile(x1, x2, a, Pa);
        profile(y1, y2, bv, Pb);
        for (int j = 0; j < segs; ++j) {
            float t0 = (float)j / segs, t1 = (float)(j + 1) / segs;
            Strip s;
            s.f = {Pb[j], Pa[j], Pa[j + 1], Pb[j + 1]};
            s.u = {lerp2(uy1, uy2, t0), lerp2(ux1, ux2, t0), lerp2(ux1, ux2, t1), lerp2(uy1, uy2, t1)};
            strips.push_back(std::move(s));
        }
    }
    // Corners that meet at one point (both sides of a bevel sliding to the
    // same point, e.g. at a vertex with only two edges) collapse: drop
    // repeated corners; a face left with fewer than 3 is removed.
    auto clean = [](std::vector<int>& v, std::vector<Vec2>& u) {
        std::vector<int> ov;
        std::vector<Vec2> ou;
        for (size_t i = 0; i < v.size(); ++i) {
            if (!ov.empty() && ov.back() == v[i]) continue;
            ov.push_back(v[i]);
            ou.push_back(i < u.size() ? u[i] : Vec2());
        }
        while (ov.size() > 1 && ov.front() == ov.back()) ov.pop_back(), ou.pop_back();
        v.swap(ov);
        u.swap(ou);
        return v.size() >= 3;
    };
    std::vector<char> collapsed(m.faces.size(), 0);
    // Replace the touched faces, add the strips.
    for (int f : touchedFaces) {
        if (repl[f].empty()) continue;
        for (size_t i = 0; i < repl[f].size(); ++i) uvOf[repl[f][i]] = replUV[f][i];
        if (!clean(repl[f], replUV[f])) collapsed[f] = 1;
        b.setFace(f, repl[f], replUV[f]);
    }
    for (Strip& s : strips) {
        for (size_t i = 0; i < s.f.size(); ++i) uvOf[s.f[i]] = s.u[i];
        if (!clean(s.f, s.u)) continue;
        pushFace(newFaces, b.face(std::move(s.f), std::move(s.u)));
    }
    // Close the holes left where bevels meet (and at beveled vertices).
    std::vector<char> inHole(m.verts.size(), 0);
    for (int v = nv; v < (int)m.verts.size(); ++v) inHole[v] = 1;
    for (int v = 0; v < nv; ++v)
        if (affected[v]) inHole[v] = 1;
    // A half-edge between two hole vertices belongs to a touched face or a
    // strip, and so does its twin: only those faces need to be looked at.
    // Every face around a hole vertex (walking around a vertex can cross faces
    // the bevel did not touch). One pass over the corners.
    std::vector<int> holeFaces;
    collapsed.resize(m.faces.size(), 0);
    for (int f = 0; f < (int)m.faces.size(); ++f) {
        if (collapsed[f]) continue;
        for (int v : m.faces[f])
            if (inHole[v]) {
                holeFaces.push_back(f);
                break;
            }
    }
    std::unordered_set<uint64_t> directed;
    for (int f : holeFaces) {
        const auto& face = m.faces[f];
        for (size_t i = 0; i < face.size(); ++i) directed.insert(dkey(face[i], face[(i + 1) % face.size()]));
    }
    // Border half-edges x -> y (no twin) between hole vertices; each hole is
    // walked by rotating around its vertices through the faces, so two holes
    // that touch at one vertex (e.g. a cone's tip that stays in place) are
    // kept apart - a "next vertex" map can only hold one of them.
    std::unordered_map<uint64_t, std::pair<int, int>> heMap;  // x -> y : (face, corner of x)
    for (int f : holeFaces) {
        const auto& face = m.faces[f];
        for (size_t i = 0; i < face.size(); ++i) heMap[dkey(face[i], face[(i + 1) % face.size()])] = {f, (int)i};
    }
    auto isBorder = [&](int x, int y) {
        return !directed.count(dkey(y, x)) && inHole[x] && inHole[y] &&
               !(x < nv && y < nv && originalBoundary.count(dkey(x, y)));
    };
    // From border half-edge x -> y: the border half-edge z -> x that follows
    // it around the hole (rotating around x from the face of x -> y).
    auto nextBorder = [&](int x, int y) {
        auto it = heMap.find(dkey(x, y));
        if (it == heMap.end()) return -1;
        int f = it->second.first, c = it->second.second;
        for (int guard = 0; guard < 256; ++guard) {
            const auto& fv = m.faces[f];
            const int n = (int)fv.size();
            const int pv = fv[(c + n - 1) % n];  // incoming edge pv -> x
            auto tw = heMap.find(dkey(x, pv));
            if (tw == heMap.end() || !directed.count(dkey(x, pv))) return pv;
            f = tw->second.first;
            c = tw->second.second;
        }
        return -1;
    };
    std::unordered_set<uint64_t> usedBorder;
    for (int f : holeFaces)
        for (size_t i = 0, n = m.faces[f].size(); i < n; ++i) {
            const int x0 = m.faces[f][i], y0 = m.faces[f][(i + 1) % n];
            if (!isBorder(x0, y0) || usedBorder.count(dkey(x0, y0))) continue;
            // Hole face: y0 -> x0 -> z1 -> ... -> back to y0.
            std::vector<int> loop = {y0, x0};
            usedBorder.insert(dkey(x0, y0));
            int cx = x0, cy = y0;
            bool ok = false;
            // Walk until the next border is the starting one again (a hole may
            // pass a pinched vertex twice, so reaching y0 is not enough).
            for (int guard = 0; guard < 100000; ++guard) {
                const int z = nextBorder(cx, cy);  // border half-edge z -> cx
                if (z < 0) break;
                if (z == x0 && cx == y0) {
                    ok = true;
                    loop.pop_back();  // y0 again
                    break;
                }
                if (!isBorder(z, cx) || usedBorder.count(dkey(z, cx))) break;
                usedBorder.insert(dkey(z, cx));
                loop.push_back(z);
                cy = cx;
                cx = z;
            }
            if (!ok || loop.size() < 3) continue;
            std::vector<Vec2> lu;
            for (int v : loop) lu.push_back(uvOf.count(v) ? uvOf[v] : Vec2());
            if (!clean(loop, lu)) continue;
            pushFace(newFaces, b.face(loop, lu));
        }
    // Remove faces that collapsed to a point or a line (new-face indices shift).
    if (std::count(collapsed.begin(), collapsed.end(), 1) > 0) {
        collapsed.resize(m.faces.size(), 0);
        std::vector<int> shift(m.faces.size(), 0);
        int removed = 0;
        for (size_t f = 0; f < m.faces.size(); ++f) {
            shift[f] = removed;
            removed += collapsed[f];
        }
        for (int& f : newFaces) f -= shift[f];
        dropFaces(m, collapsed);
    }
    // Drop the original vertices the bevel replaced, keeping face indices.
    selectFaces(m, newFaces, vsel);
    std::vector<int> remap = removeUnusedVertices(m);
    std::vector<char> sel(m.verts.size(), 0);
    for (size_t v = 0; v < remap.size() && v < vsel.size(); ++v)
        if (remap[v] >= 0 && vsel[v]) sel[remap[v]] = 1;
    vsel = std::move(sel);
    m.validate();
    m.touch();
    return true;
}

// ============================================================================
// Bridge / fill
// ============================================================================
bool bridgeLoops(Mesh& m, const std::vector<Edge>& edges, const BridgeParams& p, std::vector<int>& newFaces,
                 std::vector<char>& vsel, std::string& error) {
    newFaces.clear();
    std::vector<Chain> chains;
    if (!chainEdges(edges, chains, error)) return false;
    if (chains.size() != 2) {
        error = chains.size() < 2 ? "Bridge needs two separate edge loops or chains"
                                  : "Bridge needs exactly two edge loops (" + std::to_string(chains.size()) + " found)";
        return false;
    }
    if (chains[0].closed != chains[1].closed) {
        error = "Bridge: one selection is a closed loop, the other an open chain";
        return false;
    }
    const bool closed = chains[0].closed;
    if (chains[0].verts.size() < (closed ? 3u : 2u) || chains[1].verts.size() < (closed ? 3u : 2u)) {
        error = "Bridge: each loop needs more vertices";
        return false;
    }
    HalfEdges he(m);
    int o0 = orientByFaces(he, chains[0]), o1 = orientByFaces(he, chains[1]);
    if (o0 < 0 || o1 < 0) {
        error = "Bridge needs border edges (or select two groups of faces to replace them)";
        return false;
    }
    const size_t before = m.verts.size();
    bridgeOriented(m, chains[0].verts, chains[1].verts, closed, o0 == 0 || o1 == 0, p, newFaces);
    (void)before;
    selectFaces(m, newFaces, vsel);
    m.validate();
    m.touch();
    return !newFaces.empty();
}

std::vector<std::vector<int>> faceRegions(const Mesh& m, const std::vector<int>& faces) {
    std::unordered_map<int, int> index;  // face -> position
    for (size_t i = 0; i < faces.size(); ++i) index[faces[i]] = (int)i;
    std::unordered_map<uint64_t, std::vector<int>> byEdge;
    for (int f : faces) {
        const auto& fv = m.faces[f];
        for (size_t i = 0; i < fv.size(); ++i) byEdge[ukey(fv[i], fv[(i + 1) % fv.size()])].push_back(f);
    }
    std::vector<std::vector<int>> out;
    std::unordered_set<int> seen;
    for (int f0 : faces) {
        if (seen.count(f0)) continue;
        std::vector<int> region, stack = {f0};
        seen.insert(f0);
        while (!stack.empty()) {
            int f = stack.back();
            stack.pop_back();
            region.push_back(f);
            const auto& fv = m.faces[f];
            for (size_t i = 0; i < fv.size(); ++i)
                for (int g : byEdge[ukey(fv[i], fv[(i + 1) % fv.size()])])
                    if (seen.insert(g).second) stack.push_back(g);
        }
        out.push_back(std::move(region));
    }
    return out;
}

std::vector<std::vector<int>> regionBorders(const Mesh& m, const std::vector<int>& faces) {
    std::unordered_set<uint64_t> directed;
    for (int f : faces) {
        const auto& fv = m.faces[f];
        for (size_t i = 0; i < fv.size(); ++i) directed.insert(dkey(fv[i], fv[(i + 1) % fv.size()]));
    }
    std::unordered_map<int, int> next;
    for (int f : faces) {
        const auto& fv = m.faces[f];
        for (size_t i = 0; i < fv.size(); ++i) {
            int a = fv[i], c = fv[(i + 1) % fv.size()];
            if (!directed.count(dkey(c, a))) next[a] = c;
        }
    }
    std::vector<std::vector<int>> loops;
    std::unordered_set<int> used;
    for (const auto& kv : next) {
        if (used.count(kv.first)) continue;
        std::vector<int> loop;
        int cur = kv.first;
        for (int guard = 0; guard < 10000000 && !used.count(cur); ++guard) {
            loop.push_back(cur);
            used.insert(cur);
            auto it = next.find(cur);
            if (it == next.end()) break;
            cur = it->second;
        }
        if (loop.size() >= 3) loops.push_back(std::move(loop));
    }
    return loops;
}

void deleteFaces(Mesh& m, const std::vector<char>& kill) {
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
    removeUnusedVertices(m);
    m.validate();
    m.touch();
}

namespace {
// Removes faces without touching the vertex list (indices stay valid).
void dropFaces(Mesh& m, const std::vector<char>& kill) {
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
}
// After removeUnusedVertices: remap a selection of faces' vertices.
void finishWithFaces(Mesh& m, const std::vector<int>& newFaces, std::vector<char>& vsel) {
    removeUnusedVertices(m);
    m.validate();
    m.touch();
    selectFaces(m, newFaces, vsel);
}
}  // namespace

bool bridgeRegions(Mesh& m, const std::vector<int>& faces, const BridgeParams& p, std::vector<int>& newFaces,
                   std::vector<char>& vsel, std::string& error) {
    newFaces.clear();
    auto regions = faceRegions(m, faces);
    if (regions.size() != 2) {
        error = regions.size() < 2 ? "Bridge faces: select two separate groups of faces"
                                   : "Bridge faces: " + std::to_string(regions.size()) + " groups selected, need 2";
        return false;
    }
    std::vector<std::vector<int>> loops;
    for (const auto& r : regions) {
        auto borders = regionBorders(m, r);
        if (borders.size() != 1) {
            error = borders.empty() ? "Bridge faces: a selected group has no border (it is a closed surface)"
                                    : "Bridge faces: each group must be a single patch (one border)";
            return false;
        }
        // After the faces go, the surrounding faces run the other way round.
        std::reverse(borders[0].begin(), borders[0].end());
        loops.push_back(borders[0]);
    }
    {
        // Borders that touch (two faces meeting at a cone's tip) cannot be
        // joined by a tube: refuse instead of making overlapping faces.
        std::unordered_set<int> first(loops[0].begin(), loops[0].end());
        for (int v : loops[1])
            if (first.count(v)) {
                error = "Bridge faces: the two groups touch; pick groups that share no vertex";
                return false;
            }
        // Borders already joined by edges (two opposite faces of a box): a
        // one-segment bridge would duplicate the faces that join them.
        if (p.segments <= 1) {
            const VertexFaces vf(m);
            for (int a : loops[0]) {
                for (int k = vf.start[a]; k < vf.start[a + 1]; ++k)
                    for (int v : m.faces[vf.list[k]])
                        if (v != a && !first.count(v) &&
                            std::find(loops[1].begin(), loops[1].end(), v) != loops[1].end() &&
                            !vf.owners(a, v).empty()) {
                            error = "Bridge faces: the two groups are already joined by edges (use more segments, "
                                    "or Push through)";
                            return false;
                        }
            }
        }
    }
    std::vector<char> kill(m.faces.size(), 0);
    for (int f : faces) kill[f] = 1;
    dropFaces(m, kill);
    bridgeOriented(m, loops[0], loops[1], true, false, p, newFaces);
    finishWithFaces(m, newFaces, vsel);
    return !newFaces.empty();
}

int fillHoles(Mesh& m, const std::vector<Edge>& edges, std::vector<int>& newFaces, std::string& error) {
    newFaces.clear();
    std::vector<Chain> chains;
    if (!chainEdges(edges, chains, error)) return 0;
    HalfEdges he(m);
    Builder b(m);
    for (Chain& c : chains) {
        if (c.verts.size() < 3) continue;
        if (orientByFaces(he, c) < 0) continue;  // interior edges: nothing to fill
        // Faces run v[i] -> v[i+1]; the new face runs the other way.
        std::vector<int> f(c.verts.rbegin(), c.verts.rend());
        std::vector<Vec2> u;
        Vec3 n = faceNormalRaw(m, f), t = normalize(m.verts[f[1]] - m.verts[f[0]]), s = cross(normalize(n), t);
        for (int v : f) {
            Vec3 d = m.verts[v] - m.verts[f[0]];
            u.push_back({dot(d, t), dot(d, s)});
        }
        pushFace(newFaces, b.face(f, u));
    }
    if (newFaces.empty()) {
        error = "Fill: select a border loop (edges with a face on one side only)";
        return 0;
    }
    m.validate();
    m.touch();
    return (int)newFaces.size();
}

// ============================================================================
// Push through
// ============================================================================
bool pushThrough(Mesh& m, const std::vector<int>& facesIn, const PushThroughParams& p, std::vector<int>& newFaces,
                 std::vector<char>& vsel, std::string& error) {
    newFaces.clear();
    std::vector<int> faces;
    for (int f : facesIn)
        if (f >= 0 && f < (int)m.faces.size()) faces.push_back(f);
    if (faces.empty()) {
        error = "Push through: select faces";
        return false;
    }
    if (faceRegions(m, faces).size() != 1) {
        error = "Push through: select one connected group of faces";
        return false;
    }
    const Mesh original = m;
    if (p.inset > 0.0f) {
        InsetParams ip;
        ip.thickness = p.inset;
        std::vector<int> inner;
        std::vector<char> tmp;
        if (!insetFaces(m, faces, ip, inner, tmp)) {
            error = "Push through: inset failed";
            m = original;
            return false;
        }
        faces = inner;
    }
    auto borders = regionBorders(m, faces);
    if (borders.size() != 1) {
        error = "Push through: the selection must be a single patch (one border)";
        m = original;
        return false;
    }
    std::vector<int> near = borders[0];  // region winding
    // Region normal / centre.
    Vec3 nrm, centre;
    float area = 0;
    for (int f : faces) {
        Vec3 n = faceNormalRaw(m, m.faces[f]);
        nrm += n;
        float a = length(n);
        centre += faceCenter(m, m.faces[f]) * a;
        area += a;
    }
    if (length(nrm) < 1e-12f || area <= 0) {
        error = "Push through: the selection has no area";
        m = original;
        return false;
    }
    nrm = normalize(nrm);
    centre = centre / area;
    Vec3 lo, hi;
    m.bounds(lo, hi);
    const float scale = std::max(1e-3f, length(hi - lo));
    const float eps = scale * 1e-5f;
    std::vector<char> inRegion(m.faces.size(), 0);
    for (int f : faces) inRegion[f] = 1;
    // Opposite surface along -normal from every border vertex and the centre.
    auto castFar = [&](Vec3 from, int& face, float& t) {
        face = -1;
        t = 1e30f;
        Vec3 o = from - nrm * eps;
        for (int f = 0; f < (int)m.faces.size(); ++f) {
            if (inRegion[f]) continue;
            float th;
            if (rayFace(m, f, o, -nrm, th) && th < t) t = th, face = f;
        }
        return face >= 0;
    };
    int farFace;
    float tc;
    if (!castFar(centre, farFace, tc)) {
        error = "Push through: nothing opposite the selection (the mesh must be closed behind it)";
        m = original;
        return false;
    }
    std::vector<Vec3> hits;
    std::vector<int> hitFaces;
    for (int v : near) {
        int f;
        float t;
        if (!castFar(m.verts[v], f, t)) {
            error = "Push through: part of the selection has nothing behind it";
            m = original;
            return false;
        }
        hits.push_back(m.verts[v] - nrm * (t + eps));
        hitFaces.push_back(f);
    }
    // Case A: the far side already has matching vertices (e.g. inset on both
    // sides) -> remove both patches and bridge.
    {
        std::unordered_map<int, int> match;
        bool all = true;
        std::vector<int> farLoop;
        for (size_t i = 0; i < hits.size() && all; ++i) {
            int best = -1;
            float bestD = scale * 1e-4f;
            for (int v : m.faces[hitFaces[i]]) {
                float d = length(m.verts[v] - hits[i]);
                if (d <= bestD) bestD = d, best = v;
            }
            if (best < 0) all = false;
            else farLoop.push_back(best);
        }
        if (all) {
            // A selection that spans a whole side already has its "tunnel"
            // walls: the border is joined straight to the matching far loop.
            VertexFaces vf0(m);
            for (size_t i = 0; i < near.size(); ++i)
                if (!vf0.owners(near[i], farLoop[i]).empty()) {
                    error = "Push through: the selection is a whole side of the shape; inset it first (Inset > 0)";
                    m = original;
                    return false;
                }
            // The far patch: faces reachable from the centre hit without crossing farLoop.
            std::unordered_set<uint64_t> wall;
            for (size_t i = 0; i < farLoop.size(); ++i) wall.insert(ukey(farLoop[i], farLoop[(i + 1) % farLoop.size()]));
            std::vector<int> patch, stack = {farFace};
            std::vector<std::pair<int, int>> own;
            std::unordered_set<int> seen = {farFace};
            bool leaked = false;
            while (!stack.empty() && !leaked) {
                int f = stack.back();
                stack.pop_back();
                if (inRegion[f]) leaked = true;
                patch.push_back(f);
                const auto& fv = m.faces[f];
                for (size_t i = 0; i < fv.size(); ++i) {
                    uint64_t k = ukey(fv[i], fv[(i + 1) % fv.size()]);
                    if (wall.count(k)) continue;
                    vf0.owners(fv[i], fv[(i + 1) % fv.size()], own);
                    for (const auto& g : own)
                        if (seen.insert(g.first).second) stack.push_back(g.first);
                }
                if (patch.size() > m.faces.size()) leaked = true;
            }
            auto farBorders = leaked ? std::vector<std::vector<int>>() : regionBorders(m, patch);
            if (!leaked && farBorders.size() == 1) {
                std::vector<int> all2 = faces;
                all2.insert(all2.end(), patch.begin(), patch.end());
                std::vector<int> A(near.rbegin(), near.rend()), B(farBorders[0].rbegin(), farBorders[0].rend());
                std::vector<char> kill(m.faces.size(), 0);
                for (int f : all2) kill[f] = 1;
                dropFaces(m, kill);
                BridgeParams bp;
                bridgeOriented(m, A, B, true, false, bp, newFaces);
                finishWithFaces(m, newFaces, vsel);
                return true;
            }
        }
    }
    // Case B: every hit lands inside one flat far face -> cut a matching hole.
    for (int f : hitFaces)
        if (f != farFace) {
            error = "Push through: the opposite side must be one flat face (or a matching inset); inset it first";
            m = original;
            return false;
        }
    const std::vector<int> outer = m.faces[farFace];
    const Vec3 farN = normalize(faceNormalRaw(m, outer));
    // The hole must sit strictly inside the far face (not touch its border).
    for (const Vec3& h : hits)
        for (size_t i = 0; i < outer.size(); ++i) {
            Vec3 a = m.verts[outer[i]], c = m.verts[outer[(i + 1) % outer.size()]];
            Vec3 ab = c - a;
            float t = clampf(dot(h - a, ab) / std::max(1e-20f, dot(ab, ab)), 0.0f, 1.0f);
            if (length(h - (a + ab * t)) < scale * 1e-4f) {
                error = "Push through: the hole would touch the edge of the opposite face; use Inset > 0";
                m = original;
                return false;
            }
        }
    if (dot(farN, nrm) > -0.5f) {
        error = "Push through: the opposite face is not facing away from the selection";
        m = original;
        return false;
    }
    // Hole loop H: the hits, in the order that runs counter-clockwise around
    // farN (the near border runs clockwise around nrm, i.e. CCW around -nrm).
    Builder b(m);
    std::vector<int> H;
    for (size_t i = 0; i < hits.size(); ++i) H.push_back(b.vertexLike(hits[i], near[i]));
    // H follows the near border's winding. Around farN (~ -nrm) the region
    // winding is clockwise, so reverse it to get the CCW order.
    std::reverse(H.begin(), H.end());
    // Star-shaped check around the hole centre, in the far plane.
    Vec3 hc;
    for (int v : H) hc += m.verts[v];
    hc = hc / (float)H.size();
    Vec3 ax = normalize(m.verts[outer[0]] - hc);
    if (length(ax) < 1e-6f) ax = normalize(cross(farN, std::fabs(farN.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0)));
    Vec3 ay = cross(farN, ax);
    auto angleOf = [&](int v) {
        Vec3 d = m.verts[v] - hc;
        float a = std::atan2(dot(d, ay), dot(d, ax));
        return a < 0 ? a + 2 * kPi : a;
    };
    auto starShaped = [&](const std::vector<int>& loop) {
        for (size_t i = 0; i < loop.size(); ++i) {
            Vec3 d0 = m.verts[loop[i]] - hc, d1 = m.verts[loop[(i + 1) % loop.size()]] - hc;
            if (dot(cross(d0, d1), farN) <= 0) return false;
        }
        return true;
    };
    if (!starShaped(outer) || !starShaped(H)) {
        error = "Push through: the opposite face is too irregular around the hole; inset or split it first";
        m = original;
        return false;
    }
    // Annulus between `outer` and H, merged by angle (both CCW around farN).
    auto rotateToMinAngle = [&](std::vector<int>& loop) {
        size_t best = 0;
        for (size_t i = 1; i < loop.size(); ++i)
            if (angleOf(loop[i]) < angleOf(loop[best])) best = i;
        std::rotate(loop.begin(), loop.begin() + best, loop.end());
    };
    std::vector<int> O = outer;
    rotateToMinAngle(O);
    std::vector<int> Hs = H;
    rotateToMinAngle(Hs);
    // UVs of the far face: planar map from its own UVs where available.
    std::vector<Vec2> outerUV(outer.size());
    for (size_t i = 0; i < outer.size(); ++i) outerUV[i] = b.uv(farFace, (int)i);
    auto uvAt = [&](Vec3 pos) {
        // Barycentric-free: inverse-distance blend of the outer corners' UVs
        // (exact at the corners, smooth inside; good enough for a hole cut).
        Vec2 acc;
        float wsum = 0;
        for (size_t i = 0; i < outer.size(); ++i) {
            float d = length(pos - m.verts[outer[i]]);
            if (d < 1e-9f) return outerUV[i];
            float w = 1.0f / (d * d);
            acc = acc + outerUV[i] * w;
            wsum += w;
        }
        return acc / wsum;
    };
    {
        const size_t no = O.size(), nh = Hs.size();
        auto ang = [&](const std::vector<int>& L, size_t i) {
            float a = angleOf(L[i % L.size()]);
            if (i >= L.size()) a += 2 * kPi;
            return a;
        };
        size_t i = 0, j = 0;
        std::vector<std::vector<int>> fs;
        while (i < no || j < nh) {
            float nextO = i < no ? ang(O, i + 1) : 1e9f, nextH = j < nh ? ang(Hs, j + 1) : 1e9f;
            int o0 = O[i % no], o1 = O[(i + 1) % no], h0 = Hs[j % nh], h1 = Hs[(j + 1) % nh];
            if (std::fabs(nextO - nextH) < 1e-4f && i < no && j < nh) {
                fs.push_back({o0, o1, h1, h0});
                ++i, ++j;
            } else if (nextO < nextH) {
                fs.push_back({o0, o1, h0});
                ++i;
            } else {
                fs.push_back({o0, h1, h0});
                ++j;
            }
        }
        // Replace the far face (keep the index stable: overwrite it with the first piece).
        for (size_t k = 0; k < fs.size(); ++k) {
            std::vector<Vec2> u;
            for (int v : fs[k]) u.push_back(uvAt(m.verts[v]));
            if (k == 0) b.setFace(farFace, fs[k], u);
            else b.face(fs[k], u);
        }
    }
    // Tunnel: remove the near patch and bridge its border to the hole.
    std::vector<char> kill(m.faces.size(), 0);
    for (int f : faces) kill[f] = 1;
    // Faces after the near patch shift down when it is removed: remember the
    // annulus by vertex content instead of index (only the selection needs it).
    dropFaces(m, kill);
    std::vector<int> A(near.rbegin(), near.rend());  // surrounding faces run A[i] -> A[i+1]
    // The annulus faces run H in the hole's clockwise direction: h1 -> h0 with H CCW.
    std::vector<int> B(H.rbegin(), H.rend());
    BridgeParams bp;
    bridgeOriented(m, A, B, true, false, bp, newFaces);
    finishWithFaces(m, newFaces, vsel);
    return true;
}

// ============================================================================
// Merge / check
// ============================================================================
int mergeVertices(Mesh& m, const std::vector<char>& vselIn, bool perGroup) {
    const int nv = (int)m.verts.size();
    if ((int)vselIn.size() != nv) return 0;
    std::vector<int> parent(nv);
    for (int v = 0; v < nv; ++v) parent[v] = v;
    auto findRoot = [&](int v) {
        while (parent[v] != v) v = parent[v] = parent[parent[v]];
        return v;
    };
    int first = -1, count = 0;
    for (int v = 0; v < nv; ++v)
        if (vselIn[v]) {
            ++count;
            if (first < 0) first = v;
            else if (!perGroup) parent[findRoot(v)] = findRoot(first);
        }
    if (count < 2) return 0;
    if (perGroup)
        for (const auto& f : m.faces)
            for (size_t i = 0; i < f.size(); ++i) {
                int a = f[i], c = f[(i + 1) % f.size()];
                if (vselIn[a] && vselIn[c]) parent[findRoot(a)] = findRoot(c);
            }
    // Centres.
    std::unordered_map<int, std::pair<Vec3, int>> sum;
    for (int v = 0; v < nv; ++v)
        if (vselIn[v]) {
            auto& s = sum[findRoot(v)];
            s.first += m.verts[v];
            s.second++;
        }
    int removed = 0;
    for (auto& kv : sum) {
        m.verts[kv.first] = kv.second.first / (float)kv.second.second;
        removed += kv.second.second - 1;
    }
    const bool uvs = m.hasUVs();
    std::vector<char> kill(m.faces.size(), 0);
    for (size_t f = 0; f < m.faces.size(); ++f) {
        bool hit = false;
        for (int v : m.faces[f]) hit = hit || vselIn[v];
        if (!hit) continue;
        std::vector<int> nf;
        std::vector<Vec2> nu;
        const auto& face = m.faces[f];
        for (size_t i = 0; i < face.size(); ++i) {
            int r = vselIn[face[i]] ? findRoot(face[i]) : face[i];
            if (!nf.empty() && nf.back() == r) continue;
            nf.push_back(r);
            if (uvs) nu.push_back(m.uvs[f][i]);
        }
        while (nf.size() > 1 && nf.front() == nf.back()) {
            nf.pop_back();
            if (uvs) nu.pop_back();
        }
        std::unordered_set<int> distinct(nf.begin(), nf.end());
        if (nf.size() < 3 || distinct.size() != nf.size()) {
            kill[f] = 1;
            continue;
        }
        m.faces[f] = nf;
        if (uvs) m.uvs[f] = nu;
    }
    dropFaces(m, kill);
    removeUnusedVertices(m);
    m.validate();
    m.touch();
    return removed;
}

void appendMesh(Mesh& dst, const Mesh& src, const Mat4& xf) {
    const bool uvs = dst.hasUVs() || src.hasUVs() || (dst.faces.empty() && src.hasUVs());
    const bool weights = (dst.verts.empty() || dst.hasWeights()) && src.hasWeights();
    if (uvs && !dst.hasUVs()) {
        dst.uvs.resize(dst.faces.size());
        for (size_t f = 0; f < dst.faces.size(); ++f) dst.uvs[f].assign(dst.faces[f].size(), Vec2());
    }
    if (!weights) dst.weights.clear();
    const int base = (int)dst.verts.size();
    for (const Vec3& p : src.verts) dst.verts.push_back(transformPoint(xf, p));
    if (weights) dst.weights.insert(dst.weights.end(), src.weights.begin(), src.weights.end());
    for (size_t f = 0; f < src.faces.size(); ++f) {
        std::vector<int> face = src.faces[f];
        for (int& v : face) v += base;
        dst.faces.push_back(std::move(face));
        if (uvs) dst.uvs.push_back(src.hasUVs() ? src.uvs[f] : std::vector<Vec2>(src.faces[f].size()));
    }
    // A mirrored transform flips the winding: flip the copied faces back.
    const float det = xf(0, 0) * (xf(1, 1) * xf(2, 2) - xf(1, 2) * xf(2, 1)) -
                      xf(0, 1) * (xf(1, 0) * xf(2, 2) - xf(1, 2) * xf(2, 0)) +
                      xf(0, 2) * (xf(1, 0) * xf(2, 1) - xf(1, 1) * xf(2, 0));
    if (det < 0)
        for (size_t f = dst.faces.size() - src.faces.size(); f < dst.faces.size(); ++f) {
            std::reverse(dst.faces[f].begin(), dst.faces[f].end());
            if (uvs) std::reverse(dst.uvs[f].begin(), dst.uvs[f].end());
        }
    dst.validate();
    dst.touch();
}

std::string checkMesh(const Mesh& m) {
    const int nv = (int)m.verts.size();
    for (int v = 0; v < nv; ++v) {
        const Vec3& p = m.verts[v];
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            return "vertex " + std::to_string(v) + " is not finite";
    }
    if (!m.uvs.empty() && m.uvs.size() != m.faces.size()) return "uv array out of step with faces";
    if (!m.weights.empty() && m.weights.size() != m.verts.size()) return "weight array out of step with vertices";
    for (size_t f = 0; f < m.faces.size(); ++f) {
        const auto& face = m.faces[f];
        if (face.size() < 3) return "face " + std::to_string(f) + " has fewer than 3 corners";
        if (!m.uvs.empty() && m.uvs[f].size() != face.size()) return "face " + std::to_string(f) + " uv count mismatch";
        for (size_t i = 0; i < face.size(); ++i) {
            if (face[i] < 0 || face[i] >= nv) return "face " + std::to_string(f) + " has an out-of-range index";
            if (face[i] == face[(i + 1) % face.size()]) return "face " + std::to_string(f) + " repeats a corner";
        }
    }
    return "";
}

}  // namespace meshedit
