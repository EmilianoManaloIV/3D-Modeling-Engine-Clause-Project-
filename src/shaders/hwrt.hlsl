// Hardware ray-traced path tracer (DirectX Raytracing 1.1, inline ray
// tracing in a compute shader). It is the algorithm of pathtracer.cpp /
// gpu_tracer.cpp; only the ray casts differ: they go through RayQuery, which
// runs on the GPU's ray-tracing cores (NVIDIA RTX, AMD RDNA2+, Intel Arc)
// against the acceleration structure the driver built.
//
// Compiled ahead of time (tools/compile_hwrt_shader.py -> hwrt_shader.inl)
// with the Windows SDK's dxc: dxc -T cs_6_5 -E trace / -E resolve.

cbuffer Params : register(b0) {
    float3 eye;      float tanHalf;
    float3 forward;  float orthoHalf;
    float3 rightV;   float aspect;
    float3 upV;      float lensRadius;
    float3 skyLow;   float focusDist;
    float3 skyHigh;  float clampIndirect;
    uint width; uint height; uint maxBounces; uint lightCount;
    uint ortho; uint blades; float exposure; uint anyPass;
    uint geom1Offset; uint pad1; uint pad2; uint pad3;
};
cbuffer Dispatch : register(b1) {  // root constants, per dispatch
    uint rowOffset; uint rows; uint seed; uint firstSample;
};
RaytracingAccelerationStructure scene : register(t0);
StructuredBuffer<float4> tris : register(t1);    // rt::PackedScene layouts
StructuredBuffer<float4> mats : register(t2);
StructuredBuffer<float4> lightData : register(t3);
Texture2DArray<float4> textures : register(t4);
StructuredBuffer<uint> triMap : register(t5);     // (geometry, primitive) -> packed triangle
SamplerState samp : register(s0);
RWTexture2D<float4> accum : register(u0);
RWStructuredBuffer<uint> outPixels : register(u1);

static const float PI = 3.14159265358979;
static uint rngState;
uint pcg() {
    rngState = rngState * 747796405u + 2891336453u;
    uint w = ((rngState >> ((rngState >> 28u) + 4u)) ^ rngState) * 277803737u;
    return (w >> 22u) ^ w;
}
float rnd() { return float(pcg() >> 8) * (1.0 / 16777216.0); }
float4 tex(int layer, float2 uv) { return textures.SampleLevel(samp, float3(uv.x, 1.0 - uv.y, layer), 0); }

void basis(float3 n, out float3 t, out float3 b) {
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float bb = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * bb, -s * n.x);
    b = float3(bb, s + n.y * n.y * a, -n.y);
}
float3 cosineSample(float3 n) {
    float u1 = rnd(), u2 = rnd();
    float r = sqrt(u1), phi = 2.0 * PI * u2;
    float3 t, b;
    basis(n, t, b);
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u1)));
}
float3 ggxSample(float3 n, float alpha) {
    float u1 = rnd(), u2 = rnd();
    float a2 = alpha * alpha;
    float cos2 = (1.0 - u1) / (1.0 + (a2 - 1.0) * u1);
    float cosT = sqrt(max(0.0, cos2)), sinT = sqrt(max(0.0, 1.0 - cos2));
    float phi = 2.0 * PI * u2;
    float3 t, b;
    basis(n, t, b);
    return normalize(t * (sinT * cos(phi)) + b * (sinT * sin(phi)) + n * cosT);
}
float3 coneSample(float3 axis, float radius) {
    float u1 = rnd(), u2 = rnd();
    float cosMax = cos(radius);
    float cosT = 1.0 - u1 * (1.0 - cosMax), sinT = sqrt(max(0.0, 1.0 - cosT * cosT));
    float phi = 2.0 * PI * u2;
    float3 t, b;
    basis(axis, t, b);
    return normalize(t * (sinT * cos(phi)) + b * (sinT * sin(phi)) + axis * cosT);
}
float3 uniformSphere() {
    float z = 1.0 - 2.0 * rnd(), r = sqrt(max(0.0, 1.0 - z * z)), phi = 2.0 * PI * rnd();
    return float3(r * cos(phi), r * sin(phi), z);
}
float2 sampleAperture(float u1, float u2) {
    if (blades < 3) {
        float r = sqrt(u1), phi = 2.0 * PI * u2;
        return float2(r * cos(phi), r * sin(phi));
    }
    float fb = float(blades);
    float k = min(fb - 1.0, floor(u1 * fb));
    float t = u1 * fb - k;
    float a0 = 2.0 * PI * k / fb, a1 = 2.0 * PI * (k + 1.0) / fb;
    return lerp(float2(cos(a0), sin(a0)), float2(cos(a1), sin(a1)), u2) * sqrt(t);
}

// Geometry 0 holds the opaque triangles (flagged opaque in the BLAS),
// geometry 1 the see-through ones (glass, alpha, lamp shades).
int packedTri(uint geometry, uint primitive) { return (int)triMap[(geometry == 0 ? 0 : geom1Offset) + primitive]; }

// Nearest hit through the hardware: (t, u, v, 1) or t < 0.
float4 traceRay(float3 o, float3 d, float tMax, out int tri) {
    RayDesc r;
    r.Origin = o;
    r.Direction = d;
    r.TMin = 1e-4;
    r.TMax = tMax;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, r);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) q.CommitNonOpaqueTriangleHit();
    }
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        tri = packedTri(q.CommittedGeometryIndex(), q.CommittedPrimitiveIndex());
        float2 bc = q.CommittedTriangleBarycentrics();
        return float4(q.CommittedRayT(), bc, 1.0);
    }
    tri = -1;
    return float4(-1, -1, -1, -1);
}

// Shadow test: is there an opaque triangle in the way? The first one ends
// the search; see-through candidates are never committed.
bool occludedOpaque(float3 o, float3 d, float tMax) {
    RayDesc r;
    r.Origin = o;
    r.Direction = d;
    r.TMin = 1e-4;
    r.TMax = tMax;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, r);
    while (q.Proceed()) {
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

struct Surface {
    float3 base, emission, n;
    float metallic, roughness, opacity, transmission, ior;
};
Surface surfaceAt(int tri, float2 bc, float3 d, out float3 ngFacing, out bool front) {
    float4 t0 = tris[8 * tri], t1 = tris[8 * tri + 1], t2 = tris[8 * tri + 2];
    float4 n0 = tris[8 * tri + 3], n1 = tris[8 * tri + 4], n2 = tris[8 * tri + 5];
    float4 tg = tris[8 * tri + 6], u2 = tris[8 * tri + 7];
    int m = (int)t0.w;
    float4 m0 = mats[6 * m], m1 = mats[6 * m + 1], m2 = mats[6 * m + 2];
    float4 m3 = mats[6 * m + 3], m4 = mats[6 * m + 4], m5 = mats[6 * m + 5];
    float3 ng = normalize(cross(t1.xyz - t0.xyz, t2.xyz - t0.xyz));
    front = dot(ng, d) < 0.0;
    float w0 = 1.0 - bc.x - bc.y;
    float3 ns = n0.xyz * w0 + n1.xyz * bc.x + n2.xyz * bc.y;
    ns = length(ns) > 1e-8 ? normalize(ns) : ng;
    float2 uv = float2(n0.w, n1.w) * w0 + float2(n2.w, tg.w) * bc.x + u2.xy * bc.y;
    uv *= m3.xy;
    Surface s;
    s.base = m0.rgb; s.metallic = m0.a;
    s.emission = m1.rgb; s.roughness = m1.a;
    s.opacity = m2.x; s.transmission = m2.y; s.ior = m2.z;
    if (m3.z >= 0.0) { float4 c = tex((int)m3.z, uv); s.base *= pow(c.rgb, 2.2); s.opacity *= c.a; }
    if (m4.x >= 0.0) s.roughness *= tex((int)m4.x, uv).g;
    if (m4.y >= 0.0) s.metallic *= tex((int)m4.y, uv).b;
    if (m4.w >= 0.0) s.emission *= pow(tex((int)m4.w, uv).rgb, 2.2);
    if (m5.x >= 0.0) s.opacity *= tex((int)m5.x, uv).r;
    if (m3.w >= 0.0) {
        float3 mm = tex((int)m3.w, uv).xyz * 2.0 - 1.0;
        mm.xy *= m2.w;
        float3 T = tg.xyz - ns * dot(ns, tg.xyz);
        if (length(T) > 1e-6) {
            T = normalize(T);
            float3 B = cross(ns, T) * t1.w;
            float3 nv = T * mm.x + B * mm.y + ns * max(1e-3, mm.z);
            if (length(nv) > 1e-6) ns = normalize(nv);
        }
    }
    ngFacing = front ? ng : -ng;
    if (dot(ns, ngFacing) < 0.0) ns = -ns;
    s.n = ns;
    return s;
}
float lum(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float3 schlick(float3 f0, float c) { return f0 + (1.0 - f0) * pow(saturate(1.0 - c), 5.0); }
float smithG(float nl, float nv, float a) {
    float k = a * 0.5;
    return (nl / (nl * (1.0 - k) + k)) * (nv / (nv * (1.0 - k) + k));
}
float3 f0Of(Surface s) {
    float f = (s.ior - 1.0) / (s.ior + 1.0);
    return lerp(f * f, s.base, s.metallic);
}
float3 evalBrdf(Surface s, float3 n, float3 v, float3 l) {
    float nl = dot(n, l), nv = max(dot(n, v), 1e-4);
    if (nl <= 0.0) return 0;
    float3 h = normalize(l + v);
    float nh = max(dot(n, h), 0.0), vh = max(dot(v, h), 0.0);
    float a = max(0.002, s.roughness * s.roughness), a2 = a * a;
    float dd = nh * nh * (a2 - 1.0) + 1.0;
    float3 F = schlick(f0Of(s), vh);
    float3 spec = F * (a2 / (PI * dd * dd) * smithG(nl, nv, a) / (4.0 * nl * nv));
    float3 kd = (1.0 - F) * s.base * ((1.0 - s.metallic) * (1.0 - s.transmission) / PI);
    return (kd + spec) * PI;
}
float fresnelDielectric(float cosI, float eta) {
    float sin2T = eta * eta * (1.0 - cosI * cosI);
    if (sin2T >= 1.0) return 1.0;
    float cosT = sqrt(1.0 - sin2T);
    float rs = (eta * cosI - cosT) / (eta * cosI + cosT), rp = (cosI - eta * cosT) / (cosI + eta * cosT);
    return 0.5 * (rs * rs + rp * rp);
}
float3 sky(float3 d) { return lerp(skyLow, skyHigh, saturate(d.y * 0.5 + 0.5)); }
float3 clampC(float3 c, float cl) {
    float mx = max(c.r, max(c.g, c.b));
    return mx > cl ? c * (cl / mx) : c;
}

float3 shadowTransmittance(float3 o, float3 d, float maxT) {
    if (occludedOpaque(o, d, maxT)) return 0;
    if (anyPass == 0) return 1;
    float3 tr = 1;
    for (int k = 0; k < 8; ++k) {
        int tri;
        float4 h = traceRay(o, d, maxT, tri);
        if (h.x < 0.0) return tr;
        if (tris[8 * tri + 2].w < 0.5) return 0;  // opaque blocker
        float3 ng;
        bool front;
        Surface s = surfaceAt(tri, h.yz, d, ng, front);
        if (s.emission.r + s.emission.g + s.emission.b <= 0.0) {
            tr *= (1.0 - s.opacity) + s.base * (s.opacity * s.transmission * (1.0 - s.metallic));
            if (lum(tr) < 1e-4) return 0;
        }
        o += d * (h.x + 1e-4);
        maxT -= h.x + 1e-4;
        if (maxT <= 0.0) return tr;
    }
    return tr;
}

float3 tracePath(float3 o, float3 d) {
    float3 radiance = 0, throughput = 1;
    int bounce = 0;
    for (int step = 0; step < 64; ++step) {
        int tri;
        float4 h = traceRay(o, d, 1e30, tri);
        if (bounce == 0) {  // area lights seen directly
            for (uint i = 0; i < lightCount; ++i) {
                float4 l0 = lightData[5 * i];
                if (l0.w < 2.5) continue;
                float4 l1 = lightData[5 * i + 1], l2 = lightData[5 * i + 2];
                float4 l3 = lightData[5 * i + 3], l4 = lightData[5 * i + 4];
                float denom = dot(d, l1.xyz);
                if (denom >= 0.0) continue;
                float t = dot(l0.xyz - o, l1.xyz) / denom;
                if (t <= 1e-4 || (h.x >= 0.0 && t >= h.x)) continue;
                float3 q = o + d * t - l0.xyz;
                float lu = dot(q, l3.xyz) / max(1e-12, dot(l3.xyz, l3.xyz));
                float lv = dot(q, l4.xyz) / max(1e-12, dot(l4.xyz, l4.xyz));
                if (abs(lu) <= 1.0 && abs(lv) <= 1.0) {
                    float area = 4.0 * length(l3.xyz) * length(l4.xyz);
                    return radiance + throughput * l2.rgb / max(1e-4, area);
                }
            }
        }
        float cl = (bounce == 0 || clampIndirect <= 0.0) ? 1e30 : clampIndirect;
        if (h.x < 0.0) {
            radiance += clampC(throughput * sky(d), cl);
            break;
        }
        float3 ng;
        bool front;
        Surface s = surfaceAt(tri, h.yz, d, ng, front);
        float3 p = o + d * h.x;
        float3 n = s.n, v = -d;
        float3 ap = abs(p);
        float scale = 1e-4 * max(1.0, max(ap.x, max(ap.y, ap.z)));
        if (s.opacity < 1.0 && rnd() >= s.opacity) {
            o = p + d * scale;
            continue;
        }
        radiance += clampC(throughput * s.emission, cl);
        float3 origin = p + ng * scale;
        float3 direct = 0;
        for (uint i = 0; i < lightCount; ++i) {
            float4 l0 = lightData[5 * i], l1 = lightData[5 * i + 1], l2 = lightData[5 * i + 2];
            float4 l3 = lightData[5 * i + 3], l4 = lightData[5 * i + 4];
            float type = l0.w;
            float3 L;
            float att = 1.0, maxT = 1e30;
            if (type > 0.5 && type < 1.5) {
                L = l2.w > 0.0 ? coneSample(-l1.xyz, l2.w) : -l1.xyz;
            } else {
                float3 lp = type > 2.5 ? l0.xyz + l3.xyz * (2.0 * rnd() - 1.0) + l4.xyz * (2.0 * rnd() - 1.0)
                                       : l0.xyz + uniformSphere() * l2.w;
                float3 dv = lp - p;
                float dist = length(dv);
                if (dist < 1e-5) continue;
                L = dv / dist;
                float x = saturate(1.0 - pow(dist / l1.w, 4.0));
                att = x * x / (dist * dist + 1.0);
                if (type > 1.5 && type < 2.5) {
                    float t = saturate((dot(-L, l1.xyz) - l4.w) / (l3.w - l4.w));
                    att *= t * t * (3.0 - 2.0 * t);
                } else if (type > 2.5) {
                    att *= max(0.0, dot(-L, l1.xyz));
                }
                maxT = dist - 1e-3;
            }
            float nl = dot(n, L);
            if (nl <= 0.0 || att <= 0.0 || dot(ng, L) <= 0.0) continue;
            float3 tr = shadowTransmittance(origin, L, maxT);
            if (tr.r + tr.g + tr.b <= 0.0) continue;
            direct += evalBrdf(s, n, v, L) * nl * att * l2.rgb * tr;
        }
        radiance += clampC(throughput * direct, cl);
        if (bounce >= (int)maxBounces) break;
        ++bounce;
        float nv = max(dot(n, v), 1e-4);
        float alpha = max(0.002, s.roughness * s.roughness);
        float3 F0 = f0Of(s), Fv = schlick(F0, nv);
        float diel = 1.0 - s.metallic;
        float wTrans = diel * s.transmission;
        float wSpec = lum(Fv) * (1.0 - wTrans);
        float wDiff = diel * (1.0 - s.transmission) * lum(s.base) * (1.0 - lum(Fv));
        float total = wTrans + wSpec + wDiff;
        if (total <= 1e-6) break;
        float r = rnd() * total;
        float3 weight;
        if (r < wSpec) {
            float3 m = ggxSample(n, alpha);
            float3 l = normalize(m * (2.0 * dot(v, m)) - v);
            float nl = dot(n, l), vm = max(dot(v, m), 1e-4), nm = max(dot(n, m), 1e-4);
            if (nl <= 0.0 || dot(ng, l) <= 0.0) break;
            weight = schlick(F0, vm) * (smithG(nl, nv, alpha) * vm / (nv * nm)) * (total / wSpec);
            d = l;
            o = origin;
        } else if (r < wSpec + wDiff) {
            float3 l = cosineSample(n);
            if (dot(ng, l) <= 0.0) break;
            weight = (1.0 - Fv) * s.base * (diel * (1.0 - s.transmission) * total / wDiff);
            d = l;
            o = origin;
        } else {
            float3 m = ggxSample(n, alpha);
            float cosI = dot(v, m);
            if (cosI <= 0.0) { m = n; cosI = nv; }
            float eta = front ? 1.0 / s.ior : s.ior;
            float Fr = fresnelDielectric(cosI, eta);
            if (rnd() < Fr) {
                d = normalize(m * (2.0 * cosI) - v);
                o = origin;
                weight = total / wTrans;
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

[numthreads(8, 8, 1)]
void trace(uint3 id : SV_DispatchThreadID) {
    uint x = id.x, row = rowOffset + id.y;
    if (x >= width || id.y >= rows || row >= height) return;
    rngState = x * 1973u + row * 9277u + seed * 26699u;
    rngState = rngState * 747796405u + 1u;
    pcg();
    float sx = (float(x) + rnd()) / float(width), sy = (float(row) + rnd()) / float(height);
    float ppx = (2.0 * sx - 1.0) * aspect, ppy = 1.0 - 2.0 * sy;
    float lu = rnd(), lv = rnd();
    float3 o, d;
    if (ortho != 0) {
        o = eye + rightV * (ppx * orthoHalf) + upV * (ppy * orthoHalf) - forward * 1000.0;
        d = forward;
    } else {
        o = eye;
        d = normalize(forward + rightV * (ppx * tanHalf) + upV * (ppy * tanHalf));
        if (lensRadius > 0.0) {
            float3 focus = o + d * (focusDist / max(1e-4, dot(d, forward)));
            float2 a = sampleAperture(lu, lv) * lensRadius;
            o += rightV * a.x + upV * a.y;
            d = normalize(focus - o);
        }
    }
    float3 c = tracePath(o, d);
    if (any(isnan(c)) || any(isinf(c))) c = 0;
    float4 sum = float4(c, 1.0);
    if (firstSample == 0) sum += accum[uint2(x, row)];
    accum[uint2(x, row)] = sum;
}

// Accumulated radiance -> exposure, soft shoulder, gamma -> packed RGBA8.
[numthreads(8, 8, 1)]
void resolve(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    float4 a = accum[id.xy];
    float3 c = a.a > 0.0 ? a.rgb / a.a : 0;
    c = max(c * exposure, 0);
    c = lerp(c, 0.8 + 0.2 * (1.0 - exp(-(c - 0.8) / 0.2)), step(0.8, c));
    c = pow(saturate(c), 1.0 / 2.2);
    uint3 q = uint3(round(c * 255.0));
    outPixels[id.y * width + id.x] = q.r | (q.g << 8) | (q.b << 16) | (255u << 24);
}
