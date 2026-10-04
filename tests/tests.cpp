// Unit tests for the OpenGL-free core (math, mesh operations, file I/O).
// Build with -DMODELER_BUILD_TESTS=ON and run modeler_tests.
#include "mesh.h"
#include "scene.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

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

int main() {
    testMath();
    testPrimitives();
    testCatmullClark();
    testExtrudeAndDelete();
    testRaycast();
    testFiles();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
