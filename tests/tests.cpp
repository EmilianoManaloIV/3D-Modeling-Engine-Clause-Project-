// Unit tests for the OpenGL-free core (math, mesh operations, file I/O).
// Build with -DMODELER_BUILD_TESTS=ON and run modeler_tests.
#include "bvh.h"
#include "csg.h"
#include "expr.h"
#include "image_io.h"
#include "image_load.h"
#include "hwrt.h"
#include "input_map.h"
#include "jobs.h"
#include "mesh.h"
#include "meshedit.h"
#include "parametric.h"
#include "particles.h"
#include "pathtracer.h"
#include "polygon.h"
#include "scene.h"
#include "skin.h"
#include "transform.h"
#include "uv.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <thread>

static int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                    \
    } while (0)

static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
static bool near(Vec3 a, Vec3 b, float eps = 1e-4f) { return length(a - b) <= eps; }

// Every directed edge must appear exactly once and its reverse exactly once:
// the mesh is closed and consistently oriented.
static bool closedAndConsistent(const Mesh& m) {
    std::map<std::pair<int, int>, int> directed;
    for (const auto& f : m.faces)
        for (size_t i = 0; i < f.size(); ++i) directed[{f[i], f[(i + 1) % f.size()]}]++;
    for (const auto& kv : directed) {
        if (kv.second != 1) return false;
        auto rev = directed.find({kv.first.second, kv.first.first});
        if (rev == directed.end() || rev->second != 1) return false;
    }
    return true;
}

// Divergence theorem: 3 * volume = sum over faces of (centroid . area-normal).
// Positive for outward-facing closed meshes.
static float signedVolume(const Mesh& m) {
    float v = 0;
    for (const auto& f : m.faces) v += dot(faceCenter(m, f), faceNormalRaw(m, f) * 0.5f);
    return v / 3.0f;
}

static bool indicesValid(const Mesh& m) {
    for (const auto& f : m.faces) {
        if (f.size() < 3) return false;
        for (int i : f)
            if (i < 0 || i >= (int)m.verts.size()) return false;
    }
    return true;
}

static void testMath() {
    Mat4 m = translation({1, 2, 3}) * eulerToMatrix({30, -45, 60}) * scaling({2, 0.5f, 3});
    Mat4 p = m * inverse(m);
    for (int i = 0; i < 16; ++i) CHECK(near(p.m[i], (i % 5 == 0) ? 1.0f : 0.0f));

    Vec3 e{25, -40, 110};
    Vec3 back = matrixToEuler(eulerToMatrix(e));
    CHECK(near(back, e, 1e-2f));
    // Gimbal lock case still reproduces the same rotation.
    Vec3 g{10, 90, 0};
    Mat4 a = eulerToMatrix(g), b = eulerToMatrix(matrixToEuler(a));
    for (int i = 0; i < 16; ++i) CHECK(near(a.m[i], b.m[i], 1e-3f));

    Mat4 r = rotationAxis({0, 0, 1}, toRadians(90));
    CHECK(near(transformPoint(r, {1, 0, 0}), {0, 1, 0}));

    Mat4 v = lookAt({0, 0, 5}, {0, 0, 0}, {0, 1, 0});
    CHECK(near(transformPoint(v, {0, 0, 0}), {0, 0, -5}));
    Mat4 pr = perspective(toRadians(60), 1.0f, 0.1f, 100.0f);
    Vec4 nearClip = pr * Vec4(0, 0, -0.1f, 1), farClip = pr * Vec4(0, 0, -100.0f, 1);
    CHECK(near(nearClip.z / nearClip.w, -1.0f));
    CHECK(near(farClip.z / farClip.w, 1.0f));
}

static void testPrimitives() {
    struct Case {
        const char* name;
        Mesh mesh;
        bool closed;
        float volume;  // expected (approx), < 0 to skip
    };
    Case cases[] = {
        {"cube", primitives::cube(2), true, 8.0f},
        {"sphere", primitives::uvSphere(1, 48, 32), true, 4.18879f},
        {"cylinder", primitives::cylinder(1, 2, 64), true, 2 * kPi},
        {"cone", primitives::cone(1, 2, 64), true, 2 * kPi / 3},
        {"torus", primitives::torus(1, 0.35f, 64, 32), true, 2 * kPi * kPi * 0.35f * 0.35f},
        {"plane", primitives::plane(2, 3), false, -1},
    };
    for (auto& c : cases) {
        CHECK(indicesValid(c.mesh));
        if (c.closed) {
            bool ok = closedAndConsistent(c.mesh);
            if (!ok) std::printf("  %s is not closed/consistent\n", c.name);
            CHECK(ok);
            float vol = signedVolume(c.mesh);
            bool volOk = vol > 0 && std::fabs(vol - c.volume) / c.volume < 0.03f;
            if (!volOk) std::printf("  %s volume %f expected %f\n", c.name, vol, c.volume);
            CHECK(volOk);
        }
    }
    Mesh plane = primitives::plane(2, 3);
    CHECK(plane.verts.size() == 16 && plane.faces.size() == 9);
    CHECK(faceNormalRaw(plane, plane.faces[0]).y > 0);
}

static void testCatmullClark() {
    Mesh c = catmullClark(primitives::cube(2));
    CHECK(c.verts.size() == 26);
    CHECK(c.faces.size() == 24);
    CHECK(closedAndConsistent(c));
    CHECK(signedVolume(c) > 0 && signedVolume(c) < 8.0f);  // shrinks toward a sphere
    Mesh c2 = catmullClark(c);
    CHECK(c2.faces.size() == 96);
    CHECK(closedAndConsistent(c2));

    Mesh s = catmullClark(primitives::uvSphere(1, 12, 8));  // triangles at the poles
    CHECK(closedAndConsistent(s));
    for (const auto& f : s.faces) CHECK(f.size() == 4);

    Mesh p = catmullClark(primitives::plane(2, 1));  // boundary rules
    CHECK(p.verts.size() == 9 && p.faces.size() == 4);
    for (const Vec3& v : p.verts) CHECK(near(v.y, 0.0f));
}

static void testExtrudeAndDelete() {
    Mesh m = primitives::cube(2);
    std::vector<char> sel(m.verts.size(), 0);
    for (size_t i = 0; i < m.verts.size(); ++i) sel[i] = m.verts[i].y > 0;
    Vec3 n;
    CHECK(extrudeSelectedFaces(m, sel, &n));
    CHECK(near(n, {0, 1, 0}));
    CHECK(m.verts.size() == 12);
    CHECK(m.faces.size() == 10);
    CHECK(closedAndConsistent(m));
    int selected = 0;
    for (size_t i = 0; i < sel.size(); ++i)
        if (sel[i]) {
            ++selected;
            m.verts[i] += n;  // pull the new cap up by 1
        }
    CHECK(selected == 4);
    CHECK(near(signedVolume(m), 12.0f, 1e-3f));

    // Nothing selected -> nothing happens.
    std::vector<char> none(m.verts.size(), 0);
    CHECK(!extrudeSelectedFaces(m, none, &n));

    // Deleting one corner removes the 3 faces around it.
    Mesh c = primitives::cube(2);
    std::vector<char> one(c.verts.size(), 0);
    one[0] = 1;
    deleteVertices(c, one);
    CHECK(c.faces.size() == 3);
    CHECK(c.verts.size() == 7);
    CHECK(indicesValid(c));
    CHECK(one.size() == c.verts.size());
}

static void testRaycast() {
    Mesh c = primitives::cube(2);
    float t = 0;
    CHECK(raycastMesh(c, {0, 0, 5}, {0, 0, -1}, t) && near(t, 4.0f));
    CHECK(!raycastMesh(c, {3, 0, 5}, {0, 0, -1}, t));

    Scene s;
    int a = s.add(primitives::cube(2), "A", {1, 1, 1});
    int b = s.add(primitives::cube(2), "B", {1, 1, 1});
    s.objects[a].position = {0, 0, -5};
    s.objects[b].position = {0, 0, 0};
    s.objects[b].scale = {0.5f, 0.5f, 0.5f};
    float hitT = 0;
    CHECK(pickObject(s, {0, 0, 10}, {0, 0, -1}, &hitT) == b);
    CHECK(near(hitT, 9.5f));
    s.objects[b].position = {10, 0, 0};
    CHECK(pickObject(s, {0, 0, 10}, {0, 0, -1}) == a);
    CHECK(s.objects[b].name == "B" && s.uniqueName("A") == "A.001");
}

static void testFiles() {
    Scene s;
    int i = s.add(primitives::torus(), "My Torus", {0.2f, 0.4f, 0.6f});
    s.objects[i].position = {1, 2, 3};
    s.objects[i].rotation = {10, 20, 30};
    s.objects[i].scale = {1, 2, 1};
    s.objects[i].smooth = false;
    s.add(primitives::cube(), "Box", {1, 0, 0});

    std::string err;
    CHECK(saveScene(s, "test_roundtrip.m3d", err));
    Scene l;
    CHECK(loadScene(l, "test_roundtrip.m3d", err));
    CHECK(l.objects.size() == 2);
    if (l.objects.size() == 2) {
        const Object& o = l.objects[0];
        CHECK(o.name == "My Torus");
        CHECK(near(o.position, {1, 2, 3}) && near(o.rotation, {10, 20, 30}) && near(o.scale, {1, 2, 1}));
        CHECK(near(o.color, {0.2f, 0.4f, 0.6f}, 1e-3f));
        CHECK(!o.smooth);
        CHECK(o.mesh.verts.size() == s.objects[0].mesh.verts.size());
        CHECK(o.mesh.faces.size() == s.objects[0].mesh.faces.size());
        CHECK(l.objects[0].id != l.objects[1].id);
    }

    int count = 0;
    CHECK(exportOBJ(s, "test_roundtrip.obj", err, &count));
    CHECK(count == 2);
    Scene imp;
    int first = -1;
    CHECK(importOBJ(imp, "test_roundtrip.obj", err, &first));
    CHECK(first == 0 && imp.objects.size() == 2);
    if (imp.objects.size() == 2) {
        CHECK(imp.objects[1].name == "Box");
        CHECK(imp.objects[1].mesh.faces.size() == 6 && imp.objects[1].mesh.verts.size() == 8);
        CHECK(closedAndConsistent(imp.objects[1].mesh));
        // Export bakes the transform: imported torus should sit around (1,2,3).
        Vec3 c;
        for (const Vec3& v : imp.objects[0].mesh.verts) c += v;
        c = c / float(imp.objects[0].mesh.verts.size());
        CHECK(near(c, {1, 2, 3}, 1e-3f));
    }

    // Bad input is rejected with a message rather than crashing.
    CHECK(!loadScene(l, "does_not_exist.m3d", err) && !err.empty());
    CHECK(!importOBJ(imp, "does_not_exist.obj", err));
    std::remove("test_roundtrip.m3d");
    std::remove("test_roundtrip.obj");
    std::remove("test_roundtrip.mtl");
}

// --- v2 features ---------------------------------------------------------

static bool uvsValid(const Mesh& m, float lo, float hi) {
    if (!m.hasUVs()) return false;
    for (size_t f = 0; f < m.faces.size(); ++f) {
        if (m.uvs[f].size() != m.faces[f].size()) return false;
        for (const Vec2& t : m.uvs[f])
            if (!std::isfinite(t.x) || !std::isfinite(t.y) || t.x < lo || t.x > hi || t.y < lo || t.y > hi) return false;
    }
    return true;
}

static void testUV() {
    // Primitives come with UVs in 0..1.
    CHECK(uvsValid(primitives::cube(), 0, 1));
    CHECK(uvsValid(primitives::uvSphere(), 0, 1));
    CHECK(uvsValid(primitives::cylinder(), 0, 1));
    CHECK(uvsValid(primitives::torus(), 0, 1));

    const uv::Method methods[] = {uv::Method::Smart, uv::Method::Box, uv::Method::Planar, uv::Method::PerFace};
    for (uv::Method method : methods) {
        Mesh g = generateParametric(defaultSpec(PS_Gear));
        g.uvs.clear();
        uv::unwrap(g, method);
        bool ok = uvsValid(g, -1e-4f, 1 + 1e-4f);
        if (!ok) std::printf("  unwrap %s out of range\n", uv::methodName(method));
        CHECK(ok);
    }
    Mesh s = primitives::uvSphere();
    uv::unwrap(s, uv::Method::Spherical);
    CHECK(uvsValid(s, -1e-4f, 1.5f));  // seam-fixed faces may run past 1
    uv::unwrap(s, uv::Method::Cylindrical);
    CHECK(uvsValid(s, -1e-4f, 1.5f));

    // Smart unwrap of a cube = 6 charts, packed without overlap.
    Mesh c = primitives::cube();
    uv::unwrap(c, uv::Method::Smart);
    std::vector<int> all;
    for (int f = 0; f < 6; ++f) all.push_back(f);
    int islands = 0;
    uv::islandIds(c, all, &islands);
    CHECK(islands == 6);
    bool overlap = false;
    for (int a = 0; a < 6; ++a)
        for (int b = a + 1; b < 6; ++b) {
            Vec2 la, ha, lb, hb;
            uv::bounds(c, {a}, la, ha);
            uv::bounds(c, {b}, lb, hb);
            if (la.x < hb.x - 1e-4f && lb.x < ha.x - 1e-4f && la.y < hb.y - 1e-4f && lb.y < ha.y - 1e-4f)
                overlap = true;
        }
    CHECK(!overlap);

    // A connected surface with continuous UVs is one island.
    Mesh plane = primitives::plane(2, 4);
    std::vector<int> pf;
    for (int f = 0; f < 16; ++f) pf.push_back(f);
    uv::islandIds(plane, pf, &islands);
    CHECK(islands == 1);

    // Tools: fit puts the selection into 0..1; rotate twice + flip both ways = identity.
    Mesh t = primitives::plane(2, 2);
    uv::translate(t, {}, Vec2(3, -2));
    uv::fit(t);
    Vec2 lo, hi;
    uv::bounds(t, {}, lo, hi);
    CHECK(near(lo.x, 0) && near(lo.y, 0) && near(std::max(hi.x, hi.y), 1));
    Vec2 before = t.uvs[0][0];
    uv::rotate90(t);
    uv::rotate90(t);
    uv::flip(t, {}, true);
    uv::flip(t, {}, false);
    CHECK(near(t.uvs[0][0].x, before.x) && near(t.uvs[0][0].y, before.y));

    // Subdivision keeps UVs.
    Mesh cc = catmullClark(primitives::cube());
    CHECK(uvsValid(cc, 0, 1));
}

static void testParametric() {
    for (int shape = 0; shape < PS_Count; ++shape) {
        ParametricSpec spec = defaultSpec(shape);
        Mesh m = generateParametric(spec);
        bool ok = indicesValid(m) && !m.faces.empty() && m.hasUVs();
        if (!ok) std::printf("  shape %s invalid\n", shapeDef(shape).name);
        CHECK(ok);
        if (shape != PS_Plane && shape != PS_Stairs) {
            bool closed = closedAndConsistent(m);
            if (!closed) std::printf("  shape %s not closed\n", shapeDef(shape).name);
            CHECK(closed);
        }
        if (shape != PS_Plane) {
            bool positive = signedVolume(m) > 0;
            if (!positive) std::printf("  shape %s inside-out\n", shapeDef(shape).name);
            CHECK(positive);
        }
    }
    ParametricSpec gear = defaultSpec(PS_Gear);
    gear.p[4] = 0;  // no hole -> fan caps
    CHECK(closedAndConsistent(generateParametric(gear)));
    ParametricSpec frustum = defaultSpec(PS_Cone);
    frustum.p[1] = 0.5f;  // truncated cone
    CHECK(closedAndConsistent(generateParametric(frustum)));

    // Modifier stack.
    ParametricSpec box = defaultSpec(PS_Cube);
    Mesh plain = generateParametric(box);
    box.subdivisions = 2;
    Mesh sub = generateParametric(box);
    CHECK(sub.faces.size() == plain.faces.size() * 16);
    box.subdivisions = 0;
    box.taper = 0.5f;
    box.twist = 45;
    Mesh tw = generateParametric(box);
    CHECK(tw.verts.size() == plain.verts.size());
    float topRadius = 0;
    for (const Vec3& v : tw.verts)
        if (v.y > 0.9f) topRadius = std::max(topRadius, std::sqrt(v.x * v.x + v.z * v.z));
    CHECK(near(topRadius, std::sqrt(2.0f) * 0.5f, 1e-3f));  // corner radius sqrt(2), halved by the taper
    // Parameters are clamped (integer params rounded).
    ParametricSpec bad = defaultSpec(PS_Sphere);
    bad.p[1] = 2.4f;
    clampSpec(bad);
    CHECK(bad.p[1] == 3.0f);
}

static void testHierarchy() {
    Vec3 p, r, s;
    Mat4 m = translation({1, 2, 3}) * eulerToMatrix({10, 20, 30}) * scaling({2, 3, 4});
    decomposeTRS(m, p, r, s);
    CHECK(near(p, {1, 2, 3}) && near(r, {10, 20, 30}, 1e-2f) && near(s, {2, 3, 4}, 1e-4f));

    Scene sc;
    int a = sc.add(primitives::cube(), "A", {1, 1, 1});
    int b = sc.add(primitives::cube(), "B", {1, 1, 1});
    int c = sc.add(primitives::cube(), "C", {1, 1, 1});
    sc.objects[a].position = {5, 0, 0};
    sc.objects[a].rotation = {0, 90, 0};
    sc.objects[b].position = {0, 1, 0};
    Vec3 bWorldBefore = transformPoint(sc.world(b), Vec3());
    CHECK(sc.setParent(b, a, true));
    CHECK(near(transformPoint(sc.world(b), Vec3()), bWorldBefore));  // keeps world position
    CHECK(sc.setParent(c, b, false));
    sc.objects[c].position = {0, 0, 1};
    // keepWorld gave b a local rotation cancelling a's 90 degrees, so b (and
    // c's offset along b's +Z) is still world-aligned.
    Vec3 cw = transformPoint(sc.world(c), Vec3());
    CHECK(near(cw, bWorldBefore + Vec3(0, 0, 1), 1e-4f));
    CHECK(near(sc.objects[b].rotation, {0, -90, 0}, 1e-3f));
    CHECK(!sc.setParent(a, c, true));  // cycle refused
    CHECK(sc.isAncestor(a, c) && !sc.isAncestor(c, a));
    CHECK(sc.descendants(a).size() == 2 && sc.depth(c) == 2);
    // Moving the root moves the whole chain.
    sc.objects[a].position += Vec3(0, 10, 0);
    CHECK(near(transformPoint(sc.world(c), Vec3()), cw + Vec3(0, 10, 0), 1e-4f));
    CHECK(sc.setParent(b, -1, true));
    CHECK(near(transformPoint(sc.world(b), Vec3()), bWorldBefore + Vec3(0, 10, 0), 1e-4f));
}

static void testSkinning() {
    BoneWeights w;
    for (int i = 0; i < 6; ++i) w.add(i, 0.1f * (i + 1));
    CHECK(w.weightOf(0) == 0 && w.weightOf(1) == 0 && w.weightOf(5) > 0);  // keeps the strongest 4
    w.normalize();
    CHECK(near(w.total(), 1.0f));

    Scene s;
    int col = s.add(primitives::box(0.5f, 4, 0.5f, 8), "Column", {1, 1, 1});
    s.objects[col].position = {0, 2, 0};
    Object root;
    root.kind = ObjectKind::Bone;
    root.boneLength = 2;
    int r = s.addObject(root);
    Object tip;
    tip.kind = ObjectKind::Bone;
    tip.boneLength = 2;
    tip.position = {0, 2, 0};
    tip.parent = s.objects[r].id;
    int t = s.addObject(tip);
    std::string err;
    CHECK(bindSkin(s, col, {r, t}, err));
    CHECK(isSkinned(s.objects[col]));
    for (const BoneWeights& bw : s.objects[col].mesh.weights) CHECK(near(bw.total(), 1.0f, 1e-3f));

    // Rest pose: skinning reproduces the plain world positions.
    std::vector<Vec3> scratch;
    Mat4 model;
    const std::vector<Vec3>& rest = evaluateMesh(s, col, false, scratch, model);
    Mat4 world = s.world(col);
    bool same = true;
    for (size_t v = 0; v < rest.size(); ++v)
        same &= near(transformPoint(model, rest[v]), transformPoint(world, s.objects[col].mesh.verts[v]), 1e-4f);
    CHECK(same);
    uint64_t h0 = poseHash(s, col);

    // Bend the tip bone 90 degrees: top vertices swing over, bottom ones stay.
    s.objects[t].rotation = {0, 0, 90};
    CHECK(poseHash(s, col) != h0);
    const std::vector<Vec3>& posed = evaluateMesh(s, col, false, scratch, model);
    float topX = 0, bottomMove = 0;
    for (size_t v = 0; v < posed.size(); ++v) {
        Vec3 orig = transformPoint(world, s.objects[col].mesh.verts[v]);
        if (orig.y > 3.99f) topX = std::min(topX, posed[v].x);
        if (orig.y < 0.01f) bottomMove = std::max(bottomMove, length(posed[v] - orig));
    }
    CHECK(topX < -1.5f);  // the tip rotated toward -X
    CHECK(bottomMove < 1e-3f);

    // Weights survive subdivision; unbinding clears them.
    Mesh sub = catmullClark(s.objects[col].mesh);
    CHECK(sub.hasWeights());
    unbindSkin(s.objects[col]);
    CHECK(!isSkinned(s.objects[col]) && !s.objects[col].mesh.hasWeights());
}

static void testParticles() {
    ParticleSettings ps;
    ps.rate = 100;
    ps.lifetime = 10;
    ParticleSystemState st;
    Mat4 w = translation({0, 5, 0});
    for (int i = 0; i < 50; ++i) stepParticles(st, ps, w, 0.01f);
    CHECK(st.particles.size() >= 49 && st.particles.size() <= 51);
    bool upward = true, nearEmitter = true;
    for (const Particle& p : st.particles) {
        upward &= p.vel.y > 0;
        nearEmitter &= length(p.pos - Vec3(0, 5, 0)) < 3.0f;
    }
    CHECK(upward && nearEmitter);
    std::vector<ParticleVertex> verts;
    appendParticleVertices(st, ps, verts);
    CHECK(verts.size() == st.particles.size());
    // Once spawning stops and lifetimes run out, the system empties.
    ParticleSettings shortLived = ps;
    shortLived.rate = 0;
    for (Particle& p : st.particles) p.life = 0.05f;
    for (int i = 0; i < 20; ++i) stepParticles(st, shortLived, w, 0.01f);
    CHECK(st.particles.empty());
}

static void testFilesV2() {
    Scene s;
    int gear = s.add(generateParametric(defaultSpec(PS_Gear)), "Gear", {0.2f, 0.4f, 0.6f});
    s.objects[gear].param = defaultSpec(PS_Gear);
    s.objects[gear].emission = {1, 0.5f, 0};
    s.objects[gear].emissionStrength = 3;
    s.objects[gear].roughness = 0.1f;
    Object light;
    light.kind = ObjectKind::Light;
    light.name = "Lamp";
    light.light.type = LightType::Spot;
    light.light.intensity = 42;
    light.light.spotAngle = 33;
    light.parent = s.objects[gear].id;
    s.addObject(light);
    Object bone;
    bone.kind = ObjectKind::Bone;
    bone.boneLength = 1.5f;
    int bi = s.addObject(bone);
    Object emitter;
    emitter.kind = ObjectKind::Emitter;
    emitter.particles.rate = 77;
    emitter.particles.additive = false;
    s.addObject(emitter);
    std::string err;
    CHECK(bindSkin(s, gear, {bi}, err));
    s.ambient = {0.1f, 0.2f, 0.3f};

    CHECK(saveScene(s, "test_v2.m3d", err));
    Scene l;
    CHECK(loadScene(l, "test_v2.m3d", err));
    CHECK(l.objects.size() == 4);
    if (l.objects.size() == 4) {
        const Object& g = l.objects[0];
        CHECK(g.param.shape == PS_Gear && near(g.emissionStrength, 3) && near(g.roughness, 0.1f));
        CHECK(g.mesh.hasUVs() && g.mesh.uvs.size() == s.objects[0].mesh.uvs.size());
        CHECK(isSkinned(g) && g.skinBones.size() == 1 && g.skinBones[0] == l.objects[2].id);
        CHECK(l.objects[1].kind == ObjectKind::Light && l.objects[1].light.type == LightType::Spot);
        CHECK(near(l.objects[1].light.intensity, 42) && l.objects[1].parent == g.id);
        CHECK(l.objects[2].kind == ObjectKind::Bone && near(l.objects[2].boneLength, 1.5f) && l.objects[2].hasRest);
        CHECK(l.objects[3].kind == ObjectKind::Emitter && near(l.objects[3].particles.rate, 77) &&
              !l.objects[3].particles.additive);
        CHECK(near(l.ambient, {0.1f, 0.2f, 0.3f}));
    }

    // OBJ round trip keeps UVs (vt); only meshes are exported.
    int count = 0;
    CHECK(exportOBJ(s, "test_v2.obj", err, &count));
    CHECK(count == 1);
    Scene imp;
    CHECK(importOBJ(imp, "test_v2.obj", err));
    CHECK(imp.objects.size() == 1 && imp.objects[0].mesh.hasUVs());
    if (imp.objects.size() == 1 && imp.objects[0].mesh.hasUVs()) {
        const Mesh& a = s.objects[0].mesh;
        const Mesh& b = imp.objects[0].mesh;
        bool uvSame = a.uvs.size() == b.uvs.size();
        for (size_t f = 0; uvSame && f < a.uvs.size(); ++f)
            for (size_t k = 0; k < a.uvs[f].size(); ++k)
                uvSame &= near(a.uvs[f][k].x, b.uvs[f][k].x, 1e-5f) && near(a.uvs[f][k].y, b.uvs[f][k].y, 1e-5f);
        CHECK(uvSame);
    }
    std::remove("test_v2.m3d");
    std::remove("test_v2.obj");
    std::remove("test_v2.mtl");
}

// --- Unity-style Transform API ------------------------------------------------
static void testTransformApi() {
    Scene s;
    int parent = s.add(primitives::cube(), "Parent", {1, 1, 1});
    int child = s.add(primitives::cube(), "Child", {1, 1, 1});
    s.objects[parent].position = {10, 0, 0};
    s.objects[parent].rotation = {0, 90, 0};
    s.objects[parent].scale = {2, 2, 2};
    s.objects[child].parent = s.objects[parent].id;
    s.objects[child].position = {0, 0, 1};

    // World position and axes through the hierarchy (+Z of a parent turned
    // 90 degrees about Y points along world +X, scaled by 2).
    CHECK(near(tf::position(s, child), {12, 0, 0}, 1e-4f));
    CHECK(near(tf::forward(s, child), {1, 0, 0}, 1e-4f));
    CHECK(near(tf::lossyScale(s, child), {2, 2, 2}, 1e-4f));

    // setPosition / setRotation write world values back into parent space.
    tf::setPosition(s, child, {10, 5, 0});
    CHECK(near(tf::position(s, child), {10, 5, 0}, 1e-4f));
    CHECK(near(s.objects[child].position, {0, 2.5f, 0}, 1e-4f));
    tf::setRotation(s, child, Mat4());
    CHECK(near(tf::forward(s, child), {0, 0, 1}, 1e-4f));
    CHECK(near(s.objects[child].rotation, {0, -90, 0}, 1e-3f));

    // Translate in Self vs World space.
    int o = s.add(primitives::cube(), "Mover", {1, 1, 1});
    s.objects[o].rotation = {0, 90, 0};
    tf::translate(s, o, {0, 0, 1}, Space::Self);  // local forward = world +X
    CHECK(near(tf::position(s, o), {1, 0, 0}, 1e-4f));
    tf::translate(s, o, {0, 0, 1}, Space::World);
    CHECK(near(tf::position(s, o), {1, 0, 1}, 1e-4f));

    // Rotate: Self composes on the right, World on the left.
    tf::rotate(s, o, Vec3(0, 0, 90), Space::Self);
    CHECK(near(tf::up(s, o), {0, 0, -1}, 1e-3f) || near(tf::up(s, o), {0, 0, 1}, 1e-3f));
    tf::rotate(s, o, Vec3(0, 1, 0), 90.0f, Space::World);
    CHECK(near(tf::forward(s, o), {0, 0, -1}, 1e-3f));

    // RotateAround moves the position around the pivot and turns the object.
    int r = s.add(primitives::cube(), "Orbiter", {1, 1, 1});
    s.objects[r].position = {2, 0, 0};
    tf::rotateAround(s, r, {0, 0, 0}, {0, 1, 0}, 90.0f);
    CHECK(near(tf::position(s, r), {0, 0, -2}, 1e-4f));
    CHECK(near(tf::right(s, r), {0, 0, -1}, 1e-4f));

    // LookAt points forward() at the target with up() kept upward.
    int l = s.add(primitives::cube(), "Looker", {1, 1, 1});
    s.objects[l].position = {0, 0, 0};
    tf::lookAt(s, l, {3, 0, 0});
    CHECK(near(tf::forward(s, l), {1, 0, 0}, 1e-4f));
    CHECK(near(tf::up(s, l), {0, 1, 0}, 1e-4f));
    tf::lookAt(s, l, {0, 5, 0});  // straight up: falls back to another up vector
    CHECK(near(tf::forward(s, l), {0, 1, 0}, 1e-4f));

    // Point / direction / vector conversions are inverses of each other.
    Vec3 p(1.5f, -2, 0.25f);
    CHECK(near(tf::inverseTransformPoint(s, child, tf::transformPoint(s, child, p)), p, 1e-4f));
    CHECK(near(tf::inverseTransformDirection(s, child, tf::transformDirection(s, child, p)), p, 1e-4f));
    CHECK(near(tf::inverseTransformVector(s, child, tf::transformVector(s, child, p)), p, 1e-4f));
    CHECK(near(length(tf::transformDirection(s, child, {0, 0, 1})), 1.0f, 1e-4f));  // ignores scale
    CHECK(near(length(tf::transformVector(s, child, {0, 0, 1})), 2.0f, 1e-4f));     // includes scale

    tf::reset(s.objects[child]);
    CHECK(near(s.objects[child].position, Vec3()) && near(s.objects[child].scale, {1, 1, 1}));
    CHECK(near(tf::position(s, child), {10, 0, 0}, 1e-4f));  // now sits at the parent's origin

    // Id cache stays correct while objects are added and removed.
    uint32_t id = s.objects[r].id;
    CHECK(s.indexOf(id) == r);
    s.objects.erase(s.objects.begin());  // indices shift
    CHECK(s.indexOf(id) == r - 1 && s.objects[r - 1].id == id);
    CHECK(s.indexOf(999999) == -1);

    // Bounds + ray/box rejection used by picking.
    Mesh c = primitives::cube(2);
    Vec3 lo, hi;
    CHECK(c.bounds(lo, hi) && near(lo, {-1, -1, -1}) && near(hi, {1, 1, 1}));
    CHECK(rayHitsBox({0, 0, 5}, {0, 0, -1}, lo, hi));
    CHECK(!rayHitsBox({3, 0, 5}, {0, 0, -1}, lo, hi));
    CHECK(!rayHitsBox({0, 0, 5}, {0, 0, -1}, lo, hi, 3.0f));  // beyond tMax

    // Indexed render data matches the expanded triangle list.
    Mesh sphere = primitives::uvSphere(1, 24, 16);
    std::vector<RenderVertex> verts, flat;
    std::vector<uint32_t> idx;
    buildRenderMesh(sphere, sphere.verts, true, 40, -1, verts, idx);
    buildRenderData(sphere, sphere.verts, true, 40, -1, flat);
    CHECK(idx.size() == sphere.triangleCount() * 3 && flat.size() == idx.size());
    CHECK(verts.size() < idx.size() / 2);  // smooth vertices are shared
    bool same = true;
    for (size_t k = 0; k < idx.size(); ++k)
        same &= near(verts[idx[k]].pos, flat[k].pos) && near(verts[idx[k]].normal, flat[k].normal);
    CHECK(same);
    // Positions-only edits keep the topology stamp (cached edges stay valid).
    uint64_t topo = sphere.topology, ver = sphere.version;
    sphere.verts[0].y += 0.1f;
    sphere.touchPositions();
    CHECK(sphere.topology == topo && sphere.version != ver);
    sphere.touch();
    CHECK(sphere.topology != topo);
}

#include "tests_round5.inc"
#include "tests_round6.inc"
#include "tests_round7.inc"

int main() {
    jobs::init();
    testMath();
    testPrimitives();
    testCatmullClark();
    testExtrudeAndDelete();
    testRaycast();
    testFiles();
    testUV();
    testParametric();
    testHierarchy();
    testSkinning();
    testParticles();
    testFilesV2();
    testTransformApi();
    testJobs();
    testPolygonSolver();
    testBooleans();
    testBvh();
    testPathTracer();
    testKeyBindings();
    testExpressions();
    testEdgeExtrudeAndInset();
    testMeshAccel();
    testHierarchyDragDrop();
    testImageDecoding();
    testLightsCameraMaterials();
    testSceneFilesV3();
    testHardwareRayTracingMatchesCpu();
    testLoopsAndRings();
    testSubdivideAndLoopCut();
    testConnectAndPoke();
    testBevel();
    testBridgeFillPush();
    testMergeAndJoin();
    jobs::shutdown();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
