// Tests for the job system, n-gon solver, booleans, BVH, path tracer and
// key bindings. Included from tests.cpp.

struct Rng01 {
    rt::Rng r;
    explicit Rng01(uint64_t seed) : r(seed) {}
    float next() { return r.uniform(); }
};

static float polyArea(const std::vector<Vec3>& p, const std::vector<int>& face) {
    return length(faceNormalRaw(p, face)) * 0.5f;
}

static void testJobs() {
    for (int threads : {1, 4, jobs::hardwareThreads()}) {
        jobs::setThreadCount(threads);
        CHECK(jobs::threadCount() == threads);
        std::vector<int> v(100000, 0);
        jobs::parallelFor(0, (int)v.size(), 64, [&](int b, int e) {
            for (int i = b; i < e; ++i) v[i] += i % 7;
        });
        long long sum = 0, expect = 0;
        for (int i = 0; i < (int)v.size(); ++i) {
            sum += v[i];
            expect += i % 7;
        }
        CHECK(sum == expect);
        jobs::TaskGroup g;
        std::atomic<int> counter{0};
        for (int i = 0; i < 200; ++i) jobs::run(g, [&] { counter.fetch_add(1); });
        jobs::wait(g);
        CHECK(counter.load() == 200 && g.done());
        // Nested parallelFor from inside a parallelFor must not deadlock.
        std::atomic<int> nested{0};
        jobs::parallelFor(0, 8, 1, [&](int b, int e) {
            for (int i = b; i < e; ++i)
                jobs::parallelFor(0, 100, 10, [&](int bb, int ee) { nested.fetch_add(ee - bb); });
        });
        CHECK(nested.load() == 800);
    }
    jobs::setThreadCount(jobs::hardwareThreads());
}

static void testPolygonSolver() {
    // Concave L shape (6 corners) in the XZ plane, area 3.
    std::vector<Vec3> p = {{0, 0, 0}, {2, 0, 0}, {2, 0, -1}, {1, 0, -1}, {1, 0, -2}, {0, 0, -2}};
    std::vector<int> face = {0, 1, 2, 3, 4, 5};
    CHECK(!poly::isConvex(p, face));
    std::vector<int> tris;
    CHECK(poly::triangulate(p, face, tris));
    CHECK(tris.size() == 12);  // n - 2 triangles
    float area = 0;
    Vec3 n = normalize(faceNormalRaw(p, face));
    bool sameWinding = true;
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        std::vector<int> tri = {face[tris[t]], face[tris[t + 1]], face[tris[t + 2]]};
        area += polyArea(p, tri);
        sameWinding = sameWinding && dot(faceNormalRaw(p, tri), n) > 0;
    }
    CHECK(near(area, 3.0f, 1e-4f) && near(polyArea(p, face), 3.0f, 1e-4f));
    CHECK(sameWinding);

    // A square with a collinear extra vertex (as left by T-junction repair).
    std::vector<Vec3> q = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {2, 2, 0}, {0, 2, 0}};
    std::vector<int> qf = {0, 1, 2, 3, 4};
    CHECK(poly::isConvex(q, qf));
    tris.clear();
    poly::triangulate(q, qf, tris);
    area = 0;
    for (size_t t = 0; t + 2 < tris.size(); t += 3) area += polyArea(q, {qf[tris[t]], qf[tris[t + 1]], qf[tris[t + 2]]});
    CHECK(tris.size() == 9 && near(area, 4.0f, 1e-4f));

    // Star (concave, 10 corners).
    std::vector<Vec3> star;
    std::vector<int> sf;
    for (int i = 0; i < 10; ++i) {
        float a = i * kPi / 5, r = (i % 2) ? 0.4f : 1.0f;
        star.push_back({r * std::cos(a), r * std::sin(a), 0});
        sf.push_back(i);
    }
    tris.clear();
    CHECK(poly::triangulate(star, sf, tris) && tris.size() == 24);
    area = 0;
    for (size_t t = 0; t + 2 < tris.size(); t += 3)
        area += polyArea(star, {sf[tris[t]], sf[tris[t + 1]], sf[tris[t + 2]]});
    CHECK(near(area, polyArea(star, sf), 1e-4f));

    // Whole-mesh triangulation keeps the solid closed, oriented and its volume.
    Mesh cube = primitives::cube(2.0f);
    Mesh tri = cube;
    CHECK(poly::triangulateMesh(tri, 4) == 6);
    CHECK(tri.faces.size() == 12 && tri.hasUVs() && closedAndConsistent(tri));
    CHECK(near(signedVolume(tri), 8.0f, 1e-3f));
    CHECK(poly::triangulateMesh(tri, 4) == 0);
    // ... and tris -> quads restores the 6 quads.
    CHECK(poly::trisToQuads(tri) == 6 && tri.faces.size() == 6 && closedAndConsistent(tri));
    // N-gons only: a cylinder's caps are n-gons, its sides quads.
    Mesh cyl = primitives::cylinder(1.0f, 2.0f, 12);
    size_t quads = 0;
    for (auto& f : cyl.faces) quads += f.size() == 4;
    int split = poly::triangulateMesh(cyl, 5);
    size_t maxSides = 0;
    for (auto& f : cyl.faces) maxSides = std::max(maxSides, f.size());
    CHECK(split == 2 && maxSides == 4 && closedAndConsistent(cyl));

    // Clean-up: weld duplicates and repair a T-junction.
    Mesh t;
    t.verts = {{0, 0, 0}, {2, 0, 0}, {2, 1, 0}, {0, 1, 0},             // big quad
               {0, 1, 0}, {1, 1, 0}, {1, 2, 0}, {0, 2, 0},             // left top (duplicate (0,1))
               {1, 1, 0}, {2, 1, 0}, {2, 2, 0}, {1, 2, 0}};            // right top
    t.faces = {{0, 1, 2, 3}, {4, 5, 6, 7}, {8, 9, 10, 11}};
    t.touch();
    poly::CleanupStats st = poly::cleanup(t);
    CHECK(st.weldedVertices >= 4);
    CHECK(st.tJunctionsFixed >= 1);
    CHECK(t.verts.size() == 8);
    CHECK(t.faces[0].size() == 5);  // (1,1) inserted into the big quad's top edge
    std::map<std::pair<int, int>, int> undirected;
    for (auto& f : t.faces)
        for (size_t i = 0; i < f.size(); ++i) {
            int a = f[i], b = f[(i + 1) % f.size()];
            undirected[{std::min(a, b), std::max(a, b)}]++;
        }
    int shared = 0;
    for (auto& kv : undirected) shared += kv.second == 2;
    CHECK(shared == 3);  // (0,1)-(1,1), (1,1)-(2,1), (1,1)-(1,2)

    // Degenerate faces are removed.
    Mesh d = primitives::cube(2.0f);
    d.verts.push_back({5, 5, 5});
    d.faces.push_back({0, 0, (int)d.verts.size() - 1});
    d.touch();
    poly::CleanupStats sd = poly::cleanup(d);
    CHECK(sd.degenerateFaces >= 1 && d.faces.size() == 6 && d.verts.size() == 8);
}

static Mesh shifted(Mesh m, Vec3 o) {
    for (Vec3& v : m.verts) v += o;
    m.touch();
    return m;
}

static void testBooleans() {
    const Mesh a = primitives::cube(2.0f);  // [-1, 1]^3, volume 8
    const Mesh b = shifted(primitives::cube(2.0f), {1, 1, 1});
    struct Case {
        csg::Op op;
        float volume;
    } cases[] = {{csg::Op::Union, 15.0f}, {csg::Op::Difference, 7.0f}, {csg::Op::Intersection, 1.0f}};
    for (const Case& c : cases) {
        csg::Result r = csg::apply(a, b, c.op);
        CHECK(near(signedVolume(r.mesh), c.volume, 1e-3f));
        CHECK(r.mesh.hasUVs() && indicesValid(r.mesh));
        poly::cleanup(r.mesh);
        CHECK(closedAndConsistent(r.mesh));  // T-junction repair closes all cracks
        CHECK(near(signedVolume(r.mesh), c.volume, 1e-3f));
        poly::triangulateMesh(r.mesh, 4);
        bool allTris = true;
        for (auto& f : r.mesh.faces) allTris = allTris && f.size() == 3;
        CHECK(allTris && closedAndConsistent(r.mesh) && near(signedVolume(r.mesh), c.volume, 1e-3f));
    }
    // Disjoint union keeps both; a cavity cut from the inside lowers the volume.
    csg::Result u = csg::apply(a, shifted(primitives::cube(2.0f), {5, 0, 0}), csg::Op::Union);
    CHECK(near(signedVolume(u.mesh), 16.0f, 1e-3f));
    csg::Result cav = csg::apply(a, primitives::cube(1.0f), csg::Op::Difference);
    CHECK(near(signedVolume(cav.mesh), 7.0f, 1e-3f));
    csg::Result none = csg::apply(a, shifted(primitives::cube(2.0f), {5, 0, 0}), csg::Op::Intersection);
    CHECK(none.mesh.faces.empty());
    // Curved: a sphere minus a box through it; volume within the facet error.
    Mesh sphere = primitives::uvSphere(1.0f, 32, 16);
    float sv = signedVolume(sphere);
    csg::Result half = csg::apply(sphere, shifted(primitives::cube(4.0f), {0, 2, 0}), csg::Op::Difference);
    poly::cleanup(half.mesh);
    CHECK(near(signedVolume(half.mesh), sv * 0.5f, sv * 0.02f));
    CHECK(closedAndConsistent(half.mesh));
    // Coplanar faces (two boxes sharing a face) union into one box.
    csg::Result cop = csg::apply(a, shifted(primitives::cube(2.0f), {2, 0, 0}), csg::Op::Union);
    poly::cleanup(cop.mesh);
    CHECK(near(signedVolume(cop.mesh), 16.0f, 1e-3f) && closedAndConsistent(cop.mesh));
}

static void testBvh() {
    Rng01 rng(7);
    std::vector<Vec3> v;
    for (int i = 0; i < 3000; ++i) {
        Vec3 c(rng.next() * 20 - 10, rng.next() * 20 - 10, rng.next() * 20 - 10);
        for (int k = 0; k < 3; ++k) v.push_back(c + Vec3(rng.next(), rng.next(), rng.next()) - Vec3(0.5f, 0.5f, 0.5f));
    }
    Bvh bvh;
    bvh.build(v);
    CHECK(!bvh.nodes.empty() && bvh.order.size() == 3000 && bvh.maxDepth < 64);
    // Every triangle appears exactly once in the leaves.
    std::vector<int> seen(3000, 0);
    for (const BvhNode& n : bvh.nodes)
        if (n.count > 0)
            for (int i = n.leftOrFirst; i < n.leftOrFirst + n.count; ++i) seen[bvh.order[i]]++;
    CHECK(std::all_of(seen.begin(), seen.end(), [](int s) { return s == 1; }));
    int agree = 0, hits = 0;
    const int rays = 2000;
    for (int r = 0; r < rays; ++r) {
        Vec3 o(rng.next() * 30 - 15, rng.next() * 30 - 15, rng.next() * 30 - 15);
        Vec3 d = normalize(Vec3(rng.next() - 0.5f, rng.next() - 0.5f, rng.next() - 0.5f));
        // Brute force reference.
        float best = 1e30f;
        int bestTri = -1;
        for (int t = 0; t < 3000; ++t) {
            Vec3 e1 = v[3 * t + 1] - v[3 * t], e2 = v[3 * t + 2] - v[3 * t], p = cross(d, e2);
            float det = dot(e1, p);
            if (std::fabs(det) < 1e-12f) continue;
            Vec3 s = o - v[3 * t];
            float u = dot(s, p) / det;
            if (u < 0 || u > 1) continue;
            Vec3 q = cross(s, e1);
            float w = dot(d, q) / det;
            if (w < 0 || u + w > 1) continue;
            float tt = dot(e2, q) / det;
            if (tt > 1e-4f && tt < best) best = tt, bestTri = t;
        }
        Hit h;
        bool hit = intersectBvh(bvh, v, o, d, 1e-4f, h);
        if (bestTri >= 0) ++hits;
        if (hit == (bestTri >= 0) && (!hit || near(h.t, best, 1e-3f))) ++agree;
        if (hit) CHECK(occludedBvh(bvh, v, o, d, 1e-4f, h.t + 1e-3f));
        if (hit) CHECK(!occludedBvh(bvh, v, o, d, 1e-4f, h.t * 0.999f) || h.t < 1e-3f);
    }
    CHECK(agree == rays);
    CHECK(hits > 100);  // the test actually exercises hits

    // Large input: subtrees are built in parallel; must match a serial build.
    std::vector<Vec3> big;
    for (int i = 0; i < 60000; ++i) {
        Vec3 c(rng.next() * 40 - 20, rng.next() * 40 - 20, rng.next() * 40 - 20);
        for (int k = 0; k < 3; ++k) big.push_back(c + Vec3(rng.next(), rng.next(), rng.next()) * 0.6f);
    }
    jobs::setThreadCount(1);
    Bvh serial;
    serial.build(big);
    jobs::setThreadCount(std::max(4, jobs::hardwareThreads()));
    Bvh parallel;
    parallel.build(big);
    std::vector<int> count(60000, 0);
    bool childrenValid = true;
    for (const BvhNode& n : parallel.nodes) {
        if (n.count > 0)
            for (int i = n.leftOrFirst; i < n.leftOrFirst + n.count; ++i) count[parallel.order[i]]++;
        else
            childrenValid = childrenValid && n.leftOrFirst > 0 && n.leftOrFirst + 1 < (int)parallel.nodes.size();
    }
    CHECK(childrenValid && std::all_of(count.begin(), count.end(), [](int c) { return c == 1; }));
    int same = 0;
    for (int r = 0; r < 500; ++r) {
        Vec3 o(rng.next() * 50 - 25, rng.next() * 50 - 25, rng.next() * 50 - 25);
        Vec3 d = normalize(Vec3(rng.next() - 0.5f, rng.next() - 0.5f, rng.next() - 0.5f));
        Hit a, b;
        bool ha = intersectBvh(serial, big, o, d, 1e-4f, a), hb = intersectBvh(parallel, big, o, d, 1e-4f, b);
        if (ha == hb && (!ha || (a.tri == b.tri && near(a.t, b.t, 1e-5f)))) ++same;
    }
    CHECK(same == 500);
    jobs::setThreadCount(jobs::hardwareThreads());
}

static void testPathTracer() {
    // A grey plane lit straight from above by a unit sun; no sky, no bounces:
    // radiance = albedo * N.L + specK * (N.H)^shininess, as in the viewport.
    Scene sc;
    int plane = sc.add(primitives::plane(10.0f), "Floor", {0.5f, 0.5f, 0.5f});
    sc.objects[plane].gloss = 0.0f;
    Object sun;
    sun.kind = ObjectKind::Light;
    sun.name = "Sun";
    sun.light.type = LightType::Sun;
    sun.light.intensity = 1.0f;
    sun.light.color = {1, 1, 1};
    sun.position = {0, 5, 0};
    sc.addObject(sun);
    sc.ambient = {0, 0, 0};
    rt::Settings s;
    s.maxBounces = 0;
    s.lightSize = 0.0f;
    s.studioLights = false;
    rt::SceneData data;
    rt::buildScene(sc, s, data);
    CHECK(data.triangleCount() == 2 && data.lights.size() == 1 && data.materials.size() == 1);
    rt::Rng rng(1);
    Vec3 c = rt::trace(data, s, {0.3f, 3, 0.2f}, {0, -1, 0}, rng);
    const float albedo = std::pow(0.5f, 2.2f), expect = albedo + 0.03f;
    CHECK(near(c, Vec3(expect, expect, expect), 1e-3f));
    // A ray that misses sees the sky.
    sc.ambient = {0.1f, 0.2f, 0.3f};
    rt::buildScene(sc, s, data);
    CHECK(near(rt::trace(data, s, {0, 3, 0}, {0, 1, 0}, rng), Vec3(0.1f, 0.2f, 0.3f), 1e-5f));
    // An occluder casts a shadow: only the (zero) sky remains.
    sc.ambient = {0, 0, 0};
    int blocker = sc.add(primitives::cube(1.0f), "Blocker", {1, 1, 1});
    sc.objects[blocker].position = {0, 2, 0};
    rt::buildScene(sc, s, data);
    CHECK(near(rt::trace(data, s, {0.1f, 1.0f, 0.1f}, {0, -1, 0}, rng), Vec3(), 1e-5f));
    // Emission is seen directly (black, matte cube: only the 0.03 highlight is added).
    sc.objects[blocker].color = {0, 0, 0};
    sc.objects[blocker].gloss = 0.0f;
    sc.objects[blocker].emission = {1, 0.5f, 0.25f};
    sc.objects[blocker].emissionStrength = 2.0f;
    rt::buildScene(sc, s, data);
    CHECK(near(rt::trace(data, s, {0, 5, 0}, {0, -1, 0}, rng), Vec3(2.03f, 1.03f, 0.53f), 1e-3f));
    // With bounces, the floor under the glowing cube receives its light.
    s.maxBounces = 3;
    sc.objects.erase(sc.objects.begin() + 1);  // remove the sun
    rt::buildScene(sc, s, data);
    Vec3 sum;
    for (int i = 0; i < 4000; ++i) sum += rt::trace(data, s, {1.0f, 1.0f, 0.0f}, {0, -1, 0}, rng);
    CHECK(sum.x / 4000 > 0.01f && sum.x > sum.z);

    // Progressive CPU renderer: reaches the target and produces an image.
    rt::View view = rt::makeView({0, 4, 6}, {0, 0, 0}, 50.0f, false, 1.0f, 1.0f);
    auto shared = std::make_shared<rt::SceneData>(data);
    rt::CpuRenderer r;
    r.start(shared, view, s, 24, 24, 3);
    for (int guard = 0; guard < 100000 && r.running(); ++guard) {
        r.update();
        std::this_thread::yield();
    }
    CHECK(!r.running() && r.samples() == 3 && r.image().size() == 24 * 24 * 4);
    CHECK(r.samplesPerSecond() > 0);
    // Raising the target resumes it.
    r.setTargetSamples(5);
    for (int guard = 0; guard < 100000 && r.running(); ++guard) {
        r.update();
        std::this_thread::yield();
    }
    CHECK(r.samples() == 5);
    // Display transform: 0 -> 0, monotonic, saturates below 1.
    CHECK(near(rt::toDisplay(Vec3()), Vec3()) && rt::toDisplay(Vec3(100, 100, 100)).x <= 1.0f);
    CHECK(rt::toDisplay(Vec3(0.5f, 0.5f, 0.5f)).x > rt::toDisplay(Vec3(0.2f, 0.2f, 0.2f)).x);
}

static Input pressKey(int key, bool ctrl = false, bool shift = false, bool alt = false) {
    Input in;
    in.onKey(KEY_CONTROL, ctrl);
    in.onKey(KEY_SHIFT, shift);
    in.onKey(KEY_ALT, alt);
    in.onKey(key, true);
    return in;
}

static void testKeyBindings() {
    using input::Action;
    input::Binding b;
    CHECK(input::parse("C-S-Z", b) && b.key == 'Z' && b.ctrl && b.shift && !b.alt);
    CHECK(input::toString(b) == "C-S-Z");
    CHECK(input::parse("F5", b) && b.key == KEY_F5 && input::toString(b) == "F5");
    CHECK(input::parse("Space", b) && b.key == KEY_SPACE);
    CHECK(input::parse("A-Delete", b) && b.alt && b.key == KEY_DELETE);
    CHECK(input::parse("-", b) && !b.bound() && input::toString(b) == "-");
    CHECK(!input::parse("Q-X", b));

    input::KeyMap km;
    km.setPreset(input::Preset::Unity);
    CHECK(input::toString(km.slots[(int)Action::ToolMove][0]) == "W");
    CHECK(km.pressed(Action::ToolMove, pressKey('W')));
    CHECK(!km.pressed(Action::ToolMove, pressKey('W', true)));  // Ctrl+W is not W
    CHECK(km.pressed(Action::Redo, pressKey('Z', true, true)) && km.pressed(Action::Redo, pressKey('Y', true)));
    CHECK(km.pressed(Action::Undo, pressKey('Z', true)) && !km.pressed(Action::Undo, pressKey('Z', true, true)));
    // Held actions ignore Shift (speed boost) but not Ctrl.
    CHECK(km.down(Action::FlyForward, pressKey('W', false, true)) && !km.down(Action::FlyForward, pressKey('W', true)));

    // Rebinding steals the key from the other action in the same context only.
    input::Binding w;
    input::parse("W", w);
    CHECK(km.conflict(Action::FrameSelected, w) == Action::ToolMove);
    CHECK(km.assign(Action::FrameSelected, 0, w) == Action::ToolMove);
    CHECK(!km.slots[(int)Action::ToolMove][0].bound());
    CHECK(km.pressed(Action::FrameSelected, pressKey('W')) && !km.pressed(Action::ToolMove, pressKey('W')));
    CHECK(km.slots[(int)Action::FlyForward][0] == w);  // fly context keeps W

    // Round trip through the config format.
    std::string text = km.serialize();
    input::KeyMap other;
    other.setPreset(input::Preset::Blender);
    size_t pos = 0;
    int lines = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (other.parseLine(text.substr(pos, nl - pos))) ++lines;
        pos = nl + 1;
    }
    CHECK(lines == input::kActionCount);
    bool same = true;
    for (int a = 0; a < input::kActionCount; ++a)
        for (int s = 0; s < 2; ++s) same = same && other.slots[a][s] == km.slots[a][s];
    CHECK(same);
    // Every action id is unique and resolvable.
    for (int a = 0; a < input::kActionCount; ++a) {
        Action back;
        CHECK(input::actionFromId(input::info((Action)a).id, back) && back == (Action)a);
    }
    // Presets have no conflicts within a context.
    for (input::Preset p : {input::Preset::Unity, input::Preset::Blender}) {
        km.setPreset(p);
        int conflicts = 0;
        for (int a = 0; a < input::kActionCount; ++a)
            for (int s = 0; s < 2; ++s)
                if (km.conflict((Action)a, km.slots[a][s]) != Action::Count) ++conflicts;
        CHECK(conflicts == 0);
    }
}
