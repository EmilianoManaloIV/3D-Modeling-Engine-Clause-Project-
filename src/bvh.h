#pragma once
// Bounding volume hierarchy over triangles (FoCG 5e sec. 12.3, "Spatial data
// structures": hierarchical bounding boxes), built top-down with the surface area heuristic
// evaluated over 16 bins per axis. Nodes are stored flattened in depth-first
// order so the same arrays can be walked by the CPU tracer and uploaded as a
// texture for the GPU tracer.
#include "math3d.h"

#include <cstdint>
#include <vector>

struct BvhNode {
    Vec3 lo;
    int32_t leftOrFirst = 0;  // interior: index of the left child (right = left + 1); leaf: first triangle
    Vec3 hi;
    int32_t count = 0;        // > 0: leaf with `count` triangles
};

struct Bvh {
    std::vector<BvhNode> nodes;
    std::vector<int32_t> order;  // triangle indices in leaf order
    int maxDepth = 0;

    // `v` holds three vertices per triangle.
    void build(const std::vector<Vec3>& v, int maxLeafSize = 4);
};

// Ray / triangle intersection result.
struct Hit {
    float t = 1e30f;
    int tri = -1;
    float u = 0, v = 0;  // barycentrics of vertices 1 and 2
};

// Nearest hit along o + t*d for t in (tMin, hit.t). `v` = three vertices per triangle.
bool intersectBvh(const Bvh& bvh, const std::vector<Vec3>& v, Vec3 o, Vec3 d, float tMin, Hit& hit);
// Any hit closer than tMax (shadow rays). Triangles flagged in `skip`
// (indexed by triangle, may be null) are ignored.
bool occludedBvh(const Bvh& bvh, const std::vector<Vec3>& v, Vec3 o, Vec3 d, float tMin, float tMax,
                 const std::vector<uint8_t>* skip = nullptr);

// Ray casting against one mesh through a BVH, for picking. Triangles are the
// faces' fans (as raycastMesh does); `triFace` maps them back to faces.
struct Mesh;
struct MeshAccel {
    uint64_t key = ~0ull;
    std::vector<Vec3> tris;  // 3 per triangle, in the mesh's local space
    std::vector<int> triFace;
    Bvh bvh;
    void build(const Mesh& m, const std::vector<Vec3>& positions);
    // Nearest hit t > 0 along o + t*d; `face` receives the face index.
    bool raycast(Vec3 o, Vec3 d, float& t, int* face = nullptr) const;
};
