#pragma once
// UV unwrapping and UV tools (texture mapping, FoCG 5e ch. 11).
//
// All functions work on a list of face indices ("the selection"); an empty
// list means every face. UVs are stored per face corner (Mesh::uvs), so seams
// are simply corners of the same vertex with different UVs.
#include "mesh.h"

#include <vector>

namespace uv {

enum class Method {
    Smart,        // split into charts by dominant normal axis, project, pack
    Box,          // cube projection: each face along its dominant axis
    Planar,       // one projection along the selection's average normal
    Cylindrical,  // wrap around the Y axis
    Spherical,    // latitude / longitude around the selection centre
    PerFace,      // every face its own island, packed (lightmap style)
};
const char* methodName(Method m);

void ensure(Mesh& m);  // allocates zeroed UVs if the mesh has none
void unwrap(Mesh& m, Method method, std::vector<int> faces = {});

// Island tools
std::vector<int> islandIds(const Mesh& m, const std::vector<int>& faces, int* islandCount);
void pack(Mesh& m, std::vector<int> faces = {});      // re-layout islands into 0..1 without overlap
void fit(Mesh& m, std::vector<int> faces = {});       // scale selection's bounds into 0..1
void rotate90(Mesh& m, std::vector<int> faces = {});  // counter-clockwise around the bounds' centre
void flip(Mesh& m, std::vector<int> faces, bool horizontal);
void translate(Mesh& m, const std::vector<int>& faces, Vec2 delta);

// Bounds of the selection's UVs.
void bounds(const Mesh& m, const std::vector<int>& faces, Vec2& lo, Vec2& hi);

}  // namespace uv
