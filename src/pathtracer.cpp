#include "pathtracer.h"

#include "mesh.h"
#include "profiler.h"
#include "skin.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace rt {

double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------
View makeView(Vec3 eye, Vec3 target, float fovYDeg, bool ortho, float orthoHalfHeight, float aspect) {
    View v;
    v.eye = eye;
    v.forward = normalize(target - eye);
    v.right = normalize(cross(v.forward, Vec3(0, 1, 0)));
    if (length(v.right) < 0.5f) v.right = Vec3(1, 0, 0);
    v.up = cross(v.right, v.forward);
    v.tanHalfFov = std::tan(toRadians(fovYDeg) * 0.5f);
    v.ortho = ortho;
    v.orthoHalfHeight = orthoHalfHeight;
    v.aspect = aspect;
    v.focusDistance = length(target - eye);
    return v;
}

View cameraView(const Mat4& w, const PhysicalCamera& cam, float aspect) {
    View v;
    v.eye = transformPoint(w, Vec3());
    v.forward = normalize(transformDir(w, Vec3(0, 0, -1)));
    Vec3 upHint = normalize(transformDir(w, Vec3(0, 1, 0)));
    v.right = normalize(cross(v.forward, upHint));
    if (length(v.right) < 0.5f) v.right = Vec3(1, 0, 0);
    v.up = cross(v.right, v.forward);
    v.tanHalfFov = std::tan(toRadians(cam.verticalFovDeg(aspect)) * 0.5f);
    v.aspect = aspect;
    v.lensRadius = cam.depthOfField ? cam.apertureRadius() : 0.0f;
    v.focusDistance = std::max(0.01f, cam.focusDistance);
    v.blades = cam.blades;
    v.exposure = cam.exposure();
    return v;
}

Vec2 sampleAperture(int blades, float u1, float u2) {
    if (blades < 3) {  // uniform on the disc
        float r = std::sqrt(u1), phi = 2.0f * kPi * u2;
        return {r * std::cos(phi), r * std::sin(phi)};
    }
    // Pick one of the polygon's triangles (equal areas), then a uniform point in it.
    int k = std::min(blades - 1, (int)(u1 * blades));
    float t = u1 * blades - k;
    float a0 = 2.0f * kPi * k / blades, a1 = 2.0f * kPi * (k + 1) / blades;
    float s = std::sqrt(t), b = u2;
    Vec2 p0(std::cos(a0), std::sin(a0)), p1(std::cos(a1), std::sin(a1));
    return (p0 * (1.0f - b) + p1 * b) * s;
}

void primaryRay(const View& v, float sx, float sy, float lu, float lv, Vec3& o, Vec3& d) {
    float px = (2.0f * sx - 1.0f) * v.aspect, py = 1.0f - 2.0f * sy;
    if (v.ortho) {
        // Start well behind the eye so geometry between eye and target is kept.
        o = v.eye + v.right * (px * v.orthoHalfHeight) + v.up * (py * v.orthoHalfHeight) - v.forward * 1000.0f;
        d = v.forward;
        return;
    }
    o = v.eye;
    d = normalize(v.forward + v.right * (px * v.tanHalfFov) + v.up * (py * v.tanHalfFov));
    if (v.lensRadius > 0.0f) {
        // Thin lens: every ray through the lens meets at the focal plane.
        Vec3 focus = o + d * (v.focusDistance / std::max(1e-4f, dot(d, v.forward)));
        Vec2 a = sampleAperture(v.blades, lu, lv) * v.lensRadius;
        o = o + v.right * a.x + v.up * a.y;
        d = normalize(focus - o);
    }
}

// ---------------------------------------------------------------------------
// Scene flattening
// ---------------------------------------------------------------------------
namespace {
Vec3 srgbToLinear(Vec3 c) {
    return {std::pow(std::max(c.x, 0.0f), 2.2f), std::pow(std::max(c.y, 0.0f), 2.2f),
            std::pow(std::max(c.z, 0.0f), 2.2f)};
}
}  // namespace

void buildScene(const Scene& scene, const Settings& settings, SceneData& out, Vec3 keyDir, Vec3 fillDir) {
    PROF_SCOPE("rt build scene");
    const double t0 = nowSeconds();
    out = SceneData();
    // 0. Textures (loaded in parallel, shared through the texture cache).
    {
        std::vector<std::string> paths;
        for (const Object& o : scene.objects)
            if (o.isMesh())
                for (const std::string& t : o.textures)
                    if (!t.empty()) paths.push_back(t);
        textureCache().preload(paths);
    }
    auto textureIndex = [&](const std::string& path) -> int {
        if (path.empty()) return -1;
        for (size_t i = 0; i < out.texturePaths.size(); ++i)
            if (out.texturePaths[i] == path) return (int)i;
        auto img = textureCache().get(path);
        if (!img) return -1;
        out.texturePaths.push_back(path);
        out.textures.push_back(img);
        return (int)out.textures.size() - 1;
    };

    // 1. Gather meshes (posing / world matrices touch scene caches: serial).
    struct Item {
        int object;
        std::vector<Vec3> positions;
        Mat4 model;
        size_t triStart = 0;
        std::vector<RenderVertex> verts;
        std::vector<uint32_t> indices;
    };
    std::vector<Item> items;
    std::vector<Vec3> scratch;
    for (int i = 0; i < (int)scene.objects.size(); ++i) {
        const Object& o = scene.objects[i];
        if (!o.isMesh() || o.mesh.faces.empty()) continue;
        Item it;
        it.object = i;
        it.positions = evaluateMesh(scene, i, false, scratch, it.model);
        items.push_back(std::move(it));
        Material m;
        m.baseColor = srgbToLinear(o.color);
        m.metallic = clampf(o.metallic, 0, 1);
        m.roughness = clampf(o.roughness, 0, 1);
        m.emission = o.emission * o.emissionStrength;
        m.opacity = clampf(o.opacity, 0, 1);
        m.transmission = clampf(o.transmission, 0, 1);
        m.ior = std::max(1.0f, o.ior);
        m.normalStrength = o.normalStrength;
        m.uvScale = o.uvScale;
        for (int t = 0; t < TEX_COUNT; ++t) m.tex[t] = textureIndex(o.textures[t]);
        out.materials.push_back(m);
    }
    // 2. Triangulate + transform in parallel (the expensive part).
    jobs::parallelFor(0, (int)items.size(), 1, [&](int b, int e) {
        for (int k = b; k < e; ++k) {
            Item& it = items[k];
            const Object& o = scene.objects[it.object];
            buildRenderMesh(o.mesh, it.positions, o.smooth, 40.0f, -1, it.verts, it.indices);
            Mat4 nm = transpose(inverse(it.model));
            for (RenderVertex& v : it.verts) {
                v.pos = transformPoint(it.model, v.pos);
                Vec3 n = transformDir(nm, v.normal);
                float len = length(n);
                v.normal = len > 1e-12f ? n / len : Vec3(0, 1, 0);
            }
        }
    });
    size_t tris = 0;
    for (Item& it : items) {
        it.triStart = tris;
        tris += it.indices.size() / 3;
    }
    out.verts.resize(tris * 3);
    out.normals.resize(tris * 3);
    out.uvs.resize(tris * 3);
    out.tangents.resize(tris);
    out.triMaterial.resize(tris);
    out.shadowPass.resize(tris);
    jobs::parallelFor(0, (int)items.size(), 1, [&](int b, int e) {
        for (int k = b; k < e; ++k) {
            const Item& it = items[k];
            const Material& mat = out.materials[k];
            const bool pass = mat.opacity < 0.999f || mat.transmission > 0.001f ||
                              (mat.emission.x + mat.emission.y + mat.emission.z) > 0.0f;
            for (size_t t = 0; t < it.indices.size() / 3; ++t) {
                size_t dst = it.triStart + t;
                for (int c = 0; c < 3; ++c) {
                    const RenderVertex& v = it.verts[it.indices[3 * t + c]];
                    out.verts[3 * dst + c] = v.pos;
                    out.normals[3 * dst + c] = v.normal;
                    out.uvs[3 * dst + c] = v.uv;
                }
                // Tangent frame from the UV gradients (for normal maps).
                Vec3 e1 = out.verts[3 * dst + 1] - out.verts[3 * dst], e2 = out.verts[3 * dst + 2] - out.verts[3 * dst];
                Vec2 d1 = out.uvs[3 * dst + 1] - out.uvs[3 * dst], d2 = out.uvs[3 * dst + 2] - out.uvs[3 * dst];
                float det = d1.x * d2.y - d2.x * d1.y;
                Vec3 ng = cross(e1, e2);
                Vec4 tan(1, 0, 0, 1);
                if (std::fabs(det) > 1e-12f) {
                    Vec3 tu = (e1 * d2.y - e2 * d1.y) / det, tv = (e2 * d1.x - e1 * d2.x) / det;
                    if (length(tu) > 1e-12f) {
                        Vec3 tn = normalize(tu);
                        float w = dot(cross(ng, tn), tv) < 0.0f ? -1.0f : 1.0f;
                        tan = Vec4(tn.x, tn.y, tn.z, w);
                    }
                }
                out.tangents[dst] = tan;
                out.triMaterial[dst] = k;
                out.shadowPass[dst] = pass;
            }
        }
    });

    // 3. Lights (same parameters as the viewport's Lit mode).
    for (int i = 0; i < (int)scene.objects.size() && out.lights.size() < 16; ++i) {
        const Object& o = scene.objects[i];
        if (o.kind != ObjectKind::Light) continue;
        Mat4 w = scene.world(i);
        Light L;
        L.type = (int)o.light.type;
        L.pos = transformPoint(w, Vec3());
        L.dir = normalize(transformDir(w, Vec3(0, -1, 0)));
        L.color = o.light.finalColor() * o.light.intensity;
        L.range = o.light.range;
        float half = toRadians(clampf(o.light.spotAngle, 1.0f, 179.0f) * 0.5f);
        L.cosOuter = std::cos(half);
        L.cosInner = std::cos(half * (1.0f - clampf(o.light.spotBlend, 0.0f, 1.0f)));
        if (L.cosInner <= L.cosOuter) L.cosInner = L.cosOuter + 1e-4f;
        L.radius = L.type == 1 ? settings.lightSize * 0.2f : settings.lightSize;
        L.axisU = transformDir(w, Vec3(o.light.width * 0.5f, 0, 0));
        L.axisV = transformDir(w, Vec3(0, 0, o.light.height * 0.5f));
        out.lights.push_back(L);
    }
    if (out.lights.empty() && settings.studioLights) {
        // Studio preview: a key and a fill "sun" plus a gradient sky.
        if (length(keyDir) < 0.5f) keyDir = normalize(Vec3(-0.4f, 0.75f, 0.55f));
        if (length(fillDir) < 0.5f) fillDir = normalize(Vec3(0.7f, -0.15f, 0.4f));
        Light key;
        key.type = 1;
        key.dir = -keyDir;
        key.color = Vec3(1.0f, 0.97f, 0.92f) * 0.95f;
        key.radius = settings.lightSize * 0.2f;
        Light fill = key;
        fill.dir = -fillDir;
        fill.color = Vec3(0.55f, 0.62f, 0.75f) * 0.30f;
        out.lights.push_back(key);
        out.lights.push_back(fill);
        out.skyLow = Vec3(0.07f, 0.065f, 0.06f) * settings.envStrength;
        out.skyHigh = Vec3(0.17f, 0.19f, 0.23f) * settings.envStrength;
    } else {
        out.skyLow = out.skyHigh = scene.ambient * settings.envStrength;
    }

    out.anyShadowPass = std::find(out.shadowPass.begin(), out.shadowPass.end(), 1) != out.shadowPass.end();

    // 4. Acceleration structure.
    {
        PROF_SCOPE("rt build bvh");
        out.bvh.build(out.verts);
    }
    out.buildMs = (nowSeconds() - t0) * 1000.0;
}

// ---------------------------------------------------------------------------
// Materials
// ---------------------------------------------------------------------------
Vec4 sampleTexture(const Image& img, Vec2 uv) {
    const int w = img.width, h = img.height;
    float x = (uv.x - std::floor(uv.x)) * w - 0.5f, y = (1.0f - (uv.y - std::floor(uv.y))) * h - 0.5f;  // v up
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    float fx = x - x0, fy = y - y0;
    auto texel = [&](int xi, int yi) {
        xi = ((xi % w) + w) % w;
        yi = ((yi % h) + h) % h;
        const uint8_t* p = &img.rgba[((size_t)yi * w + xi) * 4];
        return Vec4(p[0], p[1], p[2], p[3]);
    };
    Vec4 a = texel(x0, y0), b = texel(x0 + 1, y0), c = texel(x0, y0 + 1), d = texel(x0 + 1, y0 + 1);
    Vec4 top = a * (1 - fx) + b * fx, bot = c * (1 - fx) + d * fx;
    return (top * (1 - fy) + bot * fy) * (1.0f / 255.0f);
}

namespace {

inline float luminance(Vec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
inline Vec3 lerp3(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }
inline Vec3 schlick(Vec3 f0, float cosT) {
    float x = clampf(1.0f - cosT, 0.0f, 1.0f), x2 = x * x;
    float m = x2 * x2 * x;
    return f0 + (Vec3(1, 1, 1) - f0) * m;
}
inline float ggxD(float nh, float a2) {
    float d = nh * nh * (a2 - 1.0f) + 1.0f;
    return a2 / (kPi * d * d);
}
inline float smithG1(float x, float k) { return x / (x * (1.0f - k) + k); }
inline float smithG(float nl, float nv, float alpha) {
    float k = alpha * 0.5f;
    return smithG1(nl, k) * smithG1(nv, k);
}
// Exact Fresnel reflectance of a dielectric (unpolarised); eta = n_incident / n_transmitted.
float fresnelDielectric(float cosI, float eta) {
    float sin2T = eta * eta * (1.0f - cosI * cosI);
    if (sin2T >= 1.0f) return 1.0f;  // total internal reflection
    float cosT = std::sqrt(1.0f - sin2T);
    float rs = (eta * cosI - cosT) / (eta * cosI + cosT), rp = (cosI - eta * cosT) / (cosI + eta * cosT);
    return 0.5f * (rs * rs + rp * rp);
}
Vec3 f0Of(const Surface& s) {
    float f = (s.ior - 1.0f) / (s.ior + 1.0f);
    f *= f;
    return lerp3(Vec3(f, f, f), s.baseColor, s.metallic);
}

void basis(Vec3 n, Vec3& t, Vec3& b) {
    // Duff et al. 2017, "Building an orthonormal basis, revisited".
    float sign = n.z >= 0 ? 1.0f : -1.0f;
    float a = -1.0f / (sign + n.z);
    float bb = n.x * n.y * a;
    t = Vec3(1.0f + sign * n.x * n.x * a, sign * bb, -sign * n.x);
    b = Vec3(bb, sign + n.y * n.y * a, -n.y);
}
Vec3 cosineSample(Vec3 n, float u1, float u2) {
    float r = std::sqrt(u1), phi = 2.0f * kPi * u2;
    Vec3 t, b;
    basis(n, t, b);
    return normalize(t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + n * std::sqrt(std::max(0.0f, 1.0f - u1)));
}
// GGX distribution of microfacet normals around n.
Vec3 ggxSample(Vec3 n, float alpha, float u1, float u2) {
    float a2 = alpha * alpha;
    float cos2 = (1.0f - u1) / (1.0f + (a2 - 1.0f) * u1);
    float cosT = std::sqrt(std::max(0.0f, cos2)), sinT = std::sqrt(std::max(0.0f, 1.0f - cos2));
    float phi = 2.0f * kPi * u2;
    Vec3 t, b;
    basis(n, t, b);
    return normalize(t * (sinT * std::cos(phi)) + b * (sinT * std::sin(phi)) + n * cosT);
}
Vec3 uniformSphere(float u1, float u2) {
    float z = 1.0f - 2.0f * u1, r = std::sqrt(std::max(0.0f, 1.0f - z * z)), phi = 2.0f * kPi * u2;
    return {r * std::cos(phi), r * std::sin(phi), z};
}
// Direction in a cone of half-angle `radius` around `axis` (the sun's disc).
Vec3 coneSample(Vec3 axis, float radius, float u1, float u2) {
    float cosMax = std::cos(radius);
    float cosT = 1.0f - u1 * (1.0f - cosMax), sinT = std::sqrt(std::max(0.0f, 1.0f - cosT * cosT));
    float phi = 2.0f * kPi * u2;
    Vec3 t, b;
    basis(axis, t, b);
    return normalize(t * (sinT * std::cos(phi)) + b * (sinT * std::sin(phi)) + axis * cosT);
}
inline float alphaOf(float roughness) { return std::max(0.002f, roughness * roughness); }

Vec3 sky(const SceneData& s, Vec3 d) {
    float t = clampf(d.y * 0.5f + 0.5f, 0.0f, 1.0f);
    return s.skyLow + (s.skyHigh - s.skyLow) * t;
}

// sRGB texels decode through a table before filtering (decode-then-filter
// is also the correct order), instead of three pow() per lookup.
struct SrgbLut {
    float v[256];
    SrgbLut() {
        for (int i = 0; i < 256; ++i) v[i] = std::pow(i / 255.0f, 2.2f);
    }
};
const SrgbLut g_srgb;
Vec4 sampleTextureSrgb(const Image& img, Vec2 uv) {
    const int w = img.width, h = img.height;
    float x = (uv.x - std::floor(uv.x)) * w - 0.5f, y = (1.0f - (uv.y - std::floor(uv.y))) * h - 0.5f;
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    float fx = x - x0, fy = y - y0;
    auto texel = [&](int xi, int yi) {
        xi = ((xi % w) + w) % w;
        yi = ((yi % h) + h) % h;
        const uint8_t* p = &img.rgba[((size_t)yi * w + xi) * 4];
        return Vec4(g_srgb.v[p[0]], g_srgb.v[p[1]], g_srgb.v[p[2]], p[3] * (1.0f / 255.0f));
    };
    Vec4 a = texel(x0, y0), b = texel(x0 + 1, y0), c = texel(x0, y0 + 1), d = texel(x0 + 1, y0 + 1);
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}

// Hit point -> textured surface. `d` is the ray direction; `front` tells
// whether the ray hit the outside of the surface.
Surface surfaceAt(const SceneData& s, int tri, float u, float v, Vec3 d, Vec3& ngFacing, bool& front) {
    const Material& mat = s.materials[s.triMaterial[tri]];
    const Vec3 &a = s.verts[3 * tri], &b = s.verts[3 * tri + 1], &c = s.verts[3 * tri + 2];
    Vec3 ng = normalize(cross(b - a, c - a));
    front = dot(ng, d) < 0.0f;
    float w0 = 1.0f - u - v;
    Vec3 ns = s.normals[3 * tri] * w0 + s.normals[3 * tri + 1] * u + s.normals[3 * tri + 2] * v;
    ns = length(ns) > 1e-8f ? normalize(ns) : ng;
    Vec2 uv = s.uvs[3 * tri] * w0 + s.uvs[3 * tri + 1] * u + s.uvs[3 * tri + 2] * v;
    uv = Vec2(uv.x * mat.uvScale.x, uv.y * mat.uvScale.y);
    Surface sf;
    sf.baseColor = mat.baseColor;
    sf.metallic = mat.metallic;
    sf.roughness = mat.roughness;
    sf.emission = mat.emission;
    sf.opacity = mat.opacity;
    sf.transmission = mat.transmission;
    sf.ior = mat.ior;
    sf.ao = 1.0f;
    auto tex = [&](int slot) -> const Image* {
        int i = mat.tex[slot];
        return i >= 0 ? s.textures[i].get() : nullptr;
    };
    if (const Image* t = tex(TEX_BASE)) {
        Vec4 c4 = sampleTextureSrgb(*t, uv);
        sf.baseColor = mul(sf.baseColor, Vec3(c4.x, c4.y, c4.z));
        sf.opacity *= c4.w;
    }
    if (const Image* t = tex(TEX_ROUGHNESS)) sf.roughness *= sampleTexture(*t, uv).y;
    if (const Image* t = tex(TEX_METALLIC)) sf.metallic *= sampleTexture(*t, uv).z;
    if (const Image* t = tex(TEX_AO)) sf.ao = sampleTexture(*t, uv).x;
    if (const Image* t = tex(TEX_EMISSION)) {
        Vec4 c4 = sampleTextureSrgb(*t, uv);
        sf.emission = mul(sf.emission, Vec3(c4.x, c4.y, c4.z));
    }
    if (const Image* t = tex(TEX_OPACITY)) sf.opacity *= sampleTexture(*t, uv).x;
    if (const Image* t = tex(TEX_NORMAL)) {
        Vec4 nm = sampleTexture(*t, uv);
        Vec3 m(nm.x * 2 - 1, nm.y * 2 - 1, nm.z * 2 - 1);
        m.x *= mat.normalStrength;
        m.y *= mat.normalStrength;
        Vec4 tg = s.tangents[tri];
        Vec3 T(tg.x, tg.y, tg.z);
        T = T - ns * dot(ns, T);
        if (length(T) > 1e-6f) {
            T = normalize(T);
            Vec3 B = cross(ns, T) * tg.w;
            Vec3 n2 = T * m.x + B * m.y + ns * std::max(1e-3f, m.z);
            if (length(n2) > 1e-6f) ns = normalize(n2);
        }
    }
    // Face the ray (two-sided surfaces, like the viewport).
    ngFacing = front ? ng : -ng;
    if (dot(ns, ngFacing) < 0.0f) ns = -ns;
    sf.normal = ns;
    return sf;
}

// Transmittance of a shadow ray: 0 if blocked by an opaque surface;
// glass / alpha / emissive surfaces let (tinted) light through.
Vec3 shadowTransmittance(const SceneData& s, Vec3 o, Vec3 d, float maxT) {
    // Fast path: any opaque blocker (early-out traversal that skips the
    // see-through triangles) means full shadow; scenes without see-through
    // materials are done after that test.
    if (occludedBvh(s.bvh, s.verts, o, d, 1e-4f, maxT, &s.shadowPass)) return Vec3();
    if (!s.anyShadowPass) return Vec3(1, 1, 1);
    Vec3 tr(1, 1, 1);
    for (int k = 0; k < 8; ++k) {
        Hit h;
        h.t = maxT;
        if (!intersectBvh(s.bvh, s.verts, o, d, 1e-4f, h)) return tr;
        if (!s.shadowPass[h.tri]) return Vec3();
        Vec3 ng;
        bool front;
        Surface sf = surfaceAt(s, h.tri, h.u, h.v, d, ng, front);
        bool emissive = (sf.emission.x + sf.emission.y + sf.emission.z) > 0.0f;
        if (!emissive) {
            Vec3 pass = Vec3(1, 1, 1) * (1.0f - sf.opacity) +
                        sf.baseColor * (sf.opacity * sf.transmission * (1.0f - sf.metallic));
            tr = mul(tr, pass);
            if (luminance(tr) < 1e-4f) return Vec3();
        }
        o = o + d * (h.t + 1e-4f);
        maxT -= h.t + 1e-4f;
        if (maxT <= 0.0f) return tr;
    }
    return tr;
}

}  // namespace

Vec3 evalBrdf(const Surface& s, Vec3 n, Vec3 v, Vec3 l) {
    float nl = dot(n, l), nv = std::max(dot(n, v), 1e-4f);
    if (nl <= 0.0f) return Vec3();
    Vec3 h = normalize(l + v);
    float nh = std::max(dot(n, h), 0.0f), vh = std::max(dot(v, h), 0.0f);
    float alpha = alphaOf(s.roughness);
    Vec3 F = schlick(f0Of(s), vh);
    Vec3 spec = F * (ggxD(nh, alpha * alpha) * smithG(nl, nv, alpha) / (4.0f * nl * nv));
    Vec3 kd = mul(Vec3(1, 1, 1) - F, s.baseColor) * ((1.0f - s.metallic) * (1.0f - s.transmission) / kPi);
    return (kd + spec) * kPi;
}

Vec3 trace(const SceneData& s, const Settings& set, Vec3 o, Vec3 d, Rng& rng) {
    Vec3 radiance, throughput(1, 1, 1);
    int bounce = 0;
    auto clampIndirect = [&](Vec3 c) {
        if (bounce == 0 || set.clampIndirect <= 0) return c;
        float m = std::max(c.x, std::max(c.y, c.z));
        return m > set.clampIndirect ? c * (set.clampIndirect / m) : c;
    };
    for (int step = 0; step < 64; ++step) {
        Hit hit;
        bool found = intersectBvh(s.bvh, s.verts, o, d, 1e-4f, hit);
        // Area lights are visible to camera rays (also through glass / alpha).
        if (bounce == 0) {
            for (const Light& L : s.lights) {
                if (L.type != 3) continue;
                float denom = dot(d, L.dir);
                if (denom >= 0.0f) continue;  // seen from behind (one-sided)
                float t = dot(L.pos - o, L.dir) / denom;
                if (t <= 1e-4f || (found && t >= hit.t)) continue;
                Vec3 q = o + d * t - L.pos;
                float lu = dot(q, L.axisU) / std::max(1e-12f, dot(L.axisU, L.axisU));
                float lv = dot(q, L.axisV) / std::max(1e-12f, dot(L.axisV, L.axisV));
                if (std::fabs(lu) <= 1.0f && std::fabs(lv) <= 1.0f) {
                    float area = 4.0f * length(L.axisU) * length(L.axisV);
                    radiance += mul(throughput, L.color * (1.0f / std::max(1e-4f, area)));
                    return radiance;
                }
            }
        }
        if (!found) {
            radiance += clampIndirect(mul(throughput, sky(s, d)));
            break;
        }
        Vec3 ng;
        bool front;
        Surface sf = surfaceAt(s, hit.tri, hit.u, hit.v, d, ng, front);
        const Vec3 p = o + d * hit.t;
        const Vec3 n = sf.normal, v = -d;
        const float scale = 1e-4f * std::max(1.0f, std::max(std::fabs(p.x), std::max(std::fabs(p.y), std::fabs(p.z))));

        // Alpha: the ray continues through the surface unchanged.
        if (sf.opacity < 1.0f && rng.uniform() >= sf.opacity) {
            o = p + d * scale;
            continue;
        }
        radiance += clampIndirect(mul(throughput, sf.emission));

        // Direct light: one shadow ray per lamp.
        const Vec3 origin = p + ng * scale;
        Vec3 direct;
        for (const Light& L : s.lights) {
            Vec3 ldir;
            float att = 1.0f, maxT = 1e30f;
            if (L.type == 1) {
                ldir = L.radius > 0 ? coneSample(-L.dir, L.radius, rng.uniform(), rng.uniform()) : -L.dir;
            } else {
                Vec3 lp;
                if (L.type == 3)
                    lp = L.pos + L.axisU * (2.0f * rng.uniform() - 1.0f) + L.axisV * (2.0f * rng.uniform() - 1.0f);
                else
                    lp = L.pos + uniformSphere(rng.uniform(), rng.uniform()) * L.radius;
                Vec3 dv = lp - p;
                float dist = length(dv);
                if (dist < 1e-5f) continue;
                ldir = dv / dist;
                float r2 = dist * dist / (L.range * L.range);
                float x = clampf(1.0f - r2 * r2, 0.0f, 1.0f);
                att = x * x / (dist * dist + 1.0f);
                if (L.type == 2) {
                    float t = clampf((dot(-ldir, L.dir) - L.cosOuter) / (L.cosInner - L.cosOuter), 0.0f, 1.0f);
                    att *= t * t * (3.0f - 2.0f * t);
                } else if (L.type == 3) {
                    att *= std::max(0.0f, dot(-ldir, L.dir));  // one-sided Lambertian emitter
                }
                maxT = dist - 1e-3f;
            }
            float nl = dot(n, ldir);
            if (nl <= 0 || att <= 0 || dot(ng, ldir) <= 0) continue;
            Vec3 tr = shadowTransmittance(s, origin, ldir, maxT);
            if (tr.x + tr.y + tr.z <= 0.0f) continue;
            direct += mul(mul(evalBrdf(sf, n, v, ldir) * (nl * att), L.color), tr);
        }
        radiance += clampIndirect(mul(throughput, direct));

        if (bounce >= set.maxBounces) break;
        ++bounce;

        // Continue the path: pick one lobe of the material.
        const float nv = std::max(dot(n, v), 1e-4f);
        const float alpha = alphaOf(sf.roughness);
        const Vec3 F0 = f0Of(sf), Fv = schlick(F0, nv);
        const float dielectric = 1.0f - sf.metallic;
        float wTrans = dielectric * sf.transmission;
        float wSpec = luminance(Fv) * (1.0f - wTrans);
        float wDiff = dielectric * (1.0f - sf.transmission) * luminance(sf.baseColor) * (1.0f - luminance(Fv));
        float total = wTrans + wSpec + wDiff;
        if (total <= 1e-6f) break;
        float r = rng.uniform() * total;
        Vec3 weight;
        if (r < wSpec) {
            Vec3 m = ggxSample(n, alpha, rng.uniform(), rng.uniform());
            Vec3 l = normalize(m * (2.0f * dot(v, m)) - v);
            float nl = dot(n, l), vm = std::max(dot(v, m), 1e-4f), nm = std::max(dot(n, m), 1e-4f);
            if (nl <= 0.0f || dot(ng, l) <= 0.0f) break;
            weight = schlick(F0, vm) * (smithG(nl, nv, alpha) * vm / (nv * nm)) * (total / wSpec);
            d = l;
            o = origin;
        } else if (r < wSpec + wDiff) {
            Vec3 l = cosineSample(n, rng.uniform(), rng.uniform());
            if (dot(ng, l) <= 0.0f) break;
            weight = mul(Vec3(1, 1, 1) - Fv, sf.baseColor) * (dielectric * (1.0f - sf.transmission) * total / wDiff);
            d = l;
            o = origin;
        } else {
            // Glass: reflect or refract at a GGX microfacet (rough glass).
            Vec3 m = ggxSample(n, alpha, rng.uniform(), rng.uniform());
            float cosI = dot(v, m);
            if (cosI <= 0.0f) m = n, cosI = nv;
            const float eta = front ? 1.0f / sf.ior : sf.ior;
            float Fr = fresnelDielectric(cosI, eta);
            if (rng.uniform() < Fr) {
                d = normalize(m * (2.0f * cosI) - v);
                o = origin;
                weight = Vec3(1, 1, 1) * (total / wTrans);
            } else {
                float sin2T = eta * eta * (1.0f - cosI * cosI);
                float cosT = std::sqrt(std::max(0.0f, 1.0f - sin2T));
                d = normalize(-v * eta + m * (eta * cosI - cosT));
                o = p - ng * scale;  // continue on the far side
                weight = sf.baseColor * (total / wTrans);
            }
        }
        throughput = mul(throughput, weight);
        // Russian roulette (unbiased path termination).
        if (bounce >= 3) {
            float q = std::min(0.95f, std::max(throughput.x, std::max(throughput.y, throughput.z)));
            if (q <= 0.0f || rng.uniform() > q) break;
            throughput = throughput / q;
        }
    }
    return radiance;
}

Vec3 toDisplay(Vec3 c, float exposure) {
    Vec3 out;
    for (int i = 0; i < 3; ++i) {
        float x = std::max(0.0f, c[i] * exposure);
        if (x > 0.8f) x = 0.8f + 0.2f * (1.0f - std::exp(-(x - 0.8f) / 0.2f));
        out[i] = std::pow(std::min(x, 1.0f), 1.0f / 2.2f);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Progressive CPU renderer
// ---------------------------------------------------------------------------
CpuRenderer::~CpuRenderer() { stop(); }

void CpuRenderer::start(std::shared_ptr<const SceneData> scene, const View& view, const Settings& settings, int w,
                        int h, int targetSamples) {
    stop();
    scene_ = std::move(scene);
    view_ = view;
    settings_ = settings;
    w_ = std::max(1, w);
    h_ = std::max(1, h);
    target_ = targetSamples;
    accum_.assign((size_t)w_ * h_, Accum());
    samples_ = imageSamples_ = 0;
    image_.assign((size_t)w_ * h_ * 4, 0);
    imageDirty_ = true;
    lastResolve_ = 0;
    running_ = true;
    resumable_ = true;
    startTime_ = nowSeconds();
    endTime_ = 0;
    launchPass();
}

void CpuRenderer::stop() {
    resumable_ = false;
    if (passActive_) {
        group_.cancel = true;
        jobs::wait(group_);
        group_.cancel = false;
        passActive_ = false;
    }
    if (running_) {
        endTime_ = nowSeconds();
        if (samples_ > imageSamples_) {  // show everything rendered so far
            resolveInto(image_);
            imageSamples_ = samples_;
            imageDirty_ = true;
        }
    }
    running_ = false;
}

void CpuRenderer::launchPass() {
    const int tile = 32;
    const int tilesX = (w_ + tile - 1) / tile, tilesY = (h_ + tile - 1) / tile;
    const int sample = samples_;
    passActive_ = true;
    for (int ty = 0; ty < tilesY; ++ty)
        for (int tx = 0; tx < tilesX; ++tx) {
            jobs::run(group_, [this, tx, ty, tile, sample] {
                if (group_.cancel) return;
                const int x0 = tx * tile, y0 = ty * tile;
                const int x1 = std::min(w_, x0 + tile), y1 = std::min(h_, y0 + tile);
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x) {
                        uint64_t seed = ((uint64_t)(y * w_ + x) << 20) ^ (uint64_t)sample * 0x9E3779B97F4A7C15ull;
                        Rng rng(seed);
                        Vec3 o, d;
                        float sx = (x + rng.uniform()) / w_, sy = (y + rng.uniform()) / h_;
                        float lu = rng.uniform(), lv = rng.uniform();
                        primaryRay(view_, sx, sy, lu, lv, o, d);
                        Vec3 c = trace(*scene_, settings_, o, d, rng);
                        if (!(c.x == c.x) || !(c.y == c.y) || !(c.z == c.z)) c = Vec3();  // NaN guard
                        Accum& acc = accum_[(size_t)y * w_ + x];
                        acc.c += c;
                        acc.n += 1.0f;
                    }
            });
        }
}

bool CpuRenderer::update() {
    if (!running_ || !passActive_ || !group_.done()) return false;
    passActive_ = false;
    ++samples_;
    const bool finished = samples_ >= target_;
    // Tone-map for display while no pass is writing the accumulation buffer
    // (at most ~10 times a second, always for the final sample).
    const double now = nowSeconds();
    if (finished || samples_ == 1 || now - lastResolve_ > 0.1) {
        resolveInto(image_);
        lastResolve_ = now;
        imageSamples_ = samples_;
        imageDirty_ = true;
    }
    if (finished) {
        running_ = false;
        endTime_ = now;
        return true;
    }
    launchPass();
    return true;
}

double CpuRenderer::elapsedSeconds() const {
    if (startTime_ == 0) return 0;
    return (running_ || endTime_ == 0 ? nowSeconds() : endTime_) - startTime_;
}

double CpuRenderer::samplesPerSecond() const {
    double t = elapsedSeconds();
    return t > 0 ? (double)samples_ * w_ * h_ / t : 0.0;
}

Vec3 CpuRenderer::pixel(int x, int y) const {
    const Accum& a = accum_[(size_t)y * w_ + x];
    return a.n > 0 ? a.c / a.n : Vec3();
}

void CpuRenderer::resolveInto(std::vector<uint8_t>& rgba) const {
    rgba.resize((size_t)w_ * h_ * 4);
    jobs::parallelFor(0, h_, 16, [&](int b, int e) {
        for (int y = b; y < e; ++y)
            for (int x = 0; x < w_; ++x) {
                size_t i = (size_t)y * w_ + x;
                // Per-pixel counts keep tiles of a cancelled pass correct.
                const Accum& a = accum_[i];
                Vec3 c = toDisplay(a.n > 0 ? a.c / a.n : Vec3(), view_.exposure);
                rgba[4 * i + 0] = (uint8_t)std::lround(c.x * 255.0f);
                rgba[4 * i + 1] = (uint8_t)std::lround(c.y * 255.0f);
                rgba[4 * i + 2] = (uint8_t)std::lround(c.z * 255.0f);
                rgba[4 * i + 3] = 255;
            }
    });
}

// ---------------------------------------------------------------------------
// Packing for the GPU tracers
// ---------------------------------------------------------------------------
void packScene(const SceneData& s, int maxTextureSize, PackedScene& out) {
    PROF_SCOPE("rt pack scene");
    const size_t n = s.triangleCount();
    out.tris.assign(std::max<size_t>(1, n) * kTriTexels * 4, 0.0f);
    jobs::parallelFor(0, (int)n, 4096, [&](int b, int e) {
        for (int k = b; k < e; ++k) {
            const int t = s.bvh.order[k];
            float* o = &out.tris[(size_t)k * kTriTexels * 4];
            auto put = [&](int texel, Vec3 v, float w) {
                o[texel * 4] = v.x;
                o[texel * 4 + 1] = v.y;
                o[texel * 4 + 2] = v.z;
                o[texel * 4 + 3] = w;
            };
            const Vec4 tg = s.tangents[t];
            put(0, s.verts[3 * t], (float)s.triMaterial[t]);
            put(1, s.verts[3 * t + 1], tg.w);
            put(2, s.verts[3 * t + 2], s.shadowPass[t] ? 1.0f : 0.0f);
            put(3, s.normals[3 * t], s.uvs[3 * t].x);
            put(4, s.normals[3 * t + 1], s.uvs[3 * t].y);
            put(5, s.normals[3 * t + 2], s.uvs[3 * t + 1].x);
            put(6, Vec3(tg.x, tg.y, tg.z), s.uvs[3 * t + 1].y);
            put(7, Vec3(s.uvs[3 * t + 2].x, s.uvs[3 * t + 2].y, 0), 0);
        }
    });
    out.nodes.clear();
    for (const BvhNode& nd : s.bvh.nodes)
        out.nodes.insert(out.nodes.end(), {nd.lo.x, nd.lo.y, nd.lo.z, (float)nd.leftOrFirst, nd.hi.x, nd.hi.y, nd.hi.z,
                                           (float)nd.count});
    if (out.nodes.empty()) out.nodes = {1e30f, 1e30f, 1e30f, 0, -1e30f, -1e30f, -1e30f, 0};  // never hit
    out.mats.clear();
    for (const Material& m : s.materials) {
        auto L = [&](int slot) { return (float)m.tex[slot]; };
        out.mats.insert(out.mats.end(),
                        {m.baseColor.x, m.baseColor.y, m.baseColor.z, m.metallic, m.emission.x, m.emission.y,
                         m.emission.z, m.roughness, m.opacity, m.transmission, m.ior, m.normalStrength, m.uvScale.x,
                         m.uvScale.y, L(TEX_BASE), L(TEX_NORMAL), L(TEX_ROUGHNESS), L(TEX_METALLIC), L(TEX_AO),
                         L(TEX_EMISSION), L(TEX_OPACITY), 0, 0, 0});
    }
    if (out.mats.empty()) out.mats.assign(kMatTexels * 4, 0.0f);
    out.lights.clear();
    for (const Light& L : s.lights)
        out.lights.insert(out.lights.end(), {L.pos.x, L.pos.y, L.pos.z, (float)L.type, L.dir.x, L.dir.y, L.dir.z, L.range,
                                             L.color.x, L.color.y, L.color.z, L.radius, L.axisU.x, L.axisU.y, L.axisU.z,
                                             L.cosInner, L.axisV.x, L.axisV.y, L.axisV.z, L.cosOuter});
    out.lightCount = (int)s.lights.size();
    out.anyShadowPass = s.anyShadowPass;
    if (out.lights.empty()) out.lights.assign(kLightTexels * 4, 0.0f);
    // Textures: one square size for all layers (largest power of two in use, capped).
    int size = 1;
    for (const auto& img : s.textures)
        while (size < std::max(img->width, img->height) && size < maxTextureSize) size *= 2;
    out.texSize = size;
    out.texLayers = (int)s.textures.size();
    out.texels.assign((size_t)size * size * 4 * std::max(1, out.texLayers), 255);
    for (int l = 0; l < out.texLayers; ++l) {
        const Image& img = *s.textures[l];
        uint8_t* dst = &out.texels[(size_t)l * size * size * 4];
        jobs::parallelFor(0, size, 16, [&](int b, int e) {
            for (int y = b; y < e; ++y)
                for (int x = 0; x < size; ++x) {
                    // Box-filter down (or bilinear up) to the layer size.
                    const float sx = (float)img.width / size, sy = (float)img.height / size;
                    Vec4 acc;
                    int count = 0;
                    const int fx = std::max(1, (int)sx), fy = std::max(1, (int)sy);
                    for (int j = 0; j < fy; ++j)
                        for (int i = 0; i < fx; ++i) {
                            float u = ((x * sx) + i + 0.5f) / img.width, v = ((y * sy) + j + 0.5f) / img.height;
                            acc = acc + sampleTexture(img, Vec2(u, 1.0f - v));
                            ++count;
                        }
                    acc = acc * (255.0f / count);
                    uint8_t* p = &dst[((size_t)y * size + x) * 4];
                    p[0] = (uint8_t)std::lround(clampf(acc.x, 0, 255));
                    p[1] = (uint8_t)std::lround(clampf(acc.y, 0, 255));
                    p[2] = (uint8_t)std::lround(clampf(acc.z, 0, 255));
                    p[3] = (uint8_t)std::lround(clampf(acc.w, 0, 255));
                }
        });
    }
}

}  // namespace rt
