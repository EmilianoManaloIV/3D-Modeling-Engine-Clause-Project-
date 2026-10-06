#pragma once
// Edit-mode topology tools that work on explicit edge / face selections:
// edge extrusion and face inset. (Face extrusion lives in mesh.h.)
//
// Inset (as in Blender / 3ds Max): each selected region gets a new ring of
// faces inside its border, the border vertices moving inwards along the
// angle bisector of their two border edges, so the ring has a constant width
// (`thickness`). `depth` then pushes the inset region along its normal
// (> 0 outwards = raised, < 0 inwards = recessed), and `dish` pokes a centre
// vertex into every inner face, offset along the face normal, which makes the
// face concave (< 0) or convex (> 0). `individual` insets every face on its
// own instead of the selection as connected regions.
#include "mesh.h"

#include <utility>
#include <vector>

namespace meshedit {

using Edge = std::pair<int, int>;  // always stored with first < second

// Vertex -> faces in compressed-row form: two flat arrays built in one pass
// (no hashing, no per-edge allocations). The faces of an edge are found by
// scanning the short face list of one endpoint, so tools that only touch a
// few elements stay cheap on million-face meshes.
struct VertexFaces {
    const Mesh& m;
    std::vector<int> start, list;
    explicit VertexFaces(const Mesh& mesh) : m(mesh) {
        const size_t nv = m.verts.size();
        start.assign(nv + 1, 0);
        for (const auto& f : m.faces)
            for (int v : f)
                if (v >= 0 && v < (int)nv) ++start[v + 1];
        for (size_t v = 0; v < nv; ++v) start[v + 1] += start[v];
        list.resize(start.back());
        std::vector<int> fill(start.begin(), start.end() - 1);
        for (int f = 0; f < (int)m.faces.size(); ++f)
            for (int v : m.faces[f])
                if (v >= 0 && v < (int)nv) list[fill[v]++] = f;
    }
    bool valid(int v) const { return v >= 0 && v + 1 < (int)start.size(); }
    // Every (face, corner) whose edge face[corner] -> face[corner + 1] joins a and b.
    void owners(int a, int b, std::vector<std::pair<int, int>>& out) const {
        out.clear();
        if (!valid(a) || !valid(b)) return;
        for (int k = start[a]; k < start[a + 1]; ++k) {
            const int f = list[k];
            if (k > start[a] && list[k - 1] == f) continue;  // face lists a vertex twice
            const auto& fv = m.faces[f];
            const size_t n = fv.size();
            for (size_t i = 0; i < n; ++i) {
                const int x = fv[i], y = fv[(i + 1) % n];
                if ((x == a && y == b) || (x == b && y == a)) out.push_back({f, (int)i});
            }
        }
    }
    std::vector<std::pair<int, int>> owners(int a, int b) const {
        std::vector<std::pair<int, int>> out;
        owners(a, b, out);
        return out;
    }
    // (face, corner) of the directed edge a -> b, or false.
    bool directed(int a, int b, std::pair<int, int>* out = nullptr) const {
        if (!valid(a)) return false;
        for (int k = start[a]; k < start[a + 1]; ++k) {
            const auto& fv = m.faces[list[k]];
            const size_t n = fv.size();
            for (size_t i = 0; i < n; ++i)
                if (fv[i] == a && fv[(i + 1) % n] == b) {
                    if (out) *out = {list[k], (int)i};
                    return true;
                }
        }
        return false;
    }
};

Edge makeEdge(int a, int b);

// Corners of a face selection grouped into "fans": corners at the same vertex
// that are joined through edges shared inside the selection. A vertex where
// the selection only touches itself (a "pinch", e.g. two faces meeting at a
// cone's tip, or a ring of faces around a hole) gets one fan per side, so
// region tools can give each side its own copy of the vertex and keep the
// mesh manifold (found by the stress test's fuzzer).
struct CornerFans {
    std::vector<int> faces;      // the selection, without duplicates / invalid indices
    std::vector<int> faceStart;  // per selected face: id of its first corner
    std::vector<int> fan;        // corner id -> fan id (0 .. count-1)
    std::vector<int> fanVertex;  // fan id -> vertex
    int count = 0;
    int of(int listIndex, int corner) const { return fan[faceStart[listIndex] + corner]; }
};
CornerFans cornerFans(const Mesh& m, const std::vector<int>& faces);
// Edges whose two end vertices are selected.
std::vector<Edge> edgesFromVerts(const Mesh& m, const std::vector<char>& vsel);
// Vertex selection covering the given edges / faces.
std::vector<char> vertsFromEdges(const Mesh& m, const std::vector<Edge>& edges);
std::vector<char> vertsFromFaces(const Mesh& m, const std::vector<char>& fsel);
// Faces whose every vertex is selected.
std::vector<char> facesFromVerts(const Mesh& m, const std::vector<char>& vsel);

// Each edge gets a new quad attached along it, built from copies of the
// edge's vertices (shared where selected edges meet). The quad continues
// the orientation of the face that owns the edge. `newEdges` receives the
// copied edges (the ones to move next) and `vsel` selects their vertices.
bool extrudeEdges(Mesh& m, const std::vector<Edge>& edges, std::vector<Edge>& newEdges, std::vector<char>& vsel);

struct InsetParams {
    float thickness = 0.1f;
    float depth = 0.0f;
    float dish = 0.0f;
    bool individual = false;
};
// `innerFaces` receives the inset faces (the new selection), `vsel` their vertices.
bool insetFaces(Mesh& m, const std::vector<int>& faces, const InsetParams& p, std::vector<int>& innerFaces,
                std::vector<char>& vsel);

}  // namespace meshedit
