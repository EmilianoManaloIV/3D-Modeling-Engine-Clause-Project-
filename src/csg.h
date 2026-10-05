#pragma once
// Boolean (constructive solid geometry) operations on closed meshes using
// binary space partitioning trees (FoCG 5e sec. 12.4, "BSP Trees"; the
// algorithm popularised by csg.js):
//   - each solid is turned into a BSP tree of its (convex, planar) polygons;
//   - clipping one tree against the other removes the parts of each surface
//     that lie inside / outside the other solid;
//   - inverting a tree turns a solid inside out (for difference/intersection).
// The result is a soup of convex polygons; the n-gon solver (polygon.h)
// welds it, repairs the T-junctions the splitting creates and optionally
// triangulates it back into clean triangles.
#include "mesh.h"

#include <string>

namespace csg {

enum class Op { Union, Difference, Intersection };
const char* opName(Op op);

struct Result {
    Mesh mesh;
    int inputPolygons = 0;
    int outputPolygons = 0;
};

// Both meshes must be given in the same space (e.g. world space). Inputs
// should be closed; open meshes give "best effort" results like any BSP CSG.
Result apply(const Mesh& a, const Mesh& b, Op op);

}  // namespace csg
