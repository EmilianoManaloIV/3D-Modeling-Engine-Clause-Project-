#pragma once
// Scene = list of objects linked into a transform hierarchy.
//
// Each object stores a local TRS transform and an optional parent; its world
// matrix is parentWorld * local (scene graphs, FoCG 5e sec. 12.2; skeletal
// hierarchies, GEA Vol. II sec. 13.2). Objects come in a few kinds: meshes,
// lights, empties (pure transforms for grouping), bones and particle emitters.
#include "mesh.h"
#include "parametric.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

enum class ObjectKind { Mesh = 0, Light, Empty, Bone, Emitter };
const char* kindName(ObjectKind k);

enum class LightType { Point = 0, Sun, Spot };

// Punctual light (FoCG 5e sec. 5.1; GEA Vol. II sec. 12.5). A sun or spot
// shines along the object's local -Y axis.
struct LightSettings {
    LightType type = LightType::Point;
    Vec3 color{1.0f, 0.95f, 0.85f};
    float intensity = 15.0f;
    float range = 25.0f;
    float spotAngle = 40.0f;  // full cone angle, degrees
    float spotBlend = 0.25f;  // 0 = hard edge, 1 = soft
};

struct ParticleSettings {
    float rate = 60;         // particles per second
    float lifetime = 2.0f;   // seconds (+-25%)
    float speed = 2.5f;      // initial speed along local +Y
    float spread = 20;       // cone half-angle, degrees
    float gravity = -2.0f;   // world Y acceleration
    float drag = 0.2f;
    float startSize = 0.25f, endSize = 0.05f;
    float radius = 0.1f;     // spawn sphere radius
    Vec3 startColor{1.0f, 0.78f, 0.3f};
    Vec3 endColor{0.85f, 0.15f, 0.05f};
    bool additive = true;    // glowing (additive) vs. alpha-blended smoke
};

struct Object {
    uint32_t id = 0;  // stable identity, never reused
    std::string name;
    ObjectKind kind = ObjectKind::Mesh;
    uint32_t parent = 0;  // id of the parent object, 0 = none

    Vec3 position{0, 0, 0};
    Vec3 rotation{0, 0, 0};  // Euler degrees, R = Rz * Ry * Rx
    Vec3 scale{1, 1, 1};
    bool selected = false;

    // Mesh + material
    Mesh mesh;
    Vec3 color{0.8f, 0.8f, 0.8f};
    Vec3 emission{1.0f, 1.0f, 1.0f};
    float emissionStrength = 0.0f;  // 0 = no glow
    float gloss = 0.5f;
    bool smooth = true;
    ParametricSpec param;  // active() -> mesh is regenerated from the recipe

    // Skinning: bones driving this mesh and their inverse bind matrices.
    std::vector<uint32_t> skinBones;
    std::vector<Mat4> bindInverse;

    // Light
    LightSettings light;

    // Bone length (also the display size of empties)
    float boneLength = 1.0f;
    bool hasRest = false;
    Vec3 restPosition, restRotation, restScale{1, 1, 1};

    // Emitter
    ParticleSettings particles;

    // Local matrix M = T * R * S (FoCG 5e sec. 7.5 / GEA Vol. I sec. 5.3).
    Mat4 matrix() const { return translation(position) * eulerToMatrix(rotation) * scaling(scale); }
    bool isMesh() const { return kind == ObjectKind::Mesh; }
};

struct Scene {
    std::vector<Object> objects;
    int active = -1;  // index of the active object, or -1
    uint32_t nextId = 1;
    Vec3 ambient{0.05f, 0.055f, 0.07f};

private:
    // `objects` is edited freely all over the editor, so the cache validates
    // itself: every hit is checked, and a signature of the vector (size, buffer,
    // first/last id) tells when a miss means "really absent" vs "stale map".
    mutable std::unordered_map<uint32_t, int> idCache_;
    mutable size_t cacheSize_ = ~size_t(0);
    mutable const Object* cacheData_ = nullptr;
    mutable uint32_t cacheFirst_ = 0, cacheLast_ = 0;
    void rebuildIdCache() const;
    bool idCacheCurrent() const;

public:

    int add(Mesh mesh, const std::string& baseName, Vec3 color);
    int addObject(Object o);  // assigns id + unique name
    std::string uniqueName(const std::string& base, int ignoreIndex = -1) const;
    Object* activeObject() { return (active >= 0 && active < (int)objects.size()) ? &objects[active] : nullptr; }
    int selectedCount() const;
    size_t triangleCount() const;

    // Hierarchy
    int indexOf(uint32_t id) const;  // O(1) amortized (cached id -> index map)
    int parentIndex(int i) const;
    Mat4 world(int i) const;
    void computeWorlds(std::vector<Mat4>& out) const;  // every object's world matrix, O(n)
    Mat4 parentWorld(int i) const;
    bool isAncestor(int ancestor, int i) const;
    int depth(int i) const;
    // Re-parents `child` (parent -1 = none). keepWorld preserves where the
    // object is in the world. Refuses (returns false) to create a cycle.
    bool setParent(int child, int parent, bool keepWorld);
    std::vector<int> descendants(int i) const;
};

// Ray picking of meshes (FoCG 5e ch. 4): index of the nearest hit mesh.
int pickObject(const Scene& scene, Vec3 origin, Vec3 dir, float* tOut = nullptr);

// Native scene format (.m3d, plain text: transforms, hierarchy, materials,
// lights, UVs, skinning, emitters, parametric recipes).
bool saveScene(const Scene& scene, const std::string& path, std::string& err);
bool loadScene(Scene& scene, const std::string& path, std::string& err);

// Wavefront OBJ (+ .mtl with object colors). Export bakes transforms / pose.
bool exportOBJ(const Scene& scene, const std::string& path, std::string& err, int* exportedCount = nullptr);
bool importOBJ(Scene& scene, const std::string& path, std::string& err, int* firstNewIndex = nullptr);
