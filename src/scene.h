#pragma once
// Scene = flat list of objects, each a mesh with a TRS transform.
// (A flat list is the simplest form of the scene graph in FoCG 5e sec. 12.2,
//  and of the "game world" object model in GEA Vol. II ch. 16.)
#include "mesh.h"

#include <cstdint>
#include <string>
#include <vector>

struct Object {
    uint32_t id = 0;  // stable identity (GPU cache key), never reused
    std::string name;
    Mesh mesh;
    Vec3 position{0, 0, 0};
    Vec3 rotation{0, 0, 0};  // Euler degrees, R = Rz * Ry * Rx
    Vec3 scale{1, 1, 1};
    Vec3 color{0.8f, 0.8f, 0.8f};
    bool smooth = true;
    bool selected = false;

    // Model matrix M = T * R * S (FoCG 5e sec. 7.5 / GEA Vol. I sec. 5.3).
    Mat4 matrix() const { return translation(position) * eulerToMatrix(rotation) * scaling(scale); }
};

struct Scene {
    std::vector<Object> objects;
    int active = -1;       // index of the active object, or -1
    uint32_t nextId = 1;

    int add(Mesh mesh, const std::string& baseName, Vec3 color);
    std::string uniqueName(const std::string& base, int ignoreIndex = -1) const;
    Object* activeObject() { return (active >= 0 && active < (int)objects.size()) ? &objects[active] : nullptr; }
    int selectedCount() const;
    size_t triangleCount() const;
};

// Ray picking (FoCG 5e ch. 4): returns the index of the nearest hit object.
int pickObject(const Scene& scene, Vec3 origin, Vec3 dir, float* tOut = nullptr);

// Native scene format (.m3d, plain text, keeps transforms and n-gons).
bool saveScene(const Scene& scene, const std::string& path, std::string& err);
bool loadScene(Scene& scene, const std::string& path, std::string& err);

// Wavefront OBJ (+ .mtl with object colors). Export bakes transforms.
bool exportOBJ(const Scene& scene, const std::string& path, std::string& err, int* exportedCount = nullptr);
bool importOBJ(Scene& scene, const std::string& path, std::string& err, int* firstNewIndex = nullptr);
