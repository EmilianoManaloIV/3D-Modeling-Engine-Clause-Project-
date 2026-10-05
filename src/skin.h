#pragma once
// Skeletal skinning (GEA Vol. II sec. 13.2-13.5).
//
// Bones are ordinary objects in the transform hierarchy (a bone's parent is
// usually another bone). Binding a mesh records, per bone, the inverse of the
// bone's world matrix at bind time. Posing moves the bones; each vertex is
// then transformed by the weighted sum of its bones' skinning matrices
//     S_i = boneWorld_i(now) * inverse(boneWorld_i(bind))
// applied to the vertex's world position ("matrix palette" / linear blend
// skinning). Skinning happens on the CPU so picking and export see the pose.
#include "scene.h"

#include <string>
#include <vector>

bool isSkinned(const Object& o);
void skinMatrices(const Scene& s, const Object& o, std::vector<Mat4>& out);

// Positions to draw/pick mesh `i` with, and the model matrix to apply to them.
// Unskinned meshes (or restPose = true) return the mesh's own vertices with
// the object's world matrix; skinned meshes return world-space posed
// positions (in `scratch`) with an identity model matrix.
const std::vector<Vec3>& evaluateMesh(const Scene& s, int i, bool restPose, std::vector<Vec3>& scratch, Mat4& model);

// Hash of the current skinning matrices, used to know when to re-upload.
uint64_t poseHash(const Scene& s, int i);

Vec3 boneHead(const Scene& s, int i);
Vec3 boneTail(const Scene& s, int i);

// Binds mesh `meshIndex` to the given bone indices with automatic weights
// (inverse-distance to each bone segment, 4 strongest influences).
bool bindSkin(Scene& s, int meshIndex, const std::vector<int>& bones, std::string& error);
void unbindSkin(Object& o);
// Re-computes automatic weights for an already-bound mesh.
void autoWeights(Scene& s, int meshIndex);
