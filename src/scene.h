#pragma once
// Scene = list of objects linked into a transform hierarchy.
//
// Each object stores a local TRS transform and an optional parent; its world
// matrix is parentWorld * local (scene graphs, FoCG 5e sec. 12.2; skeletal
// hierarchies, GEA Vol. II sec. 13.2). Objects come in a few kinds: meshes,
// lights, empties (pure transforms for grouping), bones and particle emitters.
#include "mesh.h"
#include "parametric.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

enum class ObjectKind { Mesh = 0, Light, Empty, Bone, Emitter, Camera };
constexpr int kObjectKindCount = 6;
const char* kindName(ObjectKind k);

enum class LightType { Point = 0, Sun, Spot, Area };
constexpr int kLightTypeCount = 4;

// Lights (FoCG 5e sec. 5.1; GEA Vol. II sec. 12.5). Sun, spot and area lights
// shine along the object's local -Y axis; an area light is a one-sided
// rectangle in the local XZ plane (width along X, height along Z).
struct LightSettings {
    LightType type = LightType::Point;
    Vec3 color{1.0f, 0.95f, 0.85f};
    float intensity = 15.0f;
    float range = 25.0f;
    float spotAngle = 40.0f;  // full cone angle, degrees
    float spotBlend = 0.25f;  // 0 = hard edge, 1 = soft
    float width = 1.0f, height = 1.0f;  // area light size
    // Colour temperature (Kelvin), multiplied with `color` when enabled
    // (like Unity's "Use Color Temperature").
    bool useTemperature = false;
    float temperature = 6500.0f;
    Vec3 finalColor() const;  // color (x temperature tint), linear RGB
};

// Black-body colour of a temperature in Kelvin (1000-40000), as linear RGB
// normalised so the brightest channel is 1. 6500 K is close to white.
Vec3 kelvinToRGB(float kelvin);

// A physical camera (thin-lens model, FoCG 5e sec. 4.3 / 13.4 depth of
// field). It looks along its local -Z axis with +Y up.
//  - Field of view from focal length and sensor width (36 mm = full frame).
//  - Depth of field: the lens aperture is focalLength / fStop wide, and
//    objects at focusDistance are sharp.
//  - Exposure combines ISO, shutter time and f-number like a real camera,
//    relative to f/8, 1/125 s, ISO 100 (= 1x, "EV 0") plus compensation.
struct PhysicalCamera {
    float focalLength = 50.0f;  // mm
    float sensorWidth = 36.0f;  // mm
    float fStop = 8.0f;
    float focusDistance = 5.0f;  // scene units (metres)
    float iso = 100.0f;
    float shutter = 1.0f / 125.0f;  // seconds
    float exposureComp = 0.0f;      // EV stops
    int blades = 0;                 // 0 = round aperture, 5..9 = polygonal bokeh
    bool depthOfField = true;
    float verticalFovDeg(float aspect) const;
    float exposure() const;  // linear multiplier
    float apertureRadius() const { return focalLength / std::max(0.5f, fStop) * 0.5f * 0.001f; }  // metres
};

// Texture slots of the PBR material (metallic-roughness, as in glTF).
// Grayscale maps work in every slot; packed "ORM" maps work too because
// roughness reads green, metallic blue and AO red.
enum TexSlot { TEX_BASE = 0, TEX_NORMAL, TEX_ROUGHNESS, TEX_METALLIC, TEX_AO, TEX_EMISSION, TEX_OPACITY, TEX_COUNT };
const char* texSlotName(int slot);
const char* texSlotLabel(int slot);

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
    // PBR (metallic-roughness): base color = `color`.
    float roughness = 0.5f;
    float metallic = 0.0f;
    float opacity = 1.0f;        // alpha: 1 = solid, 0 = invisible
    float transmission = 0.0f;   // glass: refracts light through the surface
    float ior = 1.45f;           // index of refraction for transmission / Fresnel
    float normalStrength = 1.0f;
    Vec2 uvScale{1.0f, 1.0f};    // texture tiling
    std::string textures[TEX_COUNT];  // image paths; empty = unused
    bool hasTextures() const;
    bool transparent() const { return opacity < 0.999f || transmission > 0.001f; }
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

    // Camera
    PhysicalCamera camera;

    // Local matrix M = T * R * S (FoCG 5e sec. 7.5 / GEA Vol. I sec. 5.3).
    Mat4 matrix() const { return translation(position) * eulerToMatrix(rotation) * scaling(scale); }
    bool isMesh() const { return kind == ObjectKind::Mesh; }
};

struct Scene {
    std::vector<Object> objects;
    int active = -1;  // index of the active object, or -1
    uint32_t nextId = 1;
    Vec3 ambient{0.05f, 0.055f, 0.07f};
    uint32_t renderCamera = 0;  // camera object used for rendering (0 = the first camera, if any)
    int renderCameraIndex() const;

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
// `localRaycast` (optional) intersects an unskinned mesh object `index` with a
// ray given in its local space, e.g. through a cached BVH; returns the hit t.
using LocalRaycast = std::function<bool(int index, Vec3 origin, Vec3 dir, float& t)>;
int pickObject(const Scene& scene, Vec3 origin, Vec3 dir, float* tOut = nullptr, const LocalRaycast& localRaycast = {});

// Native scene format (.m3d, plain text: transforms, hierarchy, materials,
// lights, UVs, skinning, emitters, parametric recipes).
bool saveScene(const Scene& scene, const std::string& path, std::string& err);
bool loadScene(Scene& scene, const std::string& path, std::string& err);

// Wavefront OBJ (+ .mtl with object colors). Export bakes transforms / pose.
bool exportOBJ(const Scene& scene, const std::string& path, std::string& err, int* exportedCount = nullptr);
bool importOBJ(Scene& scene, const std::string& path, std::string& err, int* firstNewIndex = nullptr);

// Outliner drag & drop. Onto = parent to the target; Before / After = become
// the target's sibling at that position; Root = unparent and move to the end.
// Moves the top-most of `ids` (children follow), keeps world transforms.
// Returns false if impossible (dropping onto itself or into its own subtree).
enum class HierarchyDrop { None = 0, Before = 1, Onto = 2, After = 3, Root = 4 };
bool moveInHierarchy(Scene& scene, const std::vector<uint32_t>& ids, uint32_t targetId, HierarchyDrop zone);
