#include "gpu_tracer.h"

#include "gl.h"
#include "profiler.h"
#include "renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

// ============================================================================
// GpuTimer
// ============================================================================
void GpuTimer::init() {
    if (ok_) return;
    gl::GenQueries(kRing, q_);
    ok_ = q_[0] != 0;
}

void GpuTimer::shutdown() {
    if (ok_) gl::DeleteQueries(kRing, q_);
    ok_ = false;
    open_ = -1;
    for (bool& p : pending_) p = false;
}

void GpuTimer::begin() {
    if (!ok_ || open_ >= 0) return;
    poll();
    if (pending_[next_]) return;  // ring full (GPU far behind): skip this measurement
    open_ = next_;
    gl::BeginQuery(GL_TIME_ELAPSED, q_[open_]);
}

void GpuTimer::end(double work) {
    if (open_ < 0) return;
    gl::EndQuery(GL_TIME_ELAPSED);
    pending_[open_] = true;
    work_[open_] = work;
    next_ = (open_ + 1) % kRing;
    open_ = -1;
}

void GpuTimer::poll() {
    if (!ok_) return;
    // Results arrive in submission order; collect every finished one.
    for (int k = 0; k < kRing; ++k) {
        int i = (next_ + k) % kRing;
        if (!pending_[i] || i == open_) continue;
        GLint available = 0;
        gl::GetQueryObjectiv(q_[i], GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available) break;
        GLuint64 ns = 0;
        gl::GetQueryObjectui64v(q_[i], GL_QUERY_RESULT, &ns);
        pending_[i] = false;
        lastMs_ = (double)ns * 1e-6;
        lastWork_ = work_[i];
        avgMs_ = avgMs_ <= 0 ? lastMs_ : avgMs_ * 0.9 + lastMs_ * 0.1;
    }
}

// ============================================================================
// GpuTracer
// ============================================================================
namespace {

const char* kTraceVS = R"(#version 330 core
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// The same algorithm as rt::trace (pathtracer.cpp), in GLSL 3.30.
const char* kTraceFS = R"(#version 330 core
#define MAX_LIGHTS 10
uniform sampler2D uTris;   // 6 texels per triangle: v0 (w = material), v1, v2, n0, n1, n2
uniform sampler2D uNodes;  // 2 texels per BVH node: (lo, leftOrFirst), (hi, count)
uniform sampler2D uMats;   // 2 texels per material: (albedo, specK), (emission, shininess)
uniform uint uSeed;
uniform vec2 uSize;
uniform vec3 uEye, uForward, uRight, uUp;
uniform float uTanHalf, uOrthoHalf, uAspect;
uniform int uOrtho;
uniform int uMaxBounces;
uniform float uClamp;
uniform vec3 uSkyLow, uSkyHigh;
uniform int uLightCount;
uniform vec3 uLightPos[MAX_LIGHTS];
uniform vec3 uLightDir[MAX_LIGHTS];
uniform vec3 uLightColor[MAX_LIGHTS];
uniform vec4 uLightParams[MAX_LIGHTS];  // type, range, cos(inner), cos(outer)
uniform float uLightRadius[MAX_LIGHTS];
out vec4 fragColor;

uint rngState;
uint pcg() {
    rngState = rngState * 747796405u + 2891336453u;
    uint w = ((rngState >> ((rngState >> 28u) + 4u)) ^ rngState) * 277803737u;
    return (w >> 22u) ^ w;
}
float rnd() { return float(pcg() >> 8) * (1.0 / 16777216.0); }

vec4 fetch(sampler2D t, int i) {
    int w = textureSize(t, 0).x;
    return texelFetch(t, ivec2(i % w, i / w), 0);
}

const float PI = 3.14159265358979;
void basis(vec3 n, out vec3 t, out vec3 b) {
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float bb = n.x * n.y * a;
    t = vec3(1.0 + s * n.x * n.x * a, s * bb, -s * n.x);
    b = vec3(bb, s + n.y * n.y * a, -n.y);
}
vec3 cosineSample(vec3 n) {
    float u1 = rnd(), u2 = rnd();
    float r = sqrt(u1), phi = 2.0 * PI * u2;
    vec3 t, b;
    basis(n, t, b);
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u1)));
}
vec3 phongSample(vec3 axis, float e) {
    float cosT = pow(rnd(), 1.0 / (e + 1.0));
    float sinT = sqrt(max(0.0, 1.0 - cosT * cosT)), phi = 2.0 * PI * rnd();
    vec3 t, b;
    basis(axis, t, b);
    return normalize(t * (sinT * cos(phi)) + b * (sinT * sin(phi)) + axis * cosT);
}
vec3 uniformSphere() {
    float z = 1.0 - 2.0 * rnd(), r = sqrt(max(0.0, 1.0 - z * z)), phi = 2.0 * PI * rnd();
    return vec3(r * cos(phi), r * sin(phi), z);
}

bool hitBox(vec3 lo, vec3 hi, vec3 o, vec3 invD, float tMax, out float tEnter) {
    vec3 t0 = (lo - o) * invD, t1 = (hi - o) * invD;
    vec3 tn = min(t0, t1), tf = max(t0, t1);
    float a = max(max(tn.x, tn.y), max(tn.z, 0.0));
    float b = min(min(tf.x, tf.y), min(tf.z, tMax));
    tEnter = a;
    return a <= b;
}

// Returns the nearest hit (t, u, v, triangle) or t < 0. anyHit = shadow ray.
vec4 traceRay(vec3 o, vec3 d, float tMax, bool anyHit, out int triOut) {
    vec3 invD = 1.0 / mix(d, vec3(1e-20), lessThan(abs(d), vec3(1e-20)));
    int stack[48];
    int sp = 0;
    stack[sp++] = 0;
    float bestT = tMax, bu = 0.0, bv = 0.0;
    triOut = -1;
    float tEnter;
    while (sp > 0) {
        int ni = stack[--sp];
        vec4 a = fetch(uNodes, 2 * ni), b = fetch(uNodes, 2 * ni + 1);
        if (!hitBox(a.xyz, b.xyz, o, invD, bestT, tEnter)) continue;
        int first = int(a.w), count = int(b.w);
        if (count > 0) {
            for (int i = first; i < first + count; ++i) {
                vec3 v0 = fetch(uTris, 6 * i).xyz, v1 = fetch(uTris, 6 * i + 1).xyz, v2 = fetch(uTris, 6 * i + 2).xyz;
                vec3 e1 = v1 - v0, e2 = v2 - v0, p = cross(d, e2);
                float det = dot(e1, p);
                if (abs(det) < 1e-12) continue;
                float inv = 1.0 / det;
                vec3 s = o - v0;
                float u = dot(s, p) * inv;
                if (u < 0.0 || u > 1.0) continue;
                vec3 q = cross(s, e1);
                float v = dot(d, q) * inv;
                if (v < 0.0 || u + v > 1.0) continue;
                float t = dot(e2, q) * inv;
                if (t > 1e-4 && t < bestT) {
                    // Shadow rays pass through emissive surfaces (lamp shades), as on the CPU.
                    if (anyHit) {
                        vec4 em = fetch(uMats, 2 * int(fetch(uTris, 6 * i).w) + 1);
                        if (em.r + em.g + em.b > 0.0) continue;
                    }
                    bestT = t;
                    bu = u;
                    bv = v;
                    triOut = i;
                    if (anyHit) return vec4(t, u, v, 1.0);
                }
            }
        } else if (sp < 46) {
            int l = first;
            vec4 la = fetch(uNodes, 2 * l), lb = fetch(uNodes, 2 * l + 1);
            vec4 ra = fetch(uNodes, 2 * l + 2), rb = fetch(uNodes, 2 * l + 3);
            float tl, tr;
            bool hl = hitBox(la.xyz, lb.xyz, o, invD, bestT, tl);
            bool hr = hitBox(ra.xyz, rb.xyz, o, invD, bestT, tr);
            if (hl && hr) {
                if (tl < tr) { stack[sp++] = l + 1; stack[sp++] = l; }
                else { stack[sp++] = l; stack[sp++] = l + 1; }
            } else if (hl) {
                stack[sp++] = l;
            } else if (hr) {
                stack[sp++] = l + 1;
            }
        }
    }
    return triOut >= 0 ? vec4(bestT, bu, bv, 1.0) : vec4(-1.0);
}

vec3 sky(vec3 d) { return mix(uSkyLow, uSkyHigh, clamp(d.y * 0.5 + 0.5, 0.0, 1.0)); }
vec3 clampC(vec3 c, bool indirect) {
    if (!indirect || uClamp <= 0.0) return c;
    float m = max(c.x, max(c.y, c.z));
    return m > uClamp ? c * (uClamp / m) : c;
}

vec3 tracePath(vec3 o, vec3 d) {
    vec3 radiance = vec3(0.0), throughput = vec3(1.0);
    for (int bounce = 0; bounce < 64; ++bounce) {
        int tri;
        vec4 h = traceRay(o, d, 1e30, false, tri);
        if (h.x < 0.0) {
            radiance += clampC(throughput * sky(d), bounce > 0);
            break;
        }
        vec4 t0 = fetch(uTris, 6 * tri), t1 = fetch(uTris, 6 * tri + 1), t2 = fetch(uTris, 6 * tri + 2);
        int m = int(t0.w);
        vec4 m0 = fetch(uMats, 2 * m), m1 = fetch(uMats, 2 * m + 1);
        vec3 albedo = m0.rgb, emission = m1.rgb;
        float specK = m0.a, shininess = m1.a;
        vec3 ng = normalize(cross(t1.xyz - t0.xyz, t2.xyz - t0.xyz));
        float w0 = 1.0 - h.y - h.z;
        vec3 ns = fetch(uTris, 6 * tri + 3).xyz * w0 + fetch(uTris, 6 * tri + 4).xyz * h.y +
                  fetch(uTris, 6 * tri + 5).xyz * h.z;
        ns = length(ns) > 1e-8 ? normalize(ns) : ng;
        if (dot(ng, d) > 0.0) ng = -ng;
        if (dot(ns, ng) < 0.0) ns = -ns;
        vec3 p = o + d * h.x;
        vec3 v = -d;
        vec3 ap = abs(p);
        vec3 origin = p + ng * (1e-4 * max(1.0, max(ap.x, max(ap.y, ap.z))));

        radiance += clampC(throughput * emission, bounce > 0);

        vec3 direct = vec3(0.0);
        for (int i = 0; i < MAX_LIGHTS; ++i) {
            if (i >= uLightCount) break;
            vec4 prm = uLightParams[i];
            vec3 L;
            float att = 1.0, maxT = 1e30;
            if (prm.x > 0.5 && prm.x < 1.5) {
                float r = uLightRadius[i];
                L = r > 0.0 ? phongSample(-uLightDir[i], 2.0 / max(1e-6, r * r)) : -uLightDir[i];
            } else {
                vec3 lp = uLightPos[i] + uniformSphere() * uLightRadius[i];
                vec3 dv = lp - p;
                float dist = length(dv);
                if (dist < 1e-5) continue;
                L = dv / dist;
                float x = clamp(1.0 - pow(dist / prm.y, 4.0), 0.0, 1.0);
                att = x * x / (dist * dist + 1.0);
                if (prm.x > 1.5) att *= smoothstep(prm.w, prm.z, dot(-L, uLightDir[i]));
                maxT = dist - 1e-3;
            }
            float ndl = dot(ns, L);
            if (ndl <= 0.0 || att <= 0.0 || dot(ng, L) <= 0.0) continue;
            int st;
            if (traceRay(origin, L, maxT, true, st).x >= 0.0) continue;
            vec3 hv = normalize(L + v);
            float spec = pow(max(dot(ns, hv), 0.0), shininess) * specK;
            direct += (albedo * ndl + vec3(spec)) * uLightColor[i] * att;
        }
        radiance += clampC(throughput * direct, bounce > 0);

        if (bounce >= uMaxBounces) break;
        if (rnd() < specK) {
            d = phongSample(normalize(reflect(d, ns)), shininess);
            if (dot(d, ng) <= 0.0) break;
        } else {
            d = cosineSample(ns);
            if (dot(d, ng) <= 0.0) break;
            throughput *= albedo;
        }
        o = origin;
        if (bounce >= 2) {
            float q = min(0.95, max(throughput.x, max(throughput.y, throughput.z)));
            if (q <= 0.0 || rnd() > q) break;
            throughput /= q;
        }
    }
    return radiance;
}

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    int rowTop = int(uSize.y) - 1 - px.y;  // image rows are counted from the top
    rngState = uint(px.x) * 1973u + uint(rowTop) * 9277u + uSeed * 26699u;
    rngState = rngState * 747796405u + 1u;
    pcg();
    float sx = (float(px.x) + rnd()) / uSize.x, sy = (float(rowTop) + rnd()) / uSize.y;
    float ppx = (2.0 * sx - 1.0) * uAspect, ppy = 1.0 - 2.0 * sy;
    vec3 o, d;
    if (uOrtho != 0) {
        o = uEye + uRight * (ppx * uOrthoHalf) + uUp * (ppy * uOrthoHalf) - uForward * 1000.0;
        d = uForward;
    } else {
        o = uEye;
        d = normalize(uForward + uRight * (ppx * uTanHalf) + uUp * (ppy * uTanHalf));
    }
    vec3 c = tracePath(o, d);
    if (any(isnan(c)) || any(isinf(c))) c = vec3(0.0);
    fragColor = vec4(c, 1.0);  // additive blend: rgb sums, alpha counts samples
}
)";

// Shows a render in the viewport: either the GPU accumulation buffer
// (sum / count, then the display transform) or a CPU result uploaded as RGBA8.
const char* kPresentVS = R"(#version 330 core
out vec2 vUV;
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
const char* kPresentFS = R"(#version 330 core
in vec2 vUV;
uniform sampler2D uImage;
uniform int uAccum;
out vec4 fragColor;
void main() {
    if (uAccum != 0) {
        vec4 a = texture(uImage, vUV);
        vec3 c = a.a > 0.0 ? a.rgb / a.a : vec3(0.0);
        c = max(c, vec3(0.0));
        c = mix(c, 0.8 + 0.2 * (1.0 - exp(-(c - 0.8) / 0.2)), step(0.8, c));
        fragColor = vec4(pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0);
    } else {
        fragColor = vec4(texture(uImage, vec2(vUV.x, 1.0 - vUV.y)).rgb, 1.0);  // rows stored top-down
    }
}
)";

}  // namespace

void GpuTracer::uploadImage(const std::vector<uint8_t>& rgba, int w, int h) {
    if (!imageTex_) gl::GenTextures(1, &imageTex_);
    gl::BindTexture(GL_TEXTURE_2D, imageTex_);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl::PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (w != imageW_ || h != imageH_) {
        gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        imageW_ = w;
        imageH_ = h;
    } else {
        gl::TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    }
}

void GpuTracer::present(bool accumulated, int x, int y, int w, int h) {
    unsigned tex = accumulated ? accumTex_ : imageTex_;
    if (!tex || !presentProg_) return;
    gl::Viewport(x, y, w, h);
    gl::Disable(GL_DEPTH_TEST);
    gl::Disable(GL_BLEND);
    gl::UseProgram(presentProg_);
    gl::BindVertexArray(vao_);
    gl::ActiveTexture(GL_TEXTURE0);
    gl::BindTexture(GL_TEXTURE_2D, tex);
    if (accumulated) {  // nearest = exact pixels when the render is 100 % size; linear otherwise
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    gl::Uniform1i(presentU_.image, 0);
    gl::Uniform1i(presentU_.accum, accumulated ? 1 : 0);
    gl::DrawArrays(GL_TRIANGLES, 0, 3);
    gl::BindVertexArray(0);
    gl::Enable(GL_BLEND);
}

bool GpuTracer::init(std::string& error) {
    presentProg_ = linkGlProgram(kPresentVS, kPresentFS, error);
    if (!presentProg_) return false;
    presentU_.image = gl::GetUniformLocation(presentProg_, "uImage");
    presentU_.accum = gl::GetUniformLocation(presentProg_, "uAccum");
    gl::GenVertexArrays(1, &vao_);
    prog_ = linkGlProgram(kTraceVS, kTraceFS, error);
    if (!prog_) return false;
    auto loc = [&](const char* n) { return gl::GetUniformLocation(prog_, n); };
    u_.tris = loc("uTris");
    u_.nodes = loc("uNodes");
    u_.mats = loc("uMats");
    u_.seed = loc("uSeed");
    u_.size = loc("uSize");
    u_.eye = loc("uEye");
    u_.forward = loc("uForward");
    u_.right = loc("uRight");
    u_.up = loc("uUp");
    u_.tanHalf = loc("uTanHalf");
    u_.orthoHalf = loc("uOrthoHalf");
    u_.ortho = loc("uOrtho");
    u_.aspect = loc("uAspect");
    u_.maxBounces = loc("uMaxBounces");
    u_.clamp = loc("uClamp");
    u_.skyLow = loc("uSkyLow");
    u_.skyHigh = loc("uSkyHigh");
    u_.lightCount = loc("uLightCount");
    for (int i = 0; i < 10; ++i) {
        char n[48];
        std::snprintf(n, sizeof n, "uLightPos[%d]", i);
        u_.lightPos[i] = loc(n);
        std::snprintf(n, sizeof n, "uLightDir[%d]", i);
        u_.lightDir[i] = loc(n);
        std::snprintf(n, sizeof n, "uLightColor[%d]", i);
        u_.lightColor[i] = loc(n);
        std::snprintf(n, sizeof n, "uLightParams[%d]", i);
        u_.lightParams[i] = loc(n);
        std::snprintf(n, sizeof n, "uLightRadius[%d]", i);
        u_.lightRadius[i] = loc(n);
    }
    gl::GenFramebuffers(1, &fbo_);
    timer_.init();
    return true;
}

void GpuTracer::shutdown() {
    unsigned texs[5] = {accumTex_, triTex_, nodeTex_, matTex_, imageTex_};
    for (unsigned t : texs)
        if (t) gl::DeleteTextures(1, &t);
    accumTex_ = triTex_ = nodeTex_ = matTex_ = imageTex_ = 0;
    imageW_ = imageH_ = 0;
    if (fbo_) gl::DeleteFramebuffers(1, &fbo_);
    if (vao_) gl::DeleteVertexArrays(1, &vao_);
    if (prog_) gl::DeleteProgram(prog_);
    if (presentProg_) gl::DeleteProgram(presentProg_);
    fbo_ = vao_ = prog_ = presentProg_ = 0;
    timer_.shutdown();
}

unsigned GpuTracer::uploadTexture(unsigned tex, const std::vector<float>& texels, int& widthOut) {
    GLint maxSize = 4096;
    gl::GetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
    const int count = std::max(1, (int)(texels.size() / 4));
    const int w = std::min(count, std::min(4096, (int)maxSize));
    const int h = (count + w - 1) / w;
    std::vector<float> padded(texels);
    padded.resize((size_t)w * h * 4, 0.0f);
    if (!tex) gl::GenTextures(1, &tex);
    gl::BindTexture(GL_TEXTURE_2D, tex);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl::PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, w, h, 0, GL_RGBA, GL_FLOAT, padded.data());
    uploadedBytes_ += padded.size() * sizeof(float);
    widthOut = w;
    return tex;
}

void GpuTracer::start(const rt::SceneData& scene, const rt::View& view, const rt::Settings& settings, int w, int h,
                      int targetSamples) {
    PROF_SCOPE("gpu trace upload");
    uploadedBytes_ = 0;
    // Triangles in BVH leaf order, so a leaf's range indexes them directly.
    const size_t tris = scene.triangleCount();
    std::vector<float> t;
    t.reserve(std::max<size_t>(1, tris) * 24);
    for (size_t k = 0; k < tris; ++k) {
        int tri = scene.bvh.order[k];
        for (int c = 0; c < 3; ++c) {
            Vec3 p = scene.verts[3 * tri + c];
            t.insert(t.end(), {p.x, p.y, p.z, c == 0 ? (float)scene.triMaterial[tri] : 0.0f});
        }
        for (int c = 0; c < 3; ++c) {
            Vec3 n = scene.normals[3 * tri + c];
            t.insert(t.end(), {n.x, n.y, n.z, 0.0f});
        }
    }
    if (t.empty()) t.assign(24, 0.0f);
    std::vector<float> nodes;
    for (const BvhNode& n : scene.bvh.nodes)
        nodes.insert(nodes.end(), {n.lo.x, n.lo.y, n.lo.z, (float)n.leftOrFirst, n.hi.x, n.hi.y, n.hi.z, (float)n.count});
    if (nodes.empty()) nodes = {1e30f, 1e30f, 1e30f, 0, -1e30f, -1e30f, -1e30f, 0};  // empty box: never hit
    std::vector<float> mats;
    for (const rt::Material& m : scene.materials)
        mats.insert(mats.end(), {m.albedo.x, m.albedo.y, m.albedo.z, m.specK, m.emission.x, m.emission.y,
                                 m.emission.z, m.shininess});
    if (mats.empty()) mats.assign(8, 0.0f);
    triTex_ = uploadTexture(triTex_, t, triW_);
    nodeTex_ = uploadTexture(nodeTex_, nodes, nodeW_);
    matTex_ = uploadTexture(matTex_, mats, matW_);

    // Float accumulation target.
    if (!accumTex_ || w != w_ || h != h_) {
        if (!accumTex_) gl::GenTextures(1, &accumTex_);
        gl::BindTexture(GL_TEXTURE_2D, accumTex_);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    }
    gl::BindFramebuffer(GL_FRAMEBUFFER, fbo_);
    gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, accumTex_, 0);
    gl::Viewport(0, 0, w, h);
    gl::ClearColor(0, 0, 0, 0);
    gl::Clear(GL_COLOR_BUFFER_BIT);
    gl::BindFramebuffer(GL_FRAMEBUFFER, 0);

    view_ = view;
    settings_ = settings;
    lights_ = scene.lights;
    skyLow_ = scene.skyLow;
    skyHigh_ = scene.skyHigh;
    w_ = w;
    h_ = h;
    target_ = targetSamples;
    samples_ = row_ = 0;
    drawnRowsTotal_ = 0;
    running_ = resumable_ = true;
    startTime_ = rt::nowSeconds();
    endTime_ = 0;
}

void GpuTracer::step(double budgetMs) {
    timer_.poll();
    if (timer_.lastWork() > 0 && timer_.lastMs() > 0) {
        double m = timer_.lastMs() / timer_.lastWork();
        msPerRow_ = msPerRow_ <= 0 ? m : msPerRow_ * 0.7 + m * 0.3;
    }
    if (!running_) return;
    PROF_SCOPE("gpu trace submit");
    // Rows (of one sample each) to submit this frame. Start small until the
    // timer has measured the scene's cost, then fill the budget; cap a single
    // draw at ~1/4 of the budget so no draw monopolises the GPU.
    int budgetRows = msPerRow_ > 0 ? (int)(budgetMs / msPerRow_) : 8;
    budgetRows = std::max(1, std::min(budgetRows, h_ * 64));
    int perDraw = msPerRow_ > 0 ? std::max(1, (int)(budgetMs * 0.25 / msPerRow_)) : 8;

    gl::BindFramebuffer(GL_FRAMEBUFFER, fbo_);
    gl::Viewport(0, 0, w_, h_);
    gl::Disable(GL_DEPTH_TEST);
    gl::Disable(GL_CULL_FACE);
    gl::Enable(GL_BLEND);
    gl::BlendFunc(GL_ONE, GL_ONE);
    gl::Enable(GL_SCISSOR_TEST);
    gl::UseProgram(prog_);
    gl::BindVertexArray(vao_);
    gl::ActiveTexture(GL_TEXTURE0);
    gl::BindTexture(GL_TEXTURE_2D, triTex_);
    gl::ActiveTexture(GL_TEXTURE0 + 1);
    gl::BindTexture(GL_TEXTURE_2D, nodeTex_);
    gl::ActiveTexture(GL_TEXTURE0 + 2);
    gl::BindTexture(GL_TEXTURE_2D, matTex_);
    gl::ActiveTexture(GL_TEXTURE0);
    gl::Uniform1i(u_.tris, 0);
    gl::Uniform1i(u_.nodes, 1);
    gl::Uniform1i(u_.mats, 2);
    gl::Uniform2f(u_.size, (float)w_, (float)h_);
    gl::Uniform3f(u_.eye, view_.eye.x, view_.eye.y, view_.eye.z);
    gl::Uniform3f(u_.forward, view_.forward.x, view_.forward.y, view_.forward.z);
    gl::Uniform3f(u_.right, view_.right.x, view_.right.y, view_.right.z);
    gl::Uniform3f(u_.up, view_.up.x, view_.up.y, view_.up.z);
    gl::Uniform1f(u_.tanHalf, view_.tanHalfFov);
    gl::Uniform1f(u_.orthoHalf, view_.orthoHalfHeight);
    gl::Uniform1i(u_.ortho, view_.ortho ? 1 : 0);
    gl::Uniform1f(u_.aspect, view_.aspect);
    gl::Uniform1i(u_.maxBounces, settings_.maxBounces);
    gl::Uniform1f(u_.clamp, settings_.clampIndirect);
    gl::Uniform3f(u_.skyLow, skyLow_.x, skyLow_.y, skyLow_.z);
    gl::Uniform3f(u_.skyHigh, skyHigh_.x, skyHigh_.y, skyHigh_.z);
    const int lc = std::min(10, (int)lights_.size());
    gl::Uniform1i(u_.lightCount, lc);
    for (int i = 0; i < lc; ++i) {
        const rt::Light& L = lights_[i];
        gl::Uniform3f(u_.lightPos[i], L.pos.x, L.pos.y, L.pos.z);
        gl::Uniform3f(u_.lightDir[i], L.dir.x, L.dir.y, L.dir.z);
        gl::Uniform3f(u_.lightColor[i], L.color.x, L.color.y, L.color.z);
        gl::Uniform4f(u_.lightParams[i], (float)L.type, L.range, L.cosInner, L.cosOuter);
        gl::Uniform1f(u_.lightRadius[i], L.radius);
    }

    timer_.begin();
    int drawn = 0;
    while (budgetRows > 0 && samples_ < target_) {
        int rows = std::min(std::min(perDraw, budgetRows), h_ - row_);
        // Rows count from the top of the image; GL's window y from the bottom.
        gl::Scissor(0, h_ - row_ - rows, w_, rows);
        gl::Uniform1ui(u_.seed, (unsigned)samples_ * 2654435761u + 12345u);
        gl::DrawArrays(GL_TRIANGLES, 0, 3);
        row_ += rows;
        drawn += rows;
        budgetRows -= rows;
        if (row_ >= h_) {
            row_ = 0;
            ++samples_;
        }
    }
    timer_.end(drawn);
    drawnRowsTotal_ += drawn;

    gl::Disable(GL_SCISSOR_TEST);
    gl::Disable(GL_BLEND);
    gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl::BindVertexArray(0);
    if (samples_ >= target_) {
        running_ = false;
        endTime_ = rt::nowSeconds();
    }
}

double GpuTracer::elapsedSeconds() const {
    if (startTime_ == 0) return 0;
    return (running_ || endTime_ == 0 ? rt::nowSeconds() : endTime_) - startTime_;
}

double GpuTracer::samplesPerSecond() const {
    double t = elapsedSeconds();
    return t > 0 ? drawnRowsTotal_ * w_ / t : 0.0;
}

void GpuTracer::readImage(std::vector<uint8_t>& rgba) {
    std::vector<float> px((size_t)w_ * h_ * 4);
    gl::BindFramebuffer(GL_FRAMEBUFFER, fbo_);
    gl::PixelStorei(GL_PACK_ALIGNMENT, 4);
    gl::ReadPixels(0, 0, w_, h_, GL_RGBA, GL_FLOAT, px.data());
    gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
    rgba.resize((size_t)w_ * h_ * 4);
    for (int y = 0; y < h_; ++y)
        for (int x = 0; x < w_; ++x) {
            const float* p = &px[((size_t)(h_ - 1 - y) * w_ + x) * 4];  // flip to top-down
            Vec3 c = p[3] > 0 ? Vec3(p[0], p[1], p[2]) / p[3] : Vec3();
            c = rt::toDisplay(c);
            uint8_t* o = &rgba[((size_t)y * w_ + x) * 4];
            o[0] = (uint8_t)std::lround(c.x * 255.0f);
            o[1] = (uint8_t)std::lround(c.y * 255.0f);
            o[2] = (uint8_t)std::lround(c.z * 255.0f);
            o[3] = 255;
        }
}
