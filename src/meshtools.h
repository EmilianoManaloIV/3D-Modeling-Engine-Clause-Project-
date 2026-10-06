#pragma once
// More edit-mode modeling tools: ways to add vertices and faces, and the
// "smart" region tools. All of them keep UVs (per corner) and bone weights,
// keep a closed, consistently oriented mesh closed and consistently
// oriented, and report the new elements so the editor can select them.
//
// Adding vertices
//   subdivideEdges   split edges into equal pieces; with `connect`, faces with
//                    two cut edges are split across (a loop cut), and faces
//                    whose every edge is cut once become quads around a centre
//   edgeRing / Loop  the ring of opposite edges across quads / the loop that
//                    runs straight through 4-valent vertices (selection helpers)
//   connectVertices  split faces along a chord between selected corners
//   pokeFaces        centre vertex + triangle fan
//
// Adding faces
//   bevel            edges (or vertices) are replaced by a strip of `segments`
//                    faces; `profile` shapes it from flat (0) via round (0.5)
//                    to the original corner (1). Corners where several bevels
//                    meet are closed with a new face.
//   bridgeLoops      connect two edge loops / chains with a tube of faces
//   bridgeRegions    remove two face regions and bridge their borders (a tunnel
//                    between two pieces of geometry)
//   fillHoles        make a face from a closed (or open) boundary chain
//   pushThrough      punch the selected faces through the mesh to the opposite
//                    side: the far face gets a matching hole and a tunnel joins
//                    them ("inverse extrusion" through a wall)
//
// Removing
//   mergeVertices    collapse to the centre (all, or per connected group)
//   deleteFaces      remove faces (and the vertices only they used)
#include "meshedit.h"

#include <string>
#include <vector>

namespace meshedit {

// --- adding vertices ---------------------------------------------------------
bool subdivideEdges(Mesh& m, const std::vector<Edge>& edges, int cuts, bool connect, std::vector<char>& vsel);
std::vector<Edge> edgeRing(const Mesh& m, Edge start);
std::vector<Edge> edgeLoop(const Mesh& m, Edge start);
// A loop cut: subdivideEdges(edgeRing(edge), cuts, connect). `vsel` = the new loop(s).
bool loopCut(Mesh& m, Edge edge, int cuts, std::vector<char>& vsel);
// Returns the number of faces split.
int connectVertices(Mesh& m, const std::vector<char>& vsel);
bool pokeFaces(Mesh& m, const std::vector<int>& faces, float offset, std::vector<int>& newFaces,
               std::vector<char>& vsel);

// --- adding faces ------------------------------------------------------------
struct BevelParams {
    float width = 0.1f;   // distance the cut moves along each neighbouring edge
    int segments = 1;     // faces across the bevel (1 = chamfer)
    float profile = 0.5f; // 0 flat, 0.5 round, 1 back to the original corner
    bool clamp = true;    // limit the width so neighbouring cuts don't overlap
};
// Bevels `edges` (edges need a face on each side), or, when `edges` is empty,
// the vertices in `verts` (every edge of a beveled vertex is cut).
bool bevel(Mesh& m, const std::vector<Edge>& edges, const std::vector<int>& verts, const BevelParams& p,
           std::vector<int>& newFaces, std::vector<char>& vsel);

struct BridgeParams {
    int segments = 1;  // rings of faces along the bridge
    int twist = 0;     // extra rotation of the pairing (closed loops), in vertices
};
// The selected edges must form exactly two chains (both closed or both open).
bool bridgeLoops(Mesh& m, const std::vector<Edge>& edges, const BridgeParams& p, std::vector<int>& newFaces,
                 std::vector<char>& vsel, std::string& error);
// The selected faces must form exactly two regions, each with one border.
bool bridgeRegions(Mesh& m, const std::vector<int>& faces, const BridgeParams& p, std::vector<int>& newFaces,
                   std::vector<char>& vsel, std::string& error);
// Each closed chain of boundary edges becomes a face (an open chain of 3+
// vertices is closed first). Returns the number of faces made.
int fillHoles(Mesh& m, const std::vector<Edge>& edges, std::vector<int>& newFaces, std::string& error);

struct PushThroughParams {
    float inset = 0.0f;  // inset the selection first (> 0 leaves a frame around the hole)
};
bool pushThrough(Mesh& m, const std::vector<int>& faces, const PushThroughParams& p, std::vector<int>& newFaces,
                 std::vector<char>& vsel, std::string& error);

// --- removing ----------------------------------------------------------------
// Collapses the selected vertices to their centre: all into one vertex, or
// each connected group (along selected edges) into its own. Faces that become
// degenerate are removed. Returns the number of vertices removed.
int mergeVertices(Mesh& m, const std::vector<char>& vsel, bool perGroup);
void deleteFaces(Mesh& m, const std::vector<char>& kill);
// Appends `src` (transformed) to `dst` (Join). UVs are kept when either mesh
// has them (missing ones become 0); bone weights only when both have them.
void appendMesh(Mesh& dst, const Mesh& src, const Mat4& transform);

// --- helpers (also used by the editor and the tests) -------------------------
// Border loops of a face region, in the region's own winding direction.
std::vector<std::vector<int>> regionBorders(const Mesh& m, const std::vector<int>& faces);
// Connected groups of faces (sharing an edge).
std::vector<std::vector<int>> faceRegions(const Mesh& m, const std::vector<int>& faces);
// Structural check used after every tool in the stress test: indices in
// range, faces of 3+ distinct consecutive corners, attribute arrays in sync,
// finite positions. Returns "" or a description of the first problem.
std::string checkMesh(const Mesh& m);

}  // namespace meshedit
