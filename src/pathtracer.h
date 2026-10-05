#pragma once
// Monte Carlo path tracer (FoCG 5e ch. 4 ray tracing, ch. 13 sampling and
// Monte Carlo integration, sec. 14.10 Monte Carlo ray tracing):
//   - primary rays jittered inside each pixel (anti-aliasing by sampling);
//   - at every hit, direct light from each lamp is sampled with a shadow ray
//     ("next event estimation"), using the same falloff as the viewport so a
//     render matches the Lit preview;
//   - the path continues by importance sampling the material: a cosine-
//     weighted diffuse bounce or a Phong-lobe glossy bounce;
//   - emissive surfaces light the scene by being hit;
//   - Russian roulette ends long paths without bias.
// Samples accumulate progressively, so the image refines over time and the
// user chooses the sample count (quality vs. time).
//
// The scene is flattened to world-space triangles + a BVH (bvh.h). The same
// data is uploaded to the GPU tracer (gpu_tracer.h), which runs this exact
// algorithm in a fragment shader; this file is the CPU (multithreaded) path.
#include "bvh.h"
#include "jobs.h"
#include "scene.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace rt {

struct Material {
    Vec3 albedo;      // linear
    float specK = 0;  // probability / strength of the glossy lobe
    Vec3 emission;    // radiance added on hit
    float shininess = 1;
};

struct Light {
    int type = 0;  // 0 point, 1 sun, 2 spot (LightType)
    Vec3 pos, dir, color;
    float range = 10, cosInner = 1, cosOuter = 0;
    float radius = 0.05f;  // soft shadows: point/spot sphere radius, sun angular radius (radians)
};

struct Settings {
    int maxBounces = 4;
    float clampIndirect = 10.0f;  // firefly clamp on indirect light (0 = off)
    float envStrength = 1.0f;     // sky brightness
    float lightSize = 0.05f;      // soft shadow size
    bool studioLights = true;     // light a scene without lamps like the Studio preview
};

struct View {
    Vec3 eye, forward, right, up;
    float tanHalfFov = 0.5f;
    float orthoHalfHeight = 1.0f;
    bool ortho = false;
    float aspect = 1.0f;
};
View makeView(Vec3 eye, Vec3 target, float fovYDeg, bool ortho, float orthoHalfHeight, float aspect);
// Primary ray through normalized image coordinates (0..1, y down).
void primaryRay(const View& v, float sx, float sy, Vec3& o, Vec3& d);

struct SceneData {
    std::vector<Vec3> verts;    // 3 per triangle, world space
    std::vector<Vec3> normals;  // 3 per triangle (shading normals)
    std::vector<int> triMaterial;
    std::vector<uint8_t> noShadow;  // per triangle: emissive surfaces (lamp shades) don't block lamps
    std::vector<Material> materials;
    std::vector<Light> lights;
    Vec3 skyLow, skyHigh;  // sky radiance by direction (horizon / zenith)
    Bvh bvh;
    double buildMs = 0;
    size_t triangleCount() const { return triMaterial.size(); }
};
// Flattens the scene (skinned poses, smooth normals, parametric shapes as
// currently shown) into world-space triangles and builds the BVH.
void buildScene(const Scene& scene, const Settings& settings, SceneData& out, Vec3 studioKeyDir = Vec3(),
                Vec3 studioFillDir = Vec3());

struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed * 6364136223846793005ull + 1442695040888963407ull) { next(); }
    uint32_t next() {  // PCG32
        uint64_t old = state;
        state = old * 6364136223846793005ull + 1442695040888963407ull;
        uint32_t x = (uint32_t)(((old >> 18u) ^ old) >> 27u), r = (uint32_t)(old >> 59u);
        return (x >> r) | (x << ((-r) & 31));
    }
    float uniform() { return (next() >> 8) * (1.0f / 16777216.0f); }
};

// Radiance arriving along the ray.
Vec3 trace(const SceneData& s, const Settings& set, Vec3 o, Vec3 d, Rng& rng);
// Display transform shared with the viewport and the GPU tracer: soft
// shoulder above 0.8, then gamma 2.2.
Vec3 toDisplay(Vec3 linear);

// Progressive multithreaded renderer: every pass adds one sample to each
// pixel; tiles of a pass are spread over the job system's worker threads
// while the UI thread keeps running.
class CpuRenderer {
public:
    ~CpuRenderer();
    void start(std::shared_ptr<const SceneData> scene, const View& view, const Settings& settings, int w, int h,
               int targetSamples);
    void stop();      // cancels and waits for the running pass
    bool update();    // call once per frame: finishes / launches passes; true if a pass completed
    bool running() const { return running_; }
    int samples() const { return samples_; }
    int width() const { return w_; }
    int height() const { return h_; }
    double elapsedSeconds() const;
    double samplesPerSecond() const;  // pixel samples per second (all threads)
    void setTargetSamples(int n) {  // raising it resumes a finished render
        target_ = n;
        if (!running_ && scene_ && samples_ < target_ && startTime_ != 0 && resumable_) {
            running_ = true;
            launchPass();
        }
    }
    // Tone-mapped RGBA8 (top row first) of the last resolved sample count.
    const std::vector<uint8_t>& image() const { return image_; }
    int imageSamples() const { return imageSamples_; }
    bool takeImageDirty() {
        bool d = imageDirty_;
        imageDirty_ = false;
        return d;
    }

private:
    void launchPass();
    void resolveInto(std::vector<uint8_t>& rgba) const;
    std::vector<uint8_t> image_;
    int imageSamples_ = 0;
    bool imageDirty_ = false;
    double lastResolve_ = 0;
    std::shared_ptr<const SceneData> scene_;
    View view_;
    Settings settings_;
    int w_ = 0, h_ = 0, target_ = 0;
    struct Accum {
        Vec3 c;
        float n = 0;
    };
    std::vector<Accum> accum_;
    jobs::TaskGroup group_;
    bool running_ = false, passActive_ = false, resumable_ = false;
    int samples_ = 0;
    double startTime_ = 0, endTime_ = 0;
};

double nowSeconds();

}  // namespace rt
