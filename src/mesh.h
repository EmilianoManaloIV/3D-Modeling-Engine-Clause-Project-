#pragma once
// Polygon mesh representation and modeling operations.
//
// A Mesh is a shared-vertex ("indexed") mesh as described in FoCG 5e sec. 12.1,
// generalized from triangles to n-gons so quads survive editing and OBJ round
// trips. Faces store vertex indices in counter-clockwise order when seen from
// the outside, so the right-hand rule gives an outward normal.
#include "math3d.h"

#include <cstdint>
#include <utility>
#include <vector>

struct Mesh {
    std::vector<Vec3> verts;
    std::vector<std::vector<int>> faces;
    // Globally unique content stamp. Any code that edits verts/faces calls
    // touch(); the renderer re-uploads GPU buffers when the stamp changes.
    uint64_t version = 0;

    void touch();
    size_t triangleCount() const;
};

// Newell's method: robust normal for planar or slightly non-planar polygons.
// The returned vector's length is twice the polygon's area.
Vec3 faceNormalRaw(const Mesh& m, const std::vector<int>& face);
Vec3 faceCenter(const Mesh& m, const std::vector<int>& face);

namespace primitives {
Mesh cube(float size = 2.0f);
Mesh plane(float size = 2.0f, int subdivisions = 1);
Mesh uvSphere(float radius = 1.0f, int segments = 24, int rings = 16);
Mesh cylinder(float radius = 1.0f, float height = 2.0f, int segments = 24);
Mesh cone(float radius = 1.0f, float height = 2.0f, int segments = 24);
Mesh torus(float majorRadius = 1.0f, float minorRadius = 0.35f, int segmentsU = 32, int segmentsV = 16);
}  // namespace primitives

// --- Modeling operations ----------------------------------------------------

// One step of Catmull-Clark subdivision (with boundary rules). Every face of
// the result is a quad.
Mesh catmullClark(const Mesh& in);

// Reverses the winding (and therefore the normal) of every face.
void flipNormals(Mesh& m);

// Drops vertices no face references. Returns old->new index map (-1 = removed).
std::vector<int> removeUnusedVertices(Mesh& m);

// Deletes the selected vertices and every face that uses them.
// `selection` is resized to match the new vertex count (and cleared).
void deleteVertices(Mesh& m, std::vector<char>& selection);

// Extrudes the region made of faces whose vertices are all selected. Side
// walls are created along the region boundary and the selection moves to the
// new (extruded) vertices. Returns false if no whole face was selected.
bool extrudeSelectedFaces(Mesh& m, std::vector<char>& selection, Vec3* outNormal);

// --- Queries -----------------------------------------------------------------

std::vector<std::pair<int, int>> uniqueEdges(const Mesh& m);

// Ray / mesh intersection (ray-triangle test of FoCG 5e sec. 4.4, in the
// Moller-Trumbore form).
// The ray is o + t*d; returns the nearest t > 0.
bool raycastMesh(const Mesh& m, Vec3 o, Vec3 d, float& tHit);

// Triangulated, per-corner-normal vertex stream for rendering. With `smooth`,
// normals are averaged across faces meeting at less than `smoothAngleDeg`
// ("auto smooth"), so a cylinder's caps stay crisp while its side is smooth.
struct RenderVertex {
    Vec3 pos;
    Vec3 normal;
};
void buildRenderData(const Mesh& m, bool smooth, float smoothAngleDeg, std::vector<RenderVertex>& out);
