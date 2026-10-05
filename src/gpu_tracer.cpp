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

// The same algorithm as rt::trace (pathtracer.cpp), in GLSL 3.30. Data
// layouts: see rt::PackedScene.
const char* kTraceFS = R"(#version 330 core
uniform sampler2D uTris, uNodes, uMats, uLights;
uniform sampler2DArray uTextures;
uniform int uLightCount;
uniform int uAnyPass;  // the scene has see-through triangles
uniform uint uSeed;
uniform vec2 uSize;
uniform vec3 uEye, uForward, uRight, uUp;
uniform float uTanHalf, uOrthoHalf, uAspect, uLensRadius, uFocusDist;
uniform int uOrtho, uBlades;
uniform int uMaxBounces;
uniform float uClamp;
uniform vec3 uSkyLow, uSkyHigh;
out vec4 fragColor;

const float PI = 3.14159265358979;
uint rngState;
uint pcg() {
    rngState = rngState * 747796405u + 2891336453u;
    uint w = ((rngState >> ((rngState >> 28u) + 4u)) ^ rngState) * 277803737u;
    return (w >> 22u) ^ w;
}
float rnd() { return float(pcg() >> 8) * (1.0 / 16777216.0); }
uniform int uWidth;  // all data textures share this row width (texels)
vec4 fetch(sampler2D t, int i) { return texelFetch(t, ivec2(i % uWidth, i / uWidth), 0); }
vec4 tex(int layer, vec2 uv) { return textureLod(uTextures, vec3(uv.x, 1.0 - uv.y, float(layer)), 0.0); }

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
vec3 ggxSample(vec3 n, float alpha) {
    float u1 = rnd(), u2 = rnd();
    float a2 = alpha * alpha;
    float cos2 = (1.0 - u1) / (1.0 + (a2 - 1.0) * u1);
    float cosT = sqrt(max(0.0, cos2)), sinT = sqrt(max(0.0, 1.0 - cos2));
    float phi = 2.0 * PI * u2;
    vec3 t, b;
    basis(n, t, b);
    return normalize(t * (sinT * cos(phi)) + b * (sinT * sin(phi)) + n * cosT);
}
vec3 coneSample(vec3 axis, float radius) {
    float u1 = rnd(), u2 = rnd();
    float cosMax = cos(radius);
    float cosT = 1.0 - u1 * (1.0 - cosMax), sinT = sqrt(max(0.0, 1.0 - cosT * cosT));
    float phi = 2.0 * PI * u2;
    vec3 t, b;
    basis(axis, t, b);
    return normalize(t * (sinT * cos(phi)) + b * (sinT * sin(phi)) + axis * cosT);
}
vec3 uniformSphere() {
    float z = 1.0 - 2.0 * rnd(), r = sqrt(max(0.0, 1.0 - z * z)), phi = 2.0 * PI * rnd();
    return vec3(r * cos(phi), r * sin(phi), z);
}
vec2 sampleAperture(float u1, float u2) {
    if (uBlades < 3) {
        float r = sqrt(u1), phi = 2.0 * PI * u2;
        return vec2(r * cos(phi), r * sin(phi));
    }
    float fb = float(uBlades);
    float k = min(fb - 1.0, floor(u1 * fb));
    float t = u1 * fb - k;
    float a0 = 2.0 * PI * k / fb, a1 = 2.0 * PI * (k + 1.0) / fb;
    return mix(vec2(cos(a0), sin(a0)), vec2(cos(a1), sin(a1)), u2) * sqrt(t);
}

bool hitBox(vec3 lo, vec3 hi, vec3 o, vec3 invD, float tMax, out float tEnter) {
    vec3 t0 = (lo - o) * invD, t1 = (hi - o) * invD;
    vec3 tn = min(t0, t1), tf = max(t0, t1);
    float a = max(max(tn.x, tn.y), max(tn.z, 0.0));
    float b = min(min(tf.x, tf.y), min(tf.z, tMax));
    tEnter = a;
    return a <= b;
}

// Nearest hit: (t, u, v, 1) or t < 0. triOut = packed triangle index.
// anyHit: shadow test - returns at the first opaque triangle (see-through
// ones are skipped).
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
                vec4 v2w = fetch(uTris, 8 * i + 2);
                if (anyHit && v2w.w > 0.5) continue;
                vec3 v0 = fetch(uTris, 8 * i).xyz, v1 = fetch(uTris, 8 * i + 1).xyz, v2 = v2w.xyz;
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

// ---- material (same as rt::surfaceAt / rt::evalBrdf) ----
struct Surface {
    vec3 base, emission, n;
    float metallic, roughness, opacity, transmission, ior;
};
Surface surfaceAt(int tri, vec2 bc, vec3 d, out vec3 ngFacing, out bool front) {
    vec4 t0 = fetch(uTris, 8 * tri), t1 = fetch(uTris, 8 * tri + 1), t2 = fetch(uTris, 8 * tri + 2);
    vec4 n0 = fetch(uTris, 8 * tri + 3), n1 = fetch(uTris, 8 * tri + 4), n2 = fetch(uTris, 8 * tri + 5);
    vec4 tg = fetch(uTris, 8 * tri + 6), u2 = fetch(uTris, 8 * tri + 7);
    int m = int(t0.w);
    vec4 m0 = fetch(uMats, 6 * m), m1 = fetch(uMats, 6 * m + 1), m2 = fetch(uMats, 6 * m + 2);
    vec4 m3 = fetch(uMats, 6 * m + 3), m4 = fetch(uMats, 6 * m + 4), m5 = fetch(uMats, 6 * m + 5);
    vec3 ng = normalize(cross(t1.xyz - t0.xyz, t2.xyz - t0.xyz));
    front = dot(ng, d) < 0.0;
    float w0 = 1.0 - bc.x - bc.y;
    vec3 ns = n0.xyz * w0 + n1.xyz * bc.x + n2.xyz * bc.y;
    ns = length(ns) > 1e-8 ? normalize(ns) : ng;
    vec2 uv = vec2(n0.w, n1.w) * w0 + vec2(n2.w, tg.w) * bc.x + u2.xy * bc.y;
    uv *= m3.xy;
    Surface s;
    s.base = m0.rgb; s.metallic = m0.a;
    s.emission = m1.rgb; s.roughness = m1.a;
    s.opacity = m2.x; s.transmission = m2.y; s.ior = m2.z;
    if (m3.z >= 0.0) { vec4 c = tex(int(m3.z), uv); s.base *= pow(c.rgb, vec3(2.2)); s.opacity *= c.a; }
    if (m4.x >= 0.0) s.roughness *= tex(int(m4.x), uv).g;
    if (m4.y >= 0.0) s.metallic *= tex(int(m4.y), uv).b;
    if (m4.w >= 0.0) s.emission *= pow(tex(int(m4.w), uv).rgb, vec3(2.2));
    if (m5.x >= 0.0) s.opacity *= tex(int(m5.x), uv).r;
    if (m3.w >= 0.0) {
        vec3 mm = tex(int(m3.w), uv).xyz * 2.0 - 1.0;
        mm.xy *= m2.w;
        vec3 T = tg.xyz - ns * dot(ns, tg.xyz);
        if (length(T) > 1e-6) {
            T = normalize(T);
            vec3 B = cross(ns, T) * t1.w;
            vec3 n2v = T * mm.x + B * mm.y + ns * max(1e-3, mm.z);
            if (length(n2v) > 1e-6) ns = normalize(n2v);
        }
    }
    ngFacing = front ? ng : -ng;
    if (dot(ns, ngFacing) < 0.0) ns = -ns;
    s.n = ns;
    return s;
}
float lum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec3 schlick(vec3 f0, float c) { return f0 + (1.0 - f0) * pow(clamp(1.0 - c, 0.0, 1.0), 5.0); }
float smithG(float nl, float nv, float a) {
    float k = a * 0.5;
    return (nl / (nl * (1.0 - k) + k)) * (nv / (nv * (1.0 - k) + k));
}
vec3 f0Of(Surface s) {
    float f = (s.ior - 1.0) / (s.ior + 1.0);
    return mix(vec3(f * f), s.base, s.metallic);
}
vec3 evalBrdf(Surface s, vec3 n, vec3 v, vec3 l) {
    float nl = dot(n, l), nv = max(dot(n, v), 1e-4);
    if (nl <= 0.0) return vec3(0.0);
    vec3 h = normalize(l + v);
    float nh = max(dot(n, h), 0.0), vh = max(dot(v, h), 0.0);
    float a = max(0.002, s.roughness * s.roughness), a2 = a * a;
    float dd = nh * nh * (a2 - 1.0) + 1.0;
    vec3 F = schlick(f0Of(s), vh);
    vec3 spec = F * (a2 / (PI * dd * dd) * smithG(nl, nv, a) / (4.0 * nl * nv));
    vec3 kd = (1.0 - F) * s.base * ((1.0 - s.metallic) * (1.0 - s.transmission) / PI);
    return (kd + spec) * PI;
}
float fresnelDielectric(float cosI, float eta) {
    float sin2T = eta * eta * (1.0 - cosI * cosI);
    if (sin2T >= 1.0) return 1.0;
    float cosT = sqrt(1.0 - sin2T);
    float rs = (eta * cosI - cosT) / (eta * cosI + cosT), rp = (cosI - eta * cosT) / (cosI + eta * cosT);
    return 0.5 * (rs * rs + rp * rp);
}
vec3 sky(vec3 d) { return mix(uSkyLow, uSkyHigh, clamp(d.y * 0.5 + 0.5, 0.0, 1.0)); }

vec3 shadowTransmittance(vec3 o, vec3 d, float maxT) {
    int blocker;
    if (traceRay(o, d, maxT, true, blocker).x >= 0.0) return vec3(0.0);  // opaque: early out
    if (uAnyPass == 0) return vec3(1.0);
    vec3 tr = vec3(1.0);
    for (int k = 0; k < 8; ++k) {
        int tri;
        vec4 h = traceRay(o, d, maxT, false, tri);
        if (h.x < 0.0) return tr;
        if (fetch(uTris, 8 * tri + 2).w < 0.5) return vec3(0.0);  // opaque blocker
        vec3 ng;
        bool front;
        Surface s = surfaceAt(tri, h.yz, d, ng, front);
        if (s.emission.r + s.emission.g + s.emission.b <= 0.0) {
            tr *= vec3(1.0 - s.opacity) + s.base * (s.opacity * s.transmission * (1.0 - s.metallic));
            if (lum(tr) < 1e-4) return vec3(0.0);
        }
        o += d * (h.x + 1e-4);
        maxT -= h.x + 1e-4;
        if (maxT <= 0.0) return tr;
    }
    return tr;
}

vec3 tracePath(vec3 o, vec3 d) {
    vec3 radiance = vec3(0.0), throughput = vec3(1.0);
    int bounce = 0;
    for (int step = 0; step < 64; ++step) {
        int tri;
        vec4 h = traceRay(o, d, 1e30, false, tri);
        if (bounce == 0) {  // area lights seen directly
            bool done = false;
            for (int i = 0; i < uLightCount; ++i) {
                vec4 l0 = fetch(uLights, 5 * i);
                if (l0.w < 2.5) continue;
                vec4 l1 = fetch(uLights, 5 * i + 1), l2 = fetch(uLights, 5 * i + 2);
                vec4 l3 = fetch(uLights, 5 * i + 3), l4 = fetch(uLights, 5 * i + 4);
                float denom = dot(d, l1.xyz);
                if (denom >= 0.0) continue;
                float t = dot(l0.xyz - o, l1.xyz) / denom;
                if (t <= 1e-4 || (h.x >= 0.0 && t >= h.x)) continue;
                vec3 q = o + d * t - l0.xyz;
                float lu = dot(q, l3.xyz) / max(1e-12, dot(l3.xyz, l3.xyz));
                float lv = dot(q, l4.xyz) / max(1e-12, dot(l4.xyz, l4.xyz));
                if (abs(lu) <= 1.0 && abs(lv) <= 1.0) {
                    float area = 4.0 * length(l3.xyz) * length(l4.xyz);
                    radiance += throughput * l2.rgb / max(1e-4, area);
                    done = true;
                    break;
                }
            }
            if (done) return radiance;
        }
        float cl = (bounce == 0 || uClamp <= 0.0) ? 1e30 : uClamp;
        if (h.x < 0.0) {
            vec3 c = throughput * sky(d);
            float mx = max(c.r, max(c.g, c.b));
            radiance += mx > cl ? c * (cl / mx) : c;
            break;
        }
        vec3 ng;
        bool front;
        Surface s = surfaceAt(tri, h.yz, d, ng, front);
        vec3 p = o + d * h.x;
        vec3 n = s.n, v = -d;
        vec3 ap = abs(p);
        float scale = 1e-4 * max(1.0, max(ap.x, max(ap.y, ap.z)));
        if (s.opacity < 1.0 && rnd() >= s.opacity) {
            o = p + d * scale;
            continue;
        }
        {
            vec3 c = throughput * s.emission;
            float mx = max(c.r, max(c.g, c.b));
            radiance += mx > cl ? c * (cl / mx) : c;
        }
        vec3 origin = p + ng * scale;
        vec3 direct = vec3(0.0);
        for (int i = 0; i < uLightCount; ++i) {
            vec4 l0 = fetch(uLights, 5 * i), l1 = fetch(uLights, 5 * i + 1), l2 = fetch(uLights, 5 * i + 2);
            vec4 l3 = fetch(uLights, 5 * i + 3), l4 = fetch(uLights, 5 * i + 4);
            float type = l0.w;
            vec3 L;
            float att = 1.0, maxT = 1e30;
            if (type > 0.5 && type < 1.5) {
                L = l2.w > 0.0 ? coneSample(-l1.xyz, l2.w) : -l1.xyz;
            } else {
                vec3 lp = type > 2.5 ? l0.xyz + l3.xyz * (2.0 * rnd() - 1.0) + l4.xyz * (2.0 * rnd() - 1.0)
                                     : l0.xyz + uniformSphere() * l2.w;
                vec3 dv = lp - p;
                float dist = length(dv);
                if (dist < 1e-5) continue;
                L = dv / dist;
                float x = clamp(1.0 - pow(dist / l1.w, 4.0), 0.0, 1.0);
                att = x * x / (dist * dist + 1.0);
                if (type > 1.5 && type < 2.5) {
                    float t = clamp((dot(-L, l1.xyz) - l4.w) / (l3.w - l4.w), 0.0, 1.0);
                    att *= t * t * (3.0 - 2.0 * t);
                } else if (type > 2.5) {
                    att *= max(0.0, dot(-L, l1.xyz));
                }
                maxT = dist - 1e-3;
            }
            float nl = dot(n, L);
            if (nl <= 0.0 || att <= 0.0 || dot(ng, L) <= 0.0) continue;
            vec3 tr = shadowTransmittance(origin, L, maxT);
            if (tr.r + tr.g + tr.b <= 0.0) continue;
            direct += evalBrdf(s, n, v, L) * nl * att * l2.rgb * tr;
        }
        {
            vec3 c = throughput * direct;
            float mx = max(c.r, max(c.g, c.b));
            radiance += mx > cl ? c * (cl / mx) : c;
        }
        if (bounce >= uMaxBounces) break;
        ++bounce;
        float nv = max(dot(n, v), 1e-4);
        float alpha = max(0.002, s.roughness * s.roughness);
        vec3 F0 = f0Of(s), Fv = schlick(F0, nv);
        float diel = 1.0 - s.metallic;
        float wTrans = diel * s.transmission;
        float wSpec = lum(Fv) * (1.0 - wTrans);
        float wDiff = diel * (1.0 - s.transmission) * lum(s.base) * (1.0 - lum(Fv));
        float total = wTrans + wSpec + wDiff;
        if (total <= 1e-6) break;
        float r = rnd() * total;
        vec3 weight;
        if (r < wSpec) {
            vec3 m = ggxSample(n, alpha);
            vec3 l = normalize(m * (2.0 * dot(v, m)) - v);
            float nl = dot(n, l), vm = max(dot(v, m), 1e-4), nm = max(dot(n, m), 1e-4);
            if (nl <= 0.0 || dot(ng, l) <= 0.0) break;
            weight = schlick(F0, vm) * (smithG(nl, nv, alpha) * vm / (nv * nm)) * (total / wSpec);
            d = l;
            o = origin;
        } else if (r < wSpec + wDiff) {
            vec3 l = cosineSample(n);
            if (dot(ng, l) <= 0.0) break;
            weight = (1.0 - Fv) * s.base * (diel * (1.0 - s.transmission) * total / wDiff);
            d = l;
            o = origin;
        } else {
            vec3 m = ggxSample(n, alpha);
            float cosI = dot(v, m);
            if (cosI <= 0.0) { m = n; cosI = nv; }
            float eta = front ? 1.0 / s.ior : s.ior;
            float Fr = fresnelDielectric(cosI, eta);
            if (rnd() < Fr) {
                d = normalize(m * (2.0 * cosI) - v);
                o = origin;
                weight = vec3(total / wTrans);
            } else {
                float sin2T = eta * eta * (1.0 - cosI * cosI);
                float cosT = sqrt(max(0.0, 1.0 - sin2T));
                d = normalize(-v * eta + m * (eta * cosI - cosT));
                o = p - ng * scale;
                weight = s.base * (total / wTrans);
            }
        }
        throughput *= weight;
        if (bounce >= 3) {
            float q = min(0.95, max(throughput.r, max(throughput.g, throughput.b)));
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
    float lu = rnd(), lv = rnd();
    if (uOrtho != 0) {
        o = uEye + uRight * (ppx * uOrthoHalf) + uUp * (ppy * uOrthoHalf) - uForward * 1000.0;
        d = uForward;
    } else {
        o = uEye;
        d = normalize(uForward + uRight * (ppx * uTanHalf) + uUp * (ppy * uTanHalf));
        if (uLensRadius > 0.0) {  // thin lens
            vec3 focus = o + d * (uFocusDist / max(1e-4, dot(d, uForward)));
            vec2 a = sampleAperture(lu, lv) * uLensRadius;
            o += uRight * a.x + uUp * a.y;
            d = normalize(focus - o);
        }
    }
    vec3 c = tracePath(o, d);
    if (any(isnan(c)) || any(isinf(c))) c = vec3(0.0);
    fragColor = vec4(c, 1.0);  // additive blend: rgb sums, alpha counts samples
}
)";

// Shows a render in the viewport: either the GPU accumulation buffer
// (sum / count, exposure, then the display transform) or a CPU result
// uploaded as RGBA8.
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
uniform float uExposure;
out vec4 fragColor;
void main() {
    if (uAccum != 0) {
        vec4 a = texture(uImage, vUV);
        vec3 c = a.a > 0.0 ? a.rgb / a.a : vec3(0.0);
        c = max(c * uExposure, vec3(0.0));
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

void GpuTracer::present(bool accumulated, int x, int y, int w, int h, float exposure) {
    unsigned tex = accumulated ? accumTex_ : imageTex_;
    if (!tex || !presentProg_) return;
    gl::Viewport(x, y, w, h);
    gl::Disable(GL_DEPTH_TEST);
    gl::Disable(GL_BLEND);
    gl::UseProgram(presentProg_);
    gl::BindVertexArray(vao_);
    gl::ActiveTexture(GL_TEXTURE0);
    gl::BindTexture(GL_TEXTURE_2D, tex);
    if (accumulated) {
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    gl::Uniform1i(presentU_.image, 0);
    gl::Uniform1i(presentU_.accum, accumulated ? 1 : 0);
    gl::Uniform1f(presentU_.exposure, exposure);
    gl::DrawArrays(GL_TRIANGLES, 0, 3);
    gl::BindVertexArray(0);
    gl::Enable(GL_BLEND);
}

bool GpuTracer::init(std::string& error) {
    presentProg_ = linkGlProgram(kPresentVS, kPresentFS, error);
    if (!presentProg_) return false;
    presentU_.image = gl::GetUniformLocation(presentProg_, "uImage");
    presentU_.accum = gl::GetUniformLocation(presentProg_, "uAccum");
    presentU_.exposure = gl::GetUniformLocation(presentProg_, "uExposure");
    gl::GenVertexArrays(1, &vao_);
    prog_ = linkGlProgram(kTraceVS, kTraceFS, error);
    if (!prog_) return false;
    auto loc = [&](const char* n) { return gl::GetUniformLocation(prog_, n); };
    u_.tris = loc("uTris");
    u_.nodes = loc("uNodes");
    u_.mats = loc("uMats");
    u_.lights = loc("uLights");
    u_.textures = loc("uTextures");
    u_.lightCount = loc("uLightCount");
    u_.anyPass = loc("uAnyPass");
    u_.width = loc("uWidth");
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
    u_.lensRadius = loc("uLensRadius");
    u_.focusDist = loc("uFocusDist");
    u_.blades = loc("uBlades");
    u_.maxBounces = loc("uMaxBounces");
    u_.clamp = loc("uClamp");
    u_.skyLow = loc("uSkyLow");
    u_.skyHigh = loc("uSkyHigh");
    gl::GenFramebuffers(1, &fbo_);
    timer_.init();
    return true;
}

void GpuTracer::shutdown() {
    unsigned texs[7] = {accumTex_, triTex_, nodeTex_, matTex_, imageTex_, lightTex_, texArray_};
    for (unsigned t : texs)
        if (t) gl::DeleteTextures(1, &t);
    accumTex_ = triTex_ = nodeTex_ = matTex_ = imageTex_ = lightTex_ = texArray_ = 0;
    imageW_ = imageH_ = 0;
    if (fbo_) gl::DeleteFramebuffers(1, &fbo_);
    if (vao_) gl::DeleteVertexArrays(1, &vao_);
    if (prog_) gl::DeleteProgram(prog_);
    if (presentProg_) gl::DeleteProgram(presentProg_);
    fbo_ = vao_ = prog_ = presentProg_ = 0;
    timer_.shutdown();
}

unsigned GpuTracer::uploadData(unsigned tex, const std::vector<float>& texels) {
    GLint maxSize = 4096;
    gl::GetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
    const int count = std::max(1, (int)(texels.size() / 4));
    const int w = std::min(4096, (int)maxSize);  // fixed row width (uWidth) for every data texture
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
    return tex;
}

void GpuTracer::start(const rt::SceneData& scene, const rt::View& view, const rt::Settings& settings, int w, int h,
                      int targetSamples) {
    PROF_SCOPE("gpu trace upload");
    uploadedBytes_ = 0;
    rt::PackedScene packed;
    rt::packScene(scene, 1024, packed);
    triTex_ = uploadData(triTex_, packed.tris);
    nodeTex_ = uploadData(nodeTex_, packed.nodes);
    matTex_ = uploadData(matTex_, packed.mats);
    lightTex_ = uploadData(lightTex_, packed.lights);
    lightCount_ = packed.lightCount;
    anyPass_ = packed.anyShadowPass;
    // Textures: one RGBA8 2D array (all layers the same size).
    if (!texArray_) gl::GenTextures(1, &texArray_);
    gl::BindTexture(GL_TEXTURE_2D_ARRAY, texArray_);
    gl::PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl::TexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, packed.texSize, packed.texSize, std::max(1, packed.texLayers), 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, packed.texels.data());
    gl::TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl::TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl::TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    gl::TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    gl::BindTexture(GL_TEXTURE_2D_ARRAY, 0);
    uploadedBytes_ += packed.texels.size();

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
    const unsigned data[4] = {triTex_, nodeTex_, matTex_, lightTex_};
    const int units[4] = {u_.tris, u_.nodes, u_.mats, u_.lights};
    for (int i = 0; i < 4; ++i) {
        gl::ActiveTexture(GL_TEXTURE0 + i);
        gl::BindTexture(GL_TEXTURE_2D, data[i]);
        gl::Uniform1i(units[i], i);
    }
    gl::ActiveTexture(GL_TEXTURE0 + 4);
    gl::BindTexture(GL_TEXTURE_2D_ARRAY, texArray_);
    gl::Uniform1i(u_.textures, 4);
    gl::ActiveTexture(GL_TEXTURE0);
    gl::Uniform1i(u_.lightCount, lightCount_);
    gl::Uniform1i(u_.anyPass, anyPass_ ? 1 : 0);
    {
        GLint maxSize = 4096;
        gl::GetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
        gl::Uniform1i(u_.width, std::min(4096, (int)maxSize));
    }
    gl::Uniform2f(u_.size, (float)w_, (float)h_);
    gl::Uniform3f(u_.eye, view_.eye.x, view_.eye.y, view_.eye.z);
    gl::Uniform3f(u_.forward, view_.forward.x, view_.forward.y, view_.forward.z);
    gl::Uniform3f(u_.right, view_.right.x, view_.right.y, view_.right.z);
    gl::Uniform3f(u_.up, view_.up.x, view_.up.y, view_.up.z);
    gl::Uniform1f(u_.tanHalf, view_.tanHalfFov);
    gl::Uniform1f(u_.orthoHalf, view_.orthoHalfHeight);
    gl::Uniform1i(u_.ortho, view_.ortho ? 1 : 0);
    gl::Uniform1f(u_.aspect, view_.aspect);
    gl::Uniform1f(u_.lensRadius, view_.lensRadius);
    gl::Uniform1f(u_.focusDist, view_.focusDistance);
    gl::Uniform1i(u_.blades, view_.blades);
    gl::Uniform1i(u_.maxBounces, settings_.maxBounces);
    gl::Uniform1f(u_.clamp, settings_.clampIndirect);
    gl::Uniform3f(u_.skyLow, skyLow_.x, skyLow_.y, skyLow_.z);
    gl::Uniform3f(u_.skyHigh, skyHigh_.x, skyHigh_.y, skyHigh_.z);

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
    gl::ActiveTexture(GL_TEXTURE0 + 4);
    gl::BindTexture(GL_TEXTURE_2D_ARRAY, 0);
    gl::ActiveTexture(GL_TEXTURE0);
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
            c = rt::toDisplay(c, view_.exposure);
            uint8_t* o = &rgba[((size_t)y * w_ + x) * 4];
            o[0] = (uint8_t)std::lround(c.x * 255.0f);
            o[1] = (uint8_t)std::lround(c.y * 255.0f);
            o[2] = (uint8_t)std::lround(c.z * 255.0f);
            o[3] = 255;
        }
}

Vec3 GpuTracer::readPixel(int x, int y) {
    float p[4] = {};
    gl::BindFramebuffer(GL_FRAMEBUFFER, fbo_);
    gl::ReadPixels(x, h_ - 1 - y, 1, 1, GL_RGBA, GL_FLOAT, p);
    gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
    return p[3] > 0 ? Vec3(p[0], p[1], p[2]) / p[3] : Vec3();
}
