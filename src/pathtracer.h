#pragma once
// Monte Carlo path tracer (FoCG 5e ch. 4 ray tracing, ch. 13 sampling and
// Monte Carlo integration, sec. 14.10 Monte Carlo ray tracing):
//   - primary rays jittered inside each pixel (anti-aliasing by sampling),
//     through a thin lens when the camera has depth of field;
//   - at every hit, direct light from each lamp is sampled with a shadow ray
//     ("next event estimation"); area lights are sampled over their surface.
//     Shadow rays pass through glass / alpha surfaces, tinted by them;
//   - materials are physically based (metallic-roughness, as in glTF):
//     GGX microfacet specular with Schlick Fresnel and Smith masking,
//     Lambert diffuse, alpha (opacity), and rough glass (transmission with
//     an index of refraction). Base color, normal, roughness, metallic, AO,
//     emission and opacity can come from textures;
//   - the path continues by importance sampling one lobe of the material;
//   - emissive surfaces light the scene by being hit;
//   - Russian roulette ends long paths without bias.
// Samples accumulate progressively. Exposure (camera ISO / shutter /
// aperture) is applied when the result is displayed.
//
// Light units: a lamp of intensity I lights a white diffuse surface it
// faces at distance d to about I / d^2, the same as in the viewport's Lit
// view (the BRDF is scaled by pi so lamps keep their viewport brightness).
//
// The same algorithm runs on the GPU (gpu_tracer.cpp, GLSL) and through
// DirectX ray tracing (hwrt.cpp, HLSL); this is the CPU (multithreaded) one.
#include "bvh.h"
#include "image_load.h"
#include "jobs.h"
#include "scene.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace rt {

struct Material {
    Vec3 baseColor{0.8f, 0.8f, 0.8f};  // linear
    float metallic = 0, roughness = 0.5f;
    Vec3 emission;                     // radiance
    float opacity = 1, transmission = 0, ior = 1.45f, normalStrength = 1;
    Vec2 uvScale{1, 1};
    int tex[TEX_COUNT] = {-1, -1, -1, -1, -1, -1, -1};  // index into SceneData::textures
};

struct Light {
    int type = 0;  // LightType: 0 point, 1 sun, 2 spot, 3 area
    Vec3 pos, dir, color;  // dir: direction the light shines; color = linear color * intensity
    float range = 10, cosInner = 1, cosOuter = 0;
    float radius = 0.05f;  // soft shadows: point/spot sphere radius, sun angular radius (radians)
    Vec3 axisU, axisV;     // area light: half-extent vectors of the rectangle
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
    float lensRadius = 0.0f;     // > 0: depth of field (thin lens)
    float focusDistance = 5.0f;  // along `forward`
    int blades = 0;              // 0 = round aperture, else polygonal
    float exposure = 1.0f;       // applied at display
};
View makeView(Vec3 eye, Vec3 target, float fovYDeg, bool ortho, float orthoHalfHeight, float aspect);
// View of a camera object (position, -Z forward, physical lens and exposure).
View cameraView(const Mat4& cameraWorld, const PhysicalCamera& cam, float aspect);
// Primary ray through normalized image coordinates (0..1, y down); (lu, lv)
// in [0,1)^2 pick the point on the lens.
void primaryRay(const View& v, float sx, float sy, float lu, float lv, Vec3& o, Vec3& d);
// Uniform point on the aperture (disc or regular polygon), in units of the radius.
Vec2 sampleAperture(int blades, float u1, float u2);

struct SceneData {
    std::vector<Vec3> verts;    // 3 per triangle, world space
    std::vector<Vec3> normals;  // 3 per triangle (shading normals)
    std::vector<Vec2> uvs;      // 3 per triangle
    std::vector<Vec4> tangents; // 1 per triangle: xyz tangent (along +u), w = bitangent sign
    std::vector<int> triMaterial;
    std::vector<uint8_t> shadowPass;  // per triangle: shadow rays pass (glass, alpha, lamp shades)
    bool anyShadowPass = false;       // the scene has such triangles at all
    std::vector<Material> materials;
    std::vector<std::shared_ptr<const Image>> textures;
    std::vector<std::string> texturePaths;
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

// The scene packed into flat float4 arrays for the GPU tracers (GLSL data
// textures and DirectX structured buffers share this layout):
//   tris: kTriTexels float4 per triangle, in BVH leaf order:
//         0 v0.xyz, material | 1 v1.xyz, tangent sign | 2 v2.xyz, shadowPass (0 / 1)
//         3 n0.xyz, uv0.x | 4 n1.xyz, uv0.y | 5 n2.xyz, uv1.x | 6 tangent.xyz, uv1.y | 7 uv2.xy, 0, 0
//   nodes: 2 float4 per BVH node: (lo, leftOrFirst), (hi, count)
//   mats: kMatTexels float4 per material:
//         0 base.rgb, metallic | 1 emission.rgb, roughness | 2 opacity, transmission, ior, normalStrength
//         3 uvScale.xy, layer(base), layer(normal) | 4 layers(roughness, metallic, ao, emission)
//         5 layer(opacity), 0, 0, 0     (layer -1 = no texture)
//   texels: every texture resampled to texSize x texSize RGBA8 (top row
//           first), one layer each.
//   lights: kLightTexels float4 per light:
//         0 pos, type | 1 dir, range | 2 color, radius | 3 axisU, cosInner | 4 axisV, cosOuter
constexpr int kTriTexels = 8, kMatTexels = 6, kLightTexels = 5;
struct PackedScene {
    std::vector<float> tris, nodes, mats, lights;
    int lightCount = 0;
    bool anyShadowPass = false;
    int texSize = 1, texLayers = 0;
    std::vector<uint8_t> texels;
};
void packScene(const SceneData& s, int maxTextureSize, PackedScene& out);

// Surface properties at a hit after texturing.
struct Surface {
    Vec3 baseColor, emission, normal;  // normal: shading normal (normal-mapped), facing the ray
    float metallic, roughness, opacity, transmission, ior, ao;
};
// Bilinear texture lookup (wrapping), 0..1 RGBA.
Vec4 sampleTexture(const Image& img, Vec2 uv);

// The material model, shared by all tracers and the viewport (pi-scaled so
// a lamp of intensity 1 facing a white diffuse surface gives 1).
// Returns BRDF * pi for light direction l, view direction v, normal n.
Vec3 evalBrdf(const Surface& s, Vec3 n, Vec3 v, Vec3 l);

// Radiance arriving along the ray (unexposed).
Vec3 trace(const SceneData& s, const Settings& set, Vec3 o, Vec3 d, Rng& rng);
// Display transform shared with the viewport and the GPU tracer: exposure,
// soft shoulder above 0.8, then gamma 2.2.
Vec3 toDisplay(Vec3 linear, float exposure = 1.0f);

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
    // Linear (unexposed) average colour of a pixel, for tests.
    Vec3 pixel(int x, int y) const;

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
