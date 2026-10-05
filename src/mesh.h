#pragma once
// Polygon mesh representation and modeling operations.
//
// A Mesh is a shared-vertex ("indexed") mesh as described in FoCG 5e sec. 12.1,
// generalized from triangles to n-gons so quads survive editing and OBJ round
// trips. Faces store vertex indices in counter-clockwise order when seen from
// the outside, so the right-hand rule gives an outward normal.
//
// Optional attributes:
//   uvs     - texture coordinates per face *corner* (FoCG 5e ch. 11). Storing
//             them per corner instead of per vertex lets UV seams exist
//             without splitting vertices.
//   weights - up to 4 bone influences per vertex for skinning
//             (GEA Vol. II sec. 13.5, "matrix palette" skinning).
#include "math3d.h"

#include <cstdint>
#include <utility>
#include <vector>

struct BoneWeights {
    int bone[4] = {-1, -1, -1, -1};  // slot into Object::skinBones, -1 = unused
    float w[4] = {0, 0, 0, 0};

    void add(int b, float weight);   // merges; keeps the 4 strongest influences
    void set(int b, float weight);   // replaces this bone's weight (0 removes)
    void remove(int b);
    void normalize();
    float weightOf(int b) const;
    float total() const { return w[0] + w[1] + w[2] + w[3]; }
};

struct Mesh {
private:
    mutable uint64_t cachedTriTopology_ = ~0ull, cachedBoundsVersion_ = ~0ull;
    mutable size_t cachedTris_ = 0;
    mutable Vec3 cachedLo_, cachedHi_;

public:
    std::vector<Vec3> verts;
    std::vector<std::vector<int>> faces;
    std::vector<std::vector<Vec2>> uvs;  // empty, or parallel to faces (one UV per corner)
    std::vector<BoneWeights> weights;    // empty, or parallel to verts
    // Globally unique content stamps. Any code that edits the mesh calls
    // touch() (bumps both); code that only moves vertices may call
    // touchPositions() (bumps `version` only) so topology-derived data such as
    // the edge list stays cached. The renderer re-uploads when `version` changes.
    uint64_t version = 0;
    uint64_t topology = 0;

    void touch();
    void touchPositions();
    size_t triangleCount() const;  // cached per topology
    bool bounds(Vec3& lo, Vec3& hi) const;  // cached per version; false if empty
    bool hasUVs() const { return !faces.empty() && uvs.size() == faces.size(); }
    bool hasWeights() const { return !verts.empty() && weights.size() == verts.size(); }
    void validate();  // drops attribute arrays that don't match the topology
};

// Newell's method: robust normal for planar or slightly non-planar polygons.
// The returned vector's length is twice the polygon's area.
Vec3 faceNormalRaw(const Mesh& m, const std::vector<int>& face);
Vec3 faceNormalRaw(const std::vector<Vec3>& positions, const std::vector<int>& face);
Vec3 faceCenter(const Mesh& m, const std::vector<int>& face);

namespace primitives {
// All primitives come with sensible UVs.
Mesh box(float w, float h, float d, int segments = 1);
Mesh cube(float size = 2.0f);
Mesh plane(float size = 2.0f, int subdivisions = 1);
Mesh grid(float width, float depth, int segX, int segZ);
Mesh uvSphere(float radius = 1.0f, int segments = 24, int rings = 16);
Mesh frustum(float rBottom, float rTop, float height, int segments);  // rTop = 0 -> cone
Mesh cylinder(float radius = 1.0f, float height = 2.0f, int segments = 24);
Mesh cone(float radius = 1.0f, float height = 2.0f, int segments = 24);
Mesh torus(float majorRadius = 1.0f, float minorRadius = 0.35f, int segmentsU = 32, int segmentsV = 16);
}  // namespace primitives

// --- Modeling operations ----------------------------------------------------

// One step of Catmull-Clark subdivision (with boundary rules). Every face of
// the result is a quad. UVs and bone weights are interpolated.
Mesh catmullClark(const Mesh& in);

// Reverses the winding (and therefore the normal) of every face.
void flipNormals(Mesh& m);

// Merges vertices closer than `eps` (used by generators that build pieces).
void weldVertices(Mesh& m, float eps = 1e-5f);

// Drops vertices no face references. Returns old->new index map (-1 = removed).
std::vector<int> removeUnusedVertices(Mesh& m);

// Deletes the selected vertices and every face that uses them.
// `selection` is resized to match the new vertex count (and cleared).
void deleteVertices(Mesh& m, std::vector<char>& selection);

// Extrudes the region made of faces whose vertices are all selected. Side
// walls are created along the region boundary and the selection moves to the
// new (extruded) vertices. Returns false if no whole face was selected.
bool extrudeSelectedFaces(Mesh& m, std::vector<char>& selection, Vec3* outNormal);
// Same for an explicit list of faces (face-select mode).
bool extrudeFaces(Mesh& m, const std::vector<int>& faces, std::vector<char>& selection, Vec3* outNormal);

// Faces whose vertices are all selected.
std::vector<int> selectedFaces(const Mesh& m, const std::vector<char>& selection);

// --- Queries -----------------------------------------------------------------

std::vector<std::pair<int, int>> uniqueEdges(const Mesh& m);

// Ray vs axis-aligned box (slab test); t range of the hit in [0, tMax].
bool rayHitsBox(Vec3 o, Vec3 d, Vec3 lo, Vec3 hi, float tMax = 1e30f);

// Ray / mesh intersection (ray-triangle test of FoCG 5e sec. 4.4, in the
// Moller-Trumbore form). The ray is o + t*d; returns the nearest t > 0.
// `positions` overrides the mesh's own vertex positions (e.g. skinned pose).
bool raycastMesh(const Mesh& m, Vec3 o, Vec3 d, float& tHit);
bool raycastMesh(const Mesh& m, const std::vector<Vec3>& positions, Vec3 o, Vec3 d, float& tHit);

// Triangulated, per-corner vertex stream for rendering. With `smooth`,
// normals are averaged across faces meeting at less than `smoothAngleDeg`
// ("auto smooth"), so a cylinder's caps stay crisp while its side is smooth.
// `weightSlot` >= 0 fills `weight` with that bone's influence (weight view).
struct RenderVertex {
    Vec3 pos;
    Vec3 normal;
    Vec2 uv;
    float weight;
};
// Indexed form used by the renderer: smooth vertices are shared between
// faces (split only at UV seams / hard edges), cutting the GPU vertex count
// and upload size several-fold on smooth meshes.
void buildRenderMesh(const Mesh& m, const std::vector<Vec3>& positions, bool smooth, float smoothAngleDeg,
                     int weightSlot, std::vector<RenderVertex>& vertices, std::vector<uint32_t>& indices);
// Expanded (non-indexed) triangle list - three vertices per triangle.
void buildRenderData(const Mesh& m, const std::vector<Vec3>& positions, bool smooth, float smoothAngleDeg,
                     int weightSlot, std::vector<RenderVertex>& out);
