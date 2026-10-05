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

}  // namespace meshedit
