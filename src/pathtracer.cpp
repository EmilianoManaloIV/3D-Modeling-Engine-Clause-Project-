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
    return v;
}

void primaryRay(const View& v, float sx, float sy, Vec3& o, Vec3& d) {
    float px = (2.0f * sx - 1.0f) * v.aspect, py = 1.0f - 2.0f * sy;
    if (v.ortho) {
        // Start well behind the eye so geometry between eye and target is kept.
        o = v.eye + v.right * (px * v.orthoHalfHeight) + v.up * (py * v.orthoHalfHeight) - v.forward * 1000.0f;
        d = v.forward;
    } else {
        o = v.eye;
        d = normalize(v.forward + v.right * (px * v.tanHalfFov) + v.up * (py * v.tanHalfFov));
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
        m.albedo = srgbToLinear(o.color);
        m.specK = 0.03f + (0.6f - 0.03f) * o.gloss;
        m.shininess = 6.0f + (160.0f - 6.0f) * o.gloss * o.gloss;
        m.emission = o.emission * o.emissionStrength;
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
    out.triMaterial.resize(tris);
    jobs::parallelFor(0, (int)items.size(), 1, [&](int b, int e) {
        for (int k = b; k < e; ++k) {
            const Item& it = items[k];
            for (size_t t = 0; t < it.indices.size() / 3; ++t) {
                size_t dst = it.triStart + t;
                for (int c = 0; c < 3; ++c) {
                    const RenderVertex& v = it.verts[it.indices[3 * t + c]];
                    out.verts[3 * dst + c] = v.pos;
                    out.normals[3 * dst + c] = v.normal;
                }
                out.triMaterial[dst] = k;
            }
        }
    });

    // 3. Lights (same parameters as the viewport's Lit mode).
    for (int i = 0; i < (int)scene.objects.size() && out.lights.size() < 8; ++i) {
        const Object& o = scene.objects[i];
        if (o.kind != ObjectKind::Light) continue;
        Mat4 w = scene.world(i);
        Light L;
        L.type = (int)o.light.type;
        L.pos = transformPoint(w, Vec3());
        L.dir = normalize(transformDir(w, Vec3(0, -1, 0)));
        L.color = o.light.color * o.light.intensity;
        L.range = o.light.range;
        float half = toRadians(clampf(o.light.spotAngle, 1.0f, 179.0f) * 0.5f);
        L.cosOuter = std::cos(half);
        L.cosInner = std::cos(half * (1.0f - clampf(o.light.spotBlend, 0.0f, 1.0f)));
        if (L.cosInner <= L.cosOuter) L.cosInner = L.cosOuter + 1e-4f;
        L.radius = L.type == 1 ? settings.lightSize * 0.2f : settings.lightSize;
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

    out.noShadow.resize(tris);
    for (size_t t = 0; t < tris; ++t) {
        const Vec3 e = out.materials[out.triMaterial[t]].emission;
        out.noShadow[t] = (e.x + e.y + e.z) > 0.0f;
    }

    // 4. Acceleration structure.
    {
        PROF_SCOPE("rt build bvh");
        out.bvh.build(out.verts);
    }
    out.buildMs = (nowSeconds() - t0) * 1000.0;
}

// ---------------------------------------------------------------------------
// Path tracing
// ---------------------------------------------------------------------------
namespace {

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

// Direction in a Phong lobe of exponent `e` around `axis`.
Vec3 phongSample(Vec3 axis, float e, float u1, float u2) {
    float cosT = std::pow(u1, 1.0f / (e + 1.0f));
    float sinT = std::sqrt(std::max(0.0f, 1.0f - cosT * cosT)), phi = 2.0f * kPi * u2;
    Vec3 t, b;
    basis(axis, t, b);
    return normalize(t * (sinT * std::cos(phi)) + b * (sinT * std::sin(phi)) + axis * cosT);
}

Vec3 uniformSphere(float u1, float u2) {
    float z = 1.0f - 2.0f * u1, r = std::sqrt(std::max(0.0f, 1.0f - z * z)), phi = 2.0f * kPi * u2;
    return {r * std::cos(phi), r * std::sin(phi), z};
}

Vec3 sky(const SceneData& s, Vec3 d) {
    float t = clampf(d.y * 0.5f + 0.5f, 0.0f, 1.0f);
    return s.skyLow + (s.skyHigh - s.skyLow) * t;
}

}  // namespace

Vec3 trace(const SceneData& s, const Settings& set, Vec3 o, Vec3 d, Rng& rng) {
    Vec3 radiance, throughput(1, 1, 1);
    for (int bounce = 0;; ++bounce) {
        Hit hit;
        if (!intersectBvh(s.bvh, s.verts, o, d, 1e-4f, hit)) {
            Vec3 c = mul(throughput, sky(s, d));
            if (bounce > 0 && set.clampIndirect > 0) {
                float m = std::max(c.x, std::max(c.y, c.z));
                if (m > set.clampIndirect) c = c * (set.clampIndirect / m);
            }
            radiance += c;
            break;
        }
        const int tri = hit.tri;
        const Material& mat = s.materials[s.triMaterial[tri]];
        const Vec3 &a = s.verts[3 * tri], &b = s.verts[3 * tri + 1], &c = s.verts[3 * tri + 2];
        Vec3 ng = normalize(cross(b - a, c - a));
        float w0 = 1.0f - hit.u - hit.v;
        Vec3 ns = s.normals[3 * tri] * w0 + s.normals[3 * tri + 1] * hit.u + s.normals[3 * tri + 2] * hit.v;
        ns = length(ns) > 1e-8f ? normalize(ns) : ng;
        if (dot(ng, d) > 0) ng = -ng;  // two-sided surfaces
        if (dot(ns, ng) < 0) ns = -ns;
        Vec3 p = o + d * hit.t;
        Vec3 v = -d;
        const Vec3 origin = p + ng * (1e-4f * std::max(1.0f, std::max(std::fabs(p.x), std::max(std::fabs(p.y), std::fabs(p.z)))));

        // Emission (light reaching the eye from glowing surfaces).
        Vec3 emitted = mul(throughput, mat.emission);
        if (bounce > 0 && set.clampIndirect > 0) {
            float m = std::max(emitted.x, std::max(emitted.y, emitted.z));
            if (m > set.clampIndirect) emitted = emitted * (set.clampIndirect / m);
        }
        radiance += emitted;

        // Direct light: one shadow ray per lamp, viewport falloff.
        Vec3 direct;
        for (const Light& L : s.lights) {
            Vec3 ldir;
            float att = 1.0f, maxT = 1e30f;
            if (L.type == 1) {
                Vec3 axis = -L.dir;
                ldir = L.radius > 0 ? phongSample(axis, 2.0f / std::max(1e-6f, L.radius * L.radius), rng.uniform(),
                                                  rng.uniform())
                                    : axis;
            } else {
                Vec3 lp = L.pos + uniformSphere(rng.uniform(), rng.uniform()) * L.radius;
                Vec3 dv = lp - p;
                float dist = length(dv);
                if (dist < 1e-5f) continue;
                ldir = dv / dist;
                float x = clampf(1.0f - std::pow(dist / L.range, 4.0f), 0.0f, 1.0f);
                att = x * x / (dist * dist + 1.0f);
                if (L.type == 2) {
                    float cd = dot(-ldir, L.dir);
                    float t = clampf((cd - L.cosOuter) / (L.cosInner - L.cosOuter), 0.0f, 1.0f);
                    att *= t * t * (3.0f - 2.0f * t);
                }
                maxT = dist - 1e-3f;
            }
            float ndl = dot(ns, ldir);
            if (ndl <= 0 || att <= 0 || dot(ng, ldir) <= 0) continue;
            if (occludedBvh(s.bvh, s.verts, origin, ldir, 1e-4f, maxT, &s.noShadow)) continue;
            Vec3 h = normalize(ldir + v);
            float spec = std::pow(std::max(dot(ns, h), 0.0f), mat.shininess) * mat.specK;
            direct += mul(mat.albedo * ndl + Vec3(spec, spec, spec), L.color) * att;
        }
        Vec3 dc = mul(throughput, direct);
        if (bounce > 0 && set.clampIndirect > 0) {
            float m = std::max(dc.x, std::max(dc.y, dc.z));
            if (m > set.clampIndirect) dc = dc * (set.clampIndirect / m);
        }
        radiance += dc;

        if (bounce >= set.maxBounces) break;
        // Continue the path: glossy with probability specK, else diffuse.
        if (rng.uniform() < mat.specK) {
            Vec3 refl = normalize(d - ns * (2.0f * dot(d, ns)));
            d = phongSample(refl, mat.shininess, rng.uniform(), rng.uniform());
            if (dot(d, ng) <= 0) break;
        } else {
            d = cosineSample(ns, rng.uniform(), rng.uniform());
            if (dot(d, ng) <= 0) break;
            throughput = mul(throughput, mat.albedo);
        }
        o = origin;
        // Russian roulette (unbiased path termination).
        if (bounce >= 2) {
            float q = std::min(0.95f, std::max(throughput.x, std::max(throughput.y, throughput.z)));
            if (q <= 0.0f || rng.uniform() > q) break;
            throughput = throughput / q;
        }
    }
    return radiance;
}

Vec3 toDisplay(Vec3 c) {
    Vec3 out;
    for (int i = 0; i < 3; ++i) {
        float x = std::max(0.0f, c[i]);
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
                        primaryRay(view_, (x + rng.uniform()) / w_, (y + rng.uniform()) / h_, o, d);
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

void CpuRenderer::resolveInto(std::vector<uint8_t>& rgba) const {
    rgba.resize((size_t)w_ * h_ * 4);
    jobs::parallelFor(0, h_, 16, [&](int b, int e) {
        for (int y = b; y < e; ++y)
            for (int x = 0; x < w_; ++x) {
                size_t i = (size_t)y * w_ + x;
                // Per-pixel counts keep tiles of a cancelled pass correct.
                const Accum& a = accum_[i];
                Vec3 c = toDisplay(a.n > 0 ? a.c / a.n : Vec3());
                rgba[4 * i + 0] = (uint8_t)std::lround(c.x * 255.0f);
                rgba[4 * i + 1] = (uint8_t)std::lround(c.y * 255.0f);
                rgba[4 * i + 2] = (uint8_t)std::lround(c.z * 255.0f);
                rgba[4 * i + 3] = 255;
            }
    });
}

}  // namespace rt
