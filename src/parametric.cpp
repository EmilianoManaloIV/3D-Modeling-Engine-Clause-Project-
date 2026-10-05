#include "parametric.h"

#include "uv.h"

#include <algorithm>
#include <cmath>

namespace {

const ShapeDef kShapes[PS_Count] = {
    {"Cube", 4,
     {{"Width", 2, 0.01f, 1000, 0.02f, false}, {"Height", 2, 0.01f, 1000, 0.02f, false},
      {"Depth", 2, 0.01f, 1000, 0.02f, false}, {"Segments", 1, 1, 32, 0.05f, true}}},
    {"Sphere", 3,
     {{"Radius", 1, 0.01f, 1000, 0.01f, false}, {"Segments", 24, 3, 128, 0.2f, true},
      {"Rings", 16, 2, 64, 0.2f, true}}},
    {"Cylinder", 3,
     {{"Radius", 1, 0.01f, 1000, 0.01f, false}, {"Height", 2, 0.01f, 1000, 0.02f, false},
      {"Segments", 24, 3, 128, 0.2f, true}}},
    {"Cone", 4,
     {{"Base radius", 1, 0.0f, 1000, 0.01f, false}, {"Top radius", 0, 0.0f, 1000, 0.01f, false},
      {"Height", 2, 0.01f, 1000, 0.02f, false}, {"Segments", 24, 3, 128, 0.2f, true}}},
    {"Plane", 4,
     {{"Width", 2, 0.01f, 1000, 0.02f, false}, {"Depth", 2, 0.01f, 1000, 0.02f, false},
      {"X segments", 1, 1, 128, 0.1f, true}, {"Z segments", 1, 1, 128, 0.1f, true}}},
    {"Torus", 4,
     {{"Major radius", 1, 0.01f, 1000, 0.01f, false}, {"Minor radius", 0.35f, 0.005f, 1000, 0.005f, false},
      {"Ring segs", 32, 3, 256, 0.2f, true}, {"Tube segs", 16, 3, 128, 0.2f, true}}},
    {"Stairs", 4,
     {{"Steps", 8, 1, 200, 0.1f, true}, {"Width", 2, 0.01f, 1000, 0.02f, false},
      {"Step height", 0.2f, 0.005f, 100, 0.005f, false}, {"Step depth", 0.3f, 0.005f, 100, 0.005f, false}}},
    {"Gear", 5,
     {{"Teeth", 12, 3, 200, 0.1f, true}, {"Radius", 1, 0.05f, 1000, 0.01f, false},
      {"Tooth len", 0.2f, 0.0f, 100, 0.005f, false}, {"Thickness", 0.4f, 0.01f, 100, 0.01f, false},
      {"Hole radius", 0.25f, 0.0f, 1000, 0.01f, false}}},
    {"Pipe", 4,
     {{"Outer radius", 1, 0.02f, 1000, 0.01f, false}, {"Inner radius", 0.7f, 0.01f, 1000, 0.01f, false},
      {"Height", 2, 0.01f, 1000, 0.02f, false}, {"Segments", 32, 3, 256, 0.2f, true}}},
    {"Spring", 6,
     {{"Coils", 4, 0.25f, 100, 0.02f, false}, {"Radius", 1, 0.02f, 1000, 0.01f, false},
      {"Wire radius", 0.15f, 0.005f, 100, 0.005f, false}, {"Height", 2, 0.0f, 1000, 0.02f, false},
      {"Segs/coil", 24, 4, 128, 0.2f, true}, {"Wire segs", 10, 3, 64, 0.1f, true}}},
};

void addQuad(Mesh& m, Vec3 o, Vec3 u, Vec3 v) {  // u x v points outward
    int b = (int)m.verts.size();
    m.verts.insert(m.verts.end(), {o, o + u, o + u + v, o + v});
    m.faces.push_back({b, b + 1, b + 2, b + 3});
}

Mesh stairs(int steps, float w, float sh, float sd) {
    Mesh m;
    const float hw = w * 0.5f, H = steps * sh, D = steps * sd;
    const float y0 = -H * 0.5f;
    for (int i = 0; i < steps; ++i) {
        const float zf = D * 0.5f - i * sd;         // front edge of this column
        const float yb = y0 + i * sh, yt = y0 + (i + 1) * sh;
        addQuad(m, {-hw, yt, zf}, {w, 0, 0}, {0, 0, -sd});                       // tread (top)
        addQuad(m, {-hw, i == 0 ? y0 : yb, zf}, {w, 0, 0}, {0, yt - (i == 0 ? y0 : yb), 0});  // riser
        addQuad(m, {-hw, y0, zf - sd}, {w, 0, 0}, {0, 0, sd});                   // bottom
        addQuad(m, {hw, y0, zf}, {0, 0, -sd}, {0, yt - y0, 0});                  // right side
        addQuad(m, {-hw, y0, zf - sd}, {0, 0, sd}, {0, yt - y0, 0});             // left side
        if (i == steps - 1) addQuad(m, {hw, y0, zf - sd}, {-w, 0, 0}, {0, yt - y0, 0});  // back
    }
    weldVertices(m, 1e-5f);
    return m;
}

// Extrudes a closed, star-shaped XZ profile (CCW seen from above) along Y.
// With hole > 0 the caps become rings around a round hole.
Mesh profileSolid(const std::vector<Vec2>& profile, float hole, float height) {
    const int n = (int)profile.size();
    const float h = height * 0.5f;
    Mesh m;
    for (const Vec2& p : profile) m.verts.push_back({p.x, -h, p.y});  // 0..n-1  bottom outer
    for (const Vec2& p : profile) m.verts.push_back({p.x, h, p.y});   // n..2n-1 top outer
    for (int i = 0; i < n; ++i) {
        int j = (i + 1) % n;
        m.faces.push_back({n + i, i, j, n + j});
    }
    if (hole > 0) {
        const int ib = 2 * n, it = 3 * n;
        for (int k = 0; k < 2; ++k)
            for (const Vec2& p : profile) {
                float a = std::atan2(p.x, p.y);
                m.verts.push_back({hole * std::sin(a), k == 0 ? -h : h, hole * std::cos(a)});
            }
        for (int i = 0; i < n; ++i) {
            int j = (i + 1) % n;
            m.faces.push_back({n + i, n + j, it + j, it + i});  // top ring
            m.faces.push_back({i, ib + i, ib + j, j});          // bottom ring
            m.faces.push_back({it + j, ib + j, ib + i, it + i});  // inner wall (faces the axis)
        }
    } else {
        const int cb = 2 * n, ct = 2 * n + 1;
        m.verts.push_back({0, -h, 0});
        m.verts.push_back({0, h, 0});
        for (int i = 0; i < n; ++i) {
            int j = (i + 1) % n;
            m.faces.push_back({ct, n + i, n + j});
            m.faces.push_back({cb, j, i});
        }
    }
    return m;
}

Mesh gear(int teeth, float R, float depth, float thickness, float hole) {
    std::vector<Vec2> profile;
    const float root = std::max(0.01f, R - depth);
    const float step = 2.0f * kPi / teeth;
    const float frac[4] = {0.0f, 0.2f, 0.45f, 0.65f};
    const float rad[4] = {root, R, R, root};
    for (int t = 0; t < teeth; ++t)
        for (int k = 0; k < 4; ++k) {
            float a = (t + frac[k]) * step;
            profile.push_back({rad[k] * std::sin(a), rad[k] * std::cos(a)});
        }
    return profileSolid(profile, std::min(hole, root * 0.95f), thickness);
}

Mesh pipe(float outer, float inner, float height, int segments) {
    std::vector<Vec2> profile;
    for (int s = 0; s < segments; ++s) {
        float a = 2.0f * kPi * s / segments;
        profile.push_back({outer * std::sin(a), outer * std::cos(a)});
    }
    return profileSolid(profile, std::min(inner, outer * 0.98f), height);
}

Mesh spring(float coils, float R, float r, float height, int segPerCoil, int wireSegs) {
    const int M = std::max(2, (int)std::lround(coils * segPerCoil));
    const int K = wireSegs;
    const float dTheta = 2.0f * kPi * coils;
    Mesh m;
    for (int i = 0; i <= M; ++i) {
        float t = float(i) / M, th = dTheta * t;
        Vec3 c{R * std::sin(th), -height * 0.5f + height * t, R * std::cos(th)};
        Vec3 T = normalize(Vec3(R * std::cos(th) * dTheta, height, -R * std::sin(th) * dTheta));
        Vec3 N0{std::sin(th), 0, std::cos(th)};
        Vec3 N = normalize(N0 - T * dot(N0, T));
        Vec3 B = cross(N, T);  // same handedness as the torus tube
        for (int j = 0; j < K; ++j) {
            float phi = 2.0f * kPi * j / K;
            m.verts.push_back(c + (N * std::cos(phi) + B * std::sin(phi)) * r);
        }
    }
    auto idx = [K](int i, int j) { return i * K + (j % K); };
    for (int i = 0; i < M; ++i)
        for (int j = 0; j < K; ++j) {
            m.faces.push_back({idx(i, j), idx(i + 1, j), idx(i + 1, j + 1), idx(i, j + 1)});
            float u0 = float(i) / M, u1 = float(i + 1) / M, v0 = float(j) / K, v1 = float(j + 1) / K;
            m.uvs.push_back({{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}});
        }
    std::vector<int> start, end;
    std::vector<Vec2> startUV, endUV;
    for (int j = 0; j < K; ++j) {
        float phi = 2.0f * kPi * j / K;
        start.push_back(idx(0, j));
        startUV.push_back({0.5f + 0.5f * std::cos(phi), 0.5f + 0.5f * std::sin(phi)});
    }
    for (int j = K - 1; j >= 0; --j) {
        float phi = 2.0f * kPi * j / K;
        end.push_back(idx(M, j));
        endUV.push_back({0.5f + 0.5f * std::cos(phi), 0.5f - 0.5f * std::sin(phi)});
    }
    m.faces.push_back(start);
    m.uvs.push_back(startUV);
    m.faces.push_back(end);
    m.uvs.push_back(endUV);
    return m;
}

}  // namespace

const ShapeDef& shapeDef(int shape) { return kShapes[std::max(0, std::min(shape, PS_Count - 1))]; }

ParametricSpec defaultSpec(int shape) {
    ParametricSpec s;
    s.shape = shape;
    const ShapeDef& d = shapeDef(shape);
    for (int i = 0; i < d.count; ++i) s.p[i] = d.params[i].def;
    return s;
}

void clampSpec(ParametricSpec& s) {
    if (!s.active()) return;
    const ShapeDef& d = shapeDef(s.shape);
    for (int i = 0; i < d.count; ++i) {
        s.p[i] = clampf(s.p[i], d.params[i].lo, d.params[i].hi);
        if (d.params[i].integer) s.p[i] = std::round(s.p[i]);
    }
    s.subdivisions = std::max(0, std::min(3, s.subdivisions));
    s.taper = clampf(s.taper, 0.0f, 10.0f);
}

Mesh generateParametric(const ParametricSpec& specIn) {
    ParametricSpec s = specIn;
    clampSpec(s);
    const float* p = s.p;
    Mesh m;
    bool needsUV = false;
    switch (s.shape) {
        case PS_Cube: m = primitives::box(p[0], p[1], p[2], (int)p[3]); break;
        case PS_Sphere: m = primitives::uvSphere(p[0], (int)p[1], (int)p[2]); break;
        case PS_Cylinder: m = primitives::cylinder(p[0], p[1], (int)p[2]); break;
        case PS_Cone:
            m = primitives::frustum(std::max(p[0], 1e-3f), p[1], p[2], (int)p[3]);
            break;
        case PS_Plane: m = primitives::grid(p[0], p[1], (int)p[2], (int)p[3]); break;
        case PS_Torus: m = primitives::torus(p[0], p[1], (int)p[2], (int)p[3]); break;
        case PS_Stairs: m = stairs((int)p[0], p[1], p[2], p[3]); needsUV = true; break;
        case PS_Gear: m = gear((int)p[0], p[1], p[2], p[3], p[4]); needsUV = true; break;
        case PS_Pipe: m = pipe(p[0], p[1], p[2], (int)p[3]); needsUV = true; break;
        case PS_Spring: m = spring(p[0], p[1], p[2], p[3], (int)p[4], (int)p[5]); break;
        default: m = primitives::cube(); break;
    }
    if (needsUV) uv::unwrap(m, uv::Method::Smart);

    // Non-destructive modifier stack, applied in order.
    for (int i = 0; i < s.subdivisions; ++i) {
        size_t corners = 0;
        for (const auto& f : m.faces) corners += f.size();
        if (corners * 4 > 600000) break;  // keep it interactive
        m = catmullClark(m);
    }
    if (std::fabs(s.twist) > 1e-4f || std::fabs(s.taper - 1.0f) > 1e-4f) {
        float lo = 1e30f, hi = -1e30f;
        for (const Vec3& v : m.verts) {
            lo = std::min(lo, v.y);
            hi = std::max(hi, v.y);
        }
        float span = std::max(1e-6f, hi - lo);
        for (Vec3& v : m.verts) {
            float t = (v.y - lo) / span;
            float a = toRadians(s.twist) * t, sc = 1.0f + (s.taper - 1.0f) * t;
            float c = std::cos(a), sn = std::sin(a);
            float x = v.x * c + v.z * sn, z = -v.x * sn + v.z * c;
            v.x = x * sc;
            v.z = z * sc;
        }
    }
    m.validate();
    m.touch();
    return m;
}
