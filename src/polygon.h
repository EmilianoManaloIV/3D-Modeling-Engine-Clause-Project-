#pragma once
// N-gon solver: robust polygon triangulation and mesh clean-up.
//
// Triangulation is ear clipping (the classic method for simple polygons,
// cf. FoCG 5e sec. 12.1 on triangle meshes): the polygon is projected onto
// the plane of its Newell normal, then convex "ears" containing no other
// vertex are cut off one by one. It handles concave polygons, collinear
// vertices (as left by T-junction repair or booleans) and slightly
// non-planar faces; if a polygon is self-intersecting it falls back to the
// best remaining ear so it always terminates.
#include "mesh.h"

#include <vector>

namespace poly {

// Triangles as triples of *corner indices* (0..face.size()-1), so per-corner
// attributes such as UVs can be carried along. Returns false if the fallback
// was needed (self-intersecting / degenerate input).
bool triangulate(const std::vector<Vec3>& positions, const std::vector<int>& face, std::vector<int>& outCorners);
bool isConvex(const std::vector<Vec3>& positions, const std::vector<int>& face);

struct CleanupStats {
    int weldedVertices = 0;
    int tJunctionsFixed = 0;
    int degenerateFaces = 0;
};
// Weld vertices closer than `eps` (0 = automatic, relative to the mesh size),
// repair T-junctions (a vertex lying on another face's edge is inserted into
// that edge, which removes cracks), and drop degenerate faces.
CleanupStats cleanup(Mesh& m, float eps = 0.0f);

// Replaces every face with at least `minSides` corners (4 = all non-triangles,
// 5 = only n-gons) by triangles. Keeps UVs. Returns the number of faces split.
int triangulateMesh(Mesh& m, int minSides);

// Greedily merges pairs of nearly coplanar triangles into convex quads.
// Returns the number of quads made.
int trisToQuads(Mesh& m, float maxAngleDeg = 10.0f);

}  // namespace poly
