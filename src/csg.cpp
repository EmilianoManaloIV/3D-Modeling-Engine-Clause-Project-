#include "csg.h"

#include "polygon.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace csg {
namespace {

struct Vert {
    Vec3 p;
    Vec2 uv;
};

struct Poly {
    std::vector<Vert> v;
    Vec3 n;     // plane: dot(n, x) = w
    float w = 0;
};

struct Node {
    bool hasPlane = false;
    Vec3 n;
    float w = 0;
    std::vector<Poly> polys;
    int front = -1, back = -1;
};

struct Tree {
    std::vector<Node> nodes;
};

float g_eps = 1e-5f;

void computePlane(Poly& poly) {
    Vec3 nrm, c;
    const size_t k = poly.v.size();
    for (size_t i = 0; i < k; ++i) {
        const Vec3 &a = poly.v[i].p, &b = poly.v[(i + 1) % k].p;
        nrm.x += (a.y - b.y) * (a.z + b.z);
        nrm.y += (a.z - b.z) * (a.x + b.x);
        nrm.z += (a.x - b.x) * (a.y + b.y);
        c += a;
    }
    poly.n = normalize(nrm);
    poly.w = dot(poly.n, c / float(k));
}

void flip(Poly& poly) {
    std::reverse(poly.v.begin(), poly.v.end());
    poly.n = -poly.n;
    poly.w = -poly.w;
}

enum { COPLANAR = 0, FRONT = 1, BACK = 2, SPANNING = 3 };

// Splits `poly` by the plane (n, w) into the four output lists.
void splitPolygon(Vec3 n, float w, const Poly& poly, std::vector<Poly>& coFront, std::vector<Poly>& coBack,
                  std::vector<Poly>& front, std::vector<Poly>& back) {
    int polyType = 0;
    const size_t k = poly.v.size();
    std::vector<int> types(k);
    for (size_t i = 0; i < k; ++i) {
        float t = dot(n, poly.v[i].p) - w;
        int type = t < -g_eps ? BACK : (t > g_eps ? FRONT : COPLANAR);
        polyType |= type;
        types[i] = type;
    }
    switch (polyType) {
        case COPLANAR:
            (dot(n, poly.n) > 0 ? coFront : coBack).push_back(poly);
            break;
        case FRONT: front.push_back(poly); break;
        case BACK: back.push_back(poly); break;
        default: {
            Poly f, b;
            for (size_t i = 0; i < k; ++i) {
                size_t j = (i + 1) % k;
                int ti = types[i], tj = types[j];
                const Vert &vi = poly.v[i], &vj = poly.v[j];
                if (ti != BACK) f.v.push_back(vi);
                if (ti != FRONT) b.v.push_back(vi);
                if ((ti | tj) == SPANNING) {
                    float t = (w - dot(n, vi.p)) / dot(n, vj.p - vi.p);
                    Vert x{vi.p + (vj.p - vi.p) * t, vi.uv + (vj.uv - vi.uv) * t};
                    f.v.push_back(x);
                    b.v.push_back(x);
                }
            }
            // Pieces keep the parent's plane (avoids drift from re-fitting).
            if (f.v.size() >= 3) {
                f.n = poly.n;
                f.w = poly.w;
                front.push_back(std::move(f));
            }
            if (b.v.size() >= 3) {
                b.n = poly.n;
                b.w = poly.w;
                back.push_back(std::move(b));
            }
        }
    }
}

int newNode(Tree& t) {
    t.nodes.emplace_back();
    return (int)t.nodes.size() - 1;
}

// Adds polygons to the tree rooted at node 0 (iterative to avoid deep recursion).
void build(Tree& t, std::vector<Poly> polys) {
    if (polys.empty()) return;
    if (t.nodes.empty()) newNode(t);
    std::vector<std::pair<int, std::vector<Poly>>> stack;
    stack.push_back({0, std::move(polys)});
    while (!stack.empty()) {
        auto [ni, list] = std::move(stack.back());
        stack.pop_back();
        if (list.empty()) continue;
        if (!t.nodes[ni].hasPlane) {
            t.nodes[ni].hasPlane = true;
            t.nodes[ni].n = list[0].n;
            t.nodes[ni].w = list[0].w;
        }
        std::vector<Poly> front, back;
        const Vec3 n = t.nodes[ni].n;
        const float w = t.nodes[ni].w;
        for (const Poly& p : list) {
            std::vector<Poly>& co = t.nodes[ni].polys;
            // A polygon whose stored plane IS this node's plane belongs here,
            // whatever its vertices say: split pieces keep their parent's
            // plane, and rounding can put a piece's vertices just outside it.
            // Re-classifying it sent it to a child that took the same plane
            // from it again - an endless descent (found by the stress test's
            // fuzzer: a boolean ran out of memory). This guarantees progress.
            if (p.n.x == n.x && p.n.y == n.y && p.n.z == n.z && p.w == w) {
                co.push_back(p);
                continue;
            }
            splitPolygon(n, w, p, co, co, front, back);
        }
        if (!front.empty()) {
            if (t.nodes[ni].front < 0) {
                int f = newNode(t);
                t.nodes[ni].front = f;
            }
            stack.push_back({t.nodes[ni].front, std::move(front)});
        }
        if (!back.empty()) {
            if (t.nodes[ni].back < 0) {
                int b = newNode(t);
                t.nodes[ni].back = b;
            }
            stack.push_back({t.nodes[ni].back, std::move(back)});
        }
    }
}

void invert(Tree& t) {
    for (Node& nd : t.nodes) {
        for (Poly& p : nd.polys) flip(p);
        nd.n = -nd.n;
        nd.w = -nd.w;
        std::swap(nd.front, nd.back);
    }
}

// Removes the parts of `polys` that are inside the solid `t`.
std::vector<Poly> clipPolygons(const Tree& t, std::vector<Poly> polys) {
    std::vector<Poly> result;
    if (t.nodes.empty()) return polys;
    std::vector<std::pair<int, std::vector<Poly>>> stack;
    stack.push_back({0, std::move(polys)});
    while (!stack.empty()) {
        auto [ni, list] = std::move(stack.back());
        stack.pop_back();
        const Node& nd = t.nodes[ni];
        if (!nd.hasPlane) {
            for (Poly& p : list) result.push_back(std::move(p));
            continue;
        }
        std::vector<Poly> front, back;
        for (const Poly& p : list) splitPolygon(nd.n, nd.w, p, front, back, front, back);
        if (nd.front >= 0) stack.push_back({nd.front, std::move(front)});
        else
            for (Poly& p : front) result.push_back(std::move(p));
        if (nd.back >= 0) stack.push_back({nd.back, std::move(back)});
        // else: behind a leaf = inside the solid -> discarded
    }
    return result;
}

void clipTo(Tree& a, const Tree& b) {
    for (Node& nd : a.nodes) nd.polys = clipPolygons(b, std::move(nd.polys));
}

std::vector<Poly> allPolygons(const Tree& t) {
    std::vector<Poly> out;
    for (const Node& nd : t.nodes) out.insert(out.end(), nd.polys.begin(), nd.polys.end());
    return out;
}

// Mesh -> convex planar polygons (non-convex or warped faces are triangulated).
std::vector<Poly> toPolygons(const Mesh& m) {
    std::vector<Poly> out;
    const bool uvs = m.hasUVs();
    std::vector<int> corners;
    for (size_t f = 0; f < m.faces.size(); ++f) {
        const auto& face = m.faces[f];
        if (face.size() < 3) continue;
        Poly whole;
        for (size_t i = 0; i < face.size(); ++i) whole.v.push_back({m.verts[face[i]], uvs ? m.uvs[f][i] : Vec2()});
        computePlane(whole);
        bool planar = length(whole.n) > 0.5f;
        for (const Vert& v : whole.v) planar = planar && std::fabs(dot(whole.n, v.p) - whole.w) <= g_eps;
        if (face.size() == 3 || (planar && poly::isConvex(m.verts, face))) {
            if (length(whole.n) > 0.5f) out.push_back(std::move(whole));
            continue;
        }
        corners.clear();
        poly::triangulate(m.verts, face, corners);
        for (size_t t = 0; t + 2 < corners.size(); t += 3) {
            Poly tri;
            for (int k = 0; k < 3; ++k) tri.v.push_back(whole.v[corners[t + k]]);
            computePlane(tri);
            if (length(tri.n) > 0.5f) out.push_back(std::move(tri));
        }
    }
    return out;
}

}  // namespace

const char* opName(Op op) {
    switch (op) {
        case Op::Union: return "Union";
        case Op::Difference: return "Difference";
        default: return "Intersection";
    }
}

Result apply(const Mesh& meshA, const Mesh& meshB, Op op) {
    Result r;
    Vec3 lo, hi, lo2, hi2;
    meshA.bounds(lo, hi);
    meshB.bounds(lo2, hi2);
    g_eps = std::max(1e-7f, length(vmax(hi, hi2) - vmin(lo, lo2)) * 1e-6f);

    std::vector<Poly> pa = toPolygons(meshA), pb = toPolygons(meshB);
    r.inputPolygons = (int)(pa.size() + pb.size());
    Tree a, b;
    build(a, std::move(pa));
    build(b, std::move(pb));
    switch (op) {
        case Op::Union:
            clipTo(a, b);
            clipTo(b, a);
            invert(b);
            clipTo(b, a);
            invert(b);
            build(a, allPolygons(b));
            break;
        case Op::Difference:
            invert(a);
            clipTo(a, b);
            clipTo(b, a);
            invert(b);
            clipTo(b, a);
            invert(b);
            build(a, allPolygons(b));
            invert(a);
            break;
        case Op::Intersection:
            invert(a);
            clipTo(b, a);
            invert(b);
            clipTo(a, b);
            clipTo(b, a);
            build(a, allPolygons(b));
            invert(a);
            break;
    }
    std::vector<Poly> polys = allPolygons(a);
    r.outputPolygons = (int)polys.size();
    Mesh& m = r.mesh;
    for (const Poly& p : polys) {
        std::vector<int> face;
        std::vector<Vec2> fu;
        for (const Vert& v : p.v) {
            face.push_back((int)m.verts.size());
            m.verts.push_back(v.p);
            fu.push_back(v.uv);
        }
        m.faces.push_back(std::move(face));
        m.uvs.push_back(std::move(fu));
    }
    if (!meshA.hasUVs() && !meshB.hasUVs()) m.uvs.clear();
    m.validate();
    m.touch();
    return r;
}

}  // namespace csg
