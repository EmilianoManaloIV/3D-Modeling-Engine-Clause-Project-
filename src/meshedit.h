#pragma once
// Edit-mode topology tools that work on explicit vertex / edge / face
// selections (face extrusion itself lives in mesh.h):
//   - more vertices: edge subdivision, loop cuts (edge rings), connecting
//     vertices across a face, poking faces;
//   - more faces: edge extrusion, inset, bevel (edges or vertices), bridging
//     two edge loops / two face regions, fill, push-in and punch-through
//     (inset + inverse extrusion, optionally a Boolean hole through the mesh);
//   - clean-up: merge vertices at their centre, join meshes.
// Every tool keeps per-corner UVs and bone weights where it can.
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

#include <string>
#include <utility>
#include <vector>

namespace meshedit {

using Edge = std::pair<int, int>;  // always stored with first < second

Edge makeEdge(int a, int b);
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

// --- Selection helpers -------------------------------------------------------
// Edge loop through `seed`: continues straight through every valence-4 vertex
// surrounded by quads (and along open borders). Includes the seed.
std::vector<Edge> edgeLoop(const Mesh& m, Edge seed);
// Edge ring through `seed`: the opposite edges of consecutive quads.
std::vector<Edge> edgeRing(const Mesh& m, Edge seed);
// Removes the flagged faces (and vertices no other face uses).
// Returns the old -> new vertex index map (-1 = removed).
std::vector<int> deleteFaces(Mesh& m, const std::vector<char>& kill);

// --- More vertices -------------------------------------------------------------
// Splits every edge into `cuts` + 1 pieces; faces using the edge get the new
// vertices (quads become n-gons). `newEdges` = the pieces, `vsel` = new vertices.
bool subdivideEdges(Mesh& m, const std::vector<Edge>& edges, int cuts, std::vector<Edge>& newEdges,
                    std::vector<char>& vsel);

struct LoopCutParams {
    int cuts = 1;
    float slide = 0.0f;  // -1..1, moves a single cut towards either side of the ring
};
// Cuts new edge loops across the quad ring of each seed edge (Blender's Ctrl+R).
// Non-quad faces at the ends of the ring get the new vertices on their edge.
bool loopCut(Mesh& m, const std::vector<Edge>& seeds, const LoopCutParams& p, std::vector<Edge>& newEdges,
             std::vector<char>& vsel);

// Splits each face that has exactly two selected, non-adjacent corners along
// the line between them. Returns the number of faces split.
int connectVertices(Mesh& m, const std::vector<char>& vsel, std::vector<Edge>& newEdges);

// Adds a centre vertex to each face (moved `offset` along its normal) and
// fans the face into triangles. Returns the number of faces poked.
int pokeFaces(Mesh& m, const std::vector<int>& faces, float offset, std::vector<int>& newFaces,
              std::vector<char>& vsel);

// --- More faces --------------------------------------------------------------------
struct BevelParams {
    float width = 0.1f;
    int segments = 1;          // > 1 rounds the bevel
    bool vertexOnly = false;   // chamfer the selected vertices instead of edges
};
// Bevels `edges` (manifold edges only), or with vertexOnly the vertices in
// `verts`. New strip / corner faces go to `newFaces`. The width is clamped so
// no edge collapses; `usedWidth` receives the width actually used.
bool bevel(Mesh& m, const std::vector<Edge>& edges, const std::vector<char>& verts, const BevelParams& p,
           std::vector<int>& newFaces, std::vector<char>& vsel, float* usedWidth = nullptr);

struct BridgeParams {
    int segments = 1;  // rings of quads along the bridge
    int twist = 0;     // extra rotation of the second loop, in vertices
};
// Connects two open edge loops (border edges) with a tube of faces. Loops of
// different sizes are joined with triangles where needed.
bool bridgeEdges(Mesh& m, const std::vector<Edge>& edges, const BridgeParams& p, std::vector<int>& newFaces,
                 std::vector<char>& vsel, std::string& error);
// Removes two separate face regions and bridges the holes they leave
// (e.g. two facing caps become a tunnel or a handle).
bool bridgeFaces(Mesh& m, const std::vector<int>& faces, const BridgeParams& p, std::vector<int>& newFaces,
                 std::vector<char>& vsel, std::string& error);

// Fills a closed loop of border edges with one face, or makes a face from the
// selected vertices (ordered around their centre) when they share no edges.
bool fillFace(Mesh& m, const std::vector<Edge>& edges, const std::vector<char>& verts, std::vector<int>& newFaces,
              std::vector<char>& vsel, std::string& error);

struct PushParams {
    float width = 0.1f;   // inset border width (0 = no border)
    float depth = 0.3f;   // inverse extrusion depth, along -normal
    bool through = false; // punch a hole through the whole mesh (Boolean)
};
// Inset + inverse extrusion: recesses the selected region into the mesh, with
// side walls (a pocket, a panel line, a window). With `through`, the inset
// region is cut through the mesh and out the other side instead.
bool pushIn(Mesh& m, const std::vector<int>& faces, const PushParams& p, std::vector<int>& innerFaces,
            std::vector<char>& vsel, std::string& error);

// The "through" part of pushIn: cuts the region (whose faces face +n) as a
// hole straight through the mesh along -n, with walls.
bool punchThrough(Mesh& m, const std::vector<int>& region, Vec3 n, std::vector<char>& vsel, std::string& error);

// --- Clean-up --------------------------------------------------------------------
// Collapses the selected vertices into one at their centre. Faces that become
// degenerate are removed. Returns the number of vertices removed.
int mergeAtCenter(Mesh& m, std::vector<char>& vsel);
// Appends `src` (transformed by `xf`) to `dst`. UVs are kept if either mesh
// has them (missing ones become 0,0); bone weights only if both have them.
void appendMesh(Mesh& dst, const Mesh& src, const Mat4& xf);

}  // namespace meshedit
