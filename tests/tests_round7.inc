// Round 7: modeling tools that add vertices / faces, bridge, push through.
#include "meshtools.h"

static int eulerCharacteristic(const Mesh& m) {
    return (int)m.verts.size() - (int)uniqueEdges(m).size() + (int)m.faces.size();
}

static bool toolResultOk(const Mesh& m) {
    return meshedit::checkMesh(m).empty() && closedAndConsistent(m) && m.hasUVs();
}

static int findEdgeAtY(const Mesh& m, float y, meshedit::Edge& out) {
    for (const auto& e : uniqueEdges(m))
        if (std::fabs(m.verts[e.first].y - y) < 1e-4f && std::fabs(m.verts[e.second].y - y) < 1e-4f) {
            out = e;
            return 1;
        }
    return 0;
}

static std::vector<int> facesFacing(const Mesh& m, Vec3 dir) {
    std::vector<int> out;
    for (int f = 0; f < (int)m.faces.size(); ++f)
        if (dot(normalize(faceNormalRaw(m, m.faces[f])), dir) > 0.99f) out.push_back(f);
    return out;
}

static void testAddVertexTools() {
    using namespace meshedit;
    // Split one edge in three: two new vertices, still closed, same volume.
    {
        Mesh m = primitives::cube(2.0f);
        std::vector<char> sel;
        CHECK(subdivideEdges(m, {uniqueEdges(m)[0]}, 2, false, sel));
        CHECK(m.verts.size() == 10);
        CHECK(std::count(sel.begin(), sel.end(), 1) == 2);
        CHECK(toolResultOk(m));
        CHECK(near(signedVolume(m), 8.0f));
    }
    // Loop cut: the ring of 4 edges around the cube, 1 and 3 cuts.
    for (int cuts : {1, 3}) {
        Mesh m = primitives::cube(2.0f);
        std::vector<char> sel;
        auto ring = edgeRing(m, uniqueEdges(m)[0]);
        CHECK(ring.size() == 4);
        CHECK(loopCut(m, uniqueEdges(m)[0], cuts, sel));
        CHECK(m.faces.size() == 6u + 4u * cuts);
        CHECK(m.verts.size() == 8u + 4u * cuts);
        CHECK(toolResultOk(m));
        CHECK(near(signedVolume(m), 8.0f));
        for (const auto& f : m.faces) CHECK(f.size() == 4);  // a clean quad loop, no n-gons
    }
    // Edge loop / ring on a UV sphere: the equator loop goes all the way round.
    {
        Mesh s = primitives::uvSphere(1.0f, 12, 8);
        Edge e;
        CHECK(findEdgeAtY(s, 0.0f, e));
        CHECK(edgeLoop(s, e).size() == 12);
        CHECK(edgeRing(s, e).size() >= 5);
    }
    // Subdividing every edge of one face (connect) turns it into 4 quads; the
    // neighbours pick up the midpoints, so there are no cracks.
    {
        Mesh m = primitives::cube(2.0f);
        const auto top = facesFacing(m, {0, 1, 0});
        CHECK(top.size() == 1);
        std::vector<Edge> edges;
        const auto& f = m.faces[top[0]];
        for (size_t i = 0; i < f.size(); ++i) edges.push_back(makeEdge(f[i], f[(i + 1) % f.size()]));
        std::vector<char> sel;
        CHECK(subdivideEdges(m, edges, 1, true, sel));
        CHECK(facesFacing(m, {0, 1, 0}).size() == 4);
        CHECK(toolResultOk(m));
        CHECK(near(signedVolume(m), 8.0f));
    }
    // Connect two opposite corners of a face: splits it along the diagonal.
    {
        Mesh m = primitives::cube(2.0f);
        const auto top = facesFacing(m, {0, 1, 0});
        std::vector<char> sel(m.verts.size(), 0);
        sel[m.faces[top[0]][0]] = sel[m.faces[top[0]][2]] = 1;
        CHECK(connectVertices(m, sel) == 1);
        CHECK(m.faces.size() == 7);
        CHECK(toolResultOk(m));
        CHECK(near(signedVolume(m), 8.0f));
    }
    // Poke: flat keeps the volume; raised by 0.5 adds six pyramids of 2/3.
    {
        Mesh m = primitives::cube(2.0f);
        std::vector<int> all(6), nf;
        for (int i = 0; i < 6; ++i) all[i] = i;
        std::vector<char> sel;
        CHECK(pokeFaces(m, all, 0.0f, nf, sel));
        CHECK(m.faces.size() == 24 && nf.size() == 24);
        CHECK(toolResultOk(m));
        CHECK(near(signedVolume(m), 8.0f));
        Mesh r = primitives::cube(2.0f);
        CHECK(pokeFaces(r, all, 0.5f, nf, sel));
        CHECK(near(signedVolume(r), 12.0f, 1e-3f));
    }
}

static void testBevel() {
    using namespace meshedit;
    const float w = 0.25f;
    // One edge: a prism of w^2/2 * 2 is cut away; the end faces become pentagons.
    {
        Mesh m = primitives::cube(2.0f);
        std::vector<int> nf;
        std::vector<char> sel;
        BevelParams p;
        p.width = w;
        CHECK(bevel(m, {uniqueEdges(m)[0]}, {}, p, nf, sel));
        CHECK(m.faces.size() == 7);
        CHECK(nf.size() == 1);
        CHECK(toolResultOk(m));
        CHECK(near(signedVolume(m), 8.0f - w * w, 1e-4f));
    }
    // All 12 edges (chamfer): 12 strips + 8 corner triangles, exact volume.
    {
        Mesh m = primitives::cube(2.0f);
        std::vector<int> nf;
        std::vector<char> sel;
        BevelParams p;
        p.width = w;
        CHECK(bevel(m, uniqueEdges(m), {}, p, nf, sel));
        CHECK(m.faces.size() == 6 + 12 + 8);
        CHECK(toolResultOk(m));
        const float removed = 6 * (2 - 2 * w) * w * w + 8 * (5 * w * w * w / 6);
        CHECK(near(signedVolume(m), 8.0f - removed, 1e-4f));
        CHECK(eulerCharacteristic(m) == 2);
    }
    // Rounded (3 segments): closed, volume between the chamfer and the cube.
    {
        Mesh m = primitives::cube(2.0f);
        std::vector<int> nf;
        std::vector<char> sel;
        BevelParams p;
        p.width = w;
        p.segments = 3;
        CHECK(bevel(m, uniqueEdges(m), {}, p, nf, sel));
        CHECK(toolResultOk(m));
        const float v = signedVolume(m);
        CHECK(v > 8.0f - (6 * (2 - 2 * w) * w * w + 8 * (5 * w * w * w / 6)) && v < 8.0f);
        CHECK(eulerCharacteristic(m) == 2);
    }
    // Vertex bevel: cuts a corner tetrahedron (0.5^3 / 6).
    {
        Mesh m = primitives::cube(2.0f);
        std::vector<int> nf;
        std::vector<char> sel;
        BevelParams p;
        p.width = 0.5f;
        CHECK(bevel(m, {}, {0}, p, nf, sel));
        CHECK(m.faces.size() == 7);
        CHECK(toolResultOk(m));
        CHECK(near(signedVolume(m), 8.0f - 0.125f / 6.0f, 1e-4f));
    }
    // Width far too large is clamped (no overlap, still closed).
    {
        Mesh m = primitives::cube(2.0f);
        std::vector<int> nf;
        std::vector<char> sel;
        BevelParams p;
        p.width = 50.0f;
        CHECK(bevel(m, uniqueEdges(m), {}, p, nf, sel));
        CHECK(meshedit::checkMesh(m).empty());
        CHECK(signedVolume(m) > 0.0f);
    }
    // Bevel of an edge on a skinned + UV'd sphere keeps the attribute arrays in step.
    {
        Mesh s = primitives::uvSphere(1.0f, 12, 8);
        s.weights.assign(s.verts.size(), BoneWeights());
        for (auto& bw : s.weights) bw.add(0, 1.0f);
        Edge e;
        CHECK(findEdgeAtY(s, 0.0f, e));
        std::vector<int> nf;
        std::vector<char> sel;
        BevelParams p;
        p.width = 0.05f;
        p.segments = 2;
        CHECK(bevel(s, edgeLoop(s, e), {}, p, nf, sel));
        CHECK(meshedit::checkMesh(s).empty());
        CHECK(s.hasWeights() && s.hasUVs());
        CHECK(closedAndConsistent(s));
    }
}

static void testBridgeFillMergePush() {
    using namespace meshedit;
    // Two unit cubes 4 apart: bridge their facing faces into one solid.
    auto twoCubes = [] {
        Mesh m;
        appendMesh(m, primitives::cube(1.0f), translation({-2, 0, 0}));
        appendMesh(m, primitives::cube(1.0f), translation({2, 0, 0}));
        return m;
    };
    for (int segs : {1, 4}) {
        Mesh m = twoCubes();
        std::vector<int> faces = facesFacing(m, {1, 0, 0}), left;
        for (int f : faces)
            if (faceCenter(m, m.faces[f]).x < 0) left.push_back(f);
        for (int f : facesFacing(m, {-1, 0, 0}))
            if (faceCenter(m, m.faces[f]).x > 0) left.push_back(f);
        CHECK(left.size() == 2);
        BridgeParams bp;
        bp.segments = segs;
        std::vector<int> nf;
        std::vector<char> sel;
        std::string err;
        CHECK(bridgeRegions(m, left, bp, nf, sel, err));
        CHECK(err.empty());
        CHECK(nf.size() == 4u * segs);
        CHECK(toolResultOk(m));
        CHECK(near(signedVolume(m), 1 + 1 + 3, 1e-4f));
        CHECK(eulerCharacteristic(m) == 2);
    }
    // The same through border edges: delete the faces first, select both rings.
    {
        Mesh m = twoCubes();
        std::vector<char> kill(m.faces.size(), 0);
        for (int f : facesFacing(m, {1, 0, 0}))
            if (faceCenter(m, m.faces[f]).x < 0) kill[f] = 1;
        for (int f : facesFacing(m, {-1, 0, 0}))
            if (faceCenter(m, m.faces[f]).x > 0) kill[f] = 1;
        deleteFaces(m, kill);
        std::vector<Edge> border;
        {
            std::map<std::pair<int, int>, int> directed;
            for (const auto& f : m.faces)
                for (size_t i = 0; i < f.size(); ++i) directed[{f[i], f[(i + 1) % f.size()]}]++;
            for (const auto& kv : directed)
                if (!directed.count({kv.first.second, kv.first.first}))
                    border.push_back(makeEdge(kv.first.first, kv.first.second));
        }
        CHECK(border.size() == 8);
        std::vector<int> nf;
        std::vector<char> sel;
        std::string err;
        CHECK(bridgeLoops(m, border, BridgeParams(), nf, sel, err));
        CHECK(toolResultOk(m));
        CHECK(near(signedVolume(m), 5.0f, 1e-4f));
        // Wrong input is refused with a message.
        Mesh c = primitives::cube(2.0f);
        CHECK(!bridgeLoops(c, {uniqueEdges(c)[0]}, BridgeParams(), nf, sel, err));
        CHECK(!err.empty());
    }
    // Fill: delete a face of a cube, fill its border again.
    {
        Mesh m = primitives::cube(2.0f);
        std::vector<char> kill(m.faces.size(), 0);
        kill[0] = 1;
        deleteFaces(m, kill);
        CHECK(!closedAndConsistent(m));
        std::vector<int> nf;
        std::string err;
        std::vector<Edge> all = uniqueEdges(m);
        std::vector<Edge> border;
        auto ef = [&](Edge e) {
            int count = 0;
            for (const auto& f : m.faces)
                for (size_t i = 0; i < f.size(); ++i)
                    if (makeEdge(f[i], f[(i + 1) % f.size()]) == e) ++count;
            return count;
        };
        for (const auto& e : all)
            if (ef(e) == 1) border.push_back(e);
        CHECK(fillHoles(m, border, nf, err) == 1);
        CHECK(closedAndConsistent(m));
        CHECK(near(signedVolume(m), 8.0f));
    }
    // Merge the top four corners: a pyramid of base 4, height 2.
    {
        Mesh m = primitives::cube(2.0f);
        std::vector<char> sel(m.verts.size(), 0);
        for (size_t v = 0; v < m.verts.size(); ++v) sel[v] = m.verts[v].y > 0;
        CHECK(mergeVertices(m, sel, false) == 3);
        CHECK(m.verts.size() == 5);
        CHECK(closedAndConsistent(m));
        CHECK(near(signedVolume(m), 8.0f / 3.0f));
    }
    // Push through with a frame: a 1x1 hole through a 2x2x2 box -> a ring.
    {
        Mesh m = primitives::cube(2.0f);
        auto front = facesFacing(m, {0, 0, 1});
        PushThroughParams pp;
        pp.inset = 0.5f;
        std::vector<int> nf;
        std::vector<char> sel;
        std::string err;
        CHECK(pushThrough(m, front, pp, nf, sel, err));
        CHECK(err.empty());
        CHECK(nf.size() == 4);
        CHECK(toolResultOk(m));
        CHECK(near(signedVolume(m), 8.0f - 2.0f, 1e-4f));
        CHECK(eulerCharacteristic(m) == 0);  // genus 1: it has a hole now
    }
    // Push through an inset that already exists on both sides (matching loops).
    {
        Mesh m = primitives::cube(2.0f);
        InsetParams ip;
        ip.thickness = 0.5f;
        std::vector<int> innerF, innerB;
        std::vector<char> sel;
        CHECK(insetFaces(m, facesFacing(m, {0, 0, -1}), ip, innerB, sel));
        CHECK(insetFaces(m, facesFacing(m, {0, 0, 1}), ip, innerF, sel));
        std::vector<int> nf;
        std::string err;
        CHECK(pushThrough(m, innerF, PushThroughParams(), nf, sel, err));
        CHECK(toolResultOk(m));
        CHECK(near(signedVolume(m), 6.0f, 1e-4f));
        CHECK(eulerCharacteristic(m) == 0);
    }
    // Pushing a whole side through is refused (the hole would be the wall).
    {
        Mesh m = primitives::cube(2.0f);
        std::vector<int> nf;
        std::vector<char> sel;
        std::string err;
        CHECK(!pushThrough(m, facesFacing(m, {0, 0, 1}), PushThroughParams(), nf, sel, err));
        CHECK(!err.empty());
        CHECK(closedAndConsistent(m) && m.faces.size() == 6);  // unchanged
    }
    // Join keeps winding under a mirror transform.
    {
        Mesh m;
        appendMesh(m, primitives::cube(1.0f), scaling({-1, 1, 1}));
        CHECK(closedAndConsistent(m));
        CHECK(signedVolume(m) > 0.99f);
    }
}

// Round 7 core changes: key names for 65% keyboards, position-only render
// refresh, deep hierarchies.
static void testRound7Core() {
    using input::Action;
    input::Binding b;
    CHECK(input::parse("Minus", b) && b.key == '-' && input::toString(b) == "Minus");
    CHECK(input::parse("C-Equals", b) && b.key == '=' && b.ctrl && input::toString(b) == "C-Equals");
    CHECK(input::parse("S-/", b) && b.key == '/' && b.shift && input::toString(b) == "S-/");
    CHECK(input::parse("]", b) && b.key == ']');
    CHECK(input::parse("PgDn", b) && b.key == KEY_PAGEDOWN);
    input::KeyMap km;
    for (input::Preset p : {input::Preset::Unity, input::Preset::Blender}) {
        km.setPreset(p);
        // Every F-key action has a second binding without F-keys.
        for (Action a : {Action::Help, Action::Stats, Action::Screenshot, Action::Render}) {
            bool nonF = false;
            for (const auto& s : km.slots[(int)a])
                nonF = nonF || (s.bound() && !(s.key >= KEY_F1 && s.key <= KEY_F12));
            CHECK(nonF);
        }
        CHECK(km.pressed(Action::Palette, pressKey('K', true)));
        // Alt+Left orbits; it no longer also moves the camera sideways.
        CHECK(km.down(Action::OrbitLeft, pressKey(KEY_LEFT, false, false, true)));
        CHECK(!km.down(Action::CameraLeft, pressKey(KEY_LEFT, false, false, true)) || p == input::Preset::Blender);
        CHECK(km.down(Action::ZoomIn, pressKey('=')));
    }

    // Position-only refresh gives the same vertices as a full rebuild.
    auto sameAsRebuild = [](Mesh m, bool smooth) {
        std::vector<RenderVertex> v0, v1;
        std::vector<uint32_t> i0, i1;
        RenderMap map;
        buildRenderMesh(m, m.verts, smooth, 40.0f, -1, v0, i0, &map);
        uint32_t s = 99;
        for (Vec3& p : m.verts) {
            s = s * 1664525u + 1013904223u;
            p += Vec3((s >> 8) % 100 * 1e-4f, (s >> 12) % 100 * 1e-4f, (s >> 16) % 100 * 1e-4f);
        }
        m.touchPositions();
        buildRenderMesh(m, m.verts, smooth, 40.0f, -1, v1, i1);
        std::vector<RenderVertex> fast;
        refreshRenderMesh(m, m.verts, -1, map, fast);
        if (fast.size() != v1.size() || i0 != i1) return false;
        for (size_t k = 0; k < fast.size(); ++k)
            if (!near(fast[k].pos, v1[k].pos, 1e-6f) || !near(fast[k].normal, v1[k].normal, 2e-3f) ||
                std::fabs(fast[k].uv.x - v1[k].uv.x) > 1e-6f)
                return false;
        return true;
    };
    CHECK(sameAsRebuild(primitives::uvSphere(1.0f, 16, 12), true));
    CHECK(sameAsRebuild(primitives::cylinder(1.0f, 2.0f, 16), true));  // crease corners (averaged groups)
    CHECK(sameAsRebuild(primitives::torus(1.0f, 0.3f, 16, 8), false));

    // A 1000-deep chain: correct world matrices past the old 256 limit,
    // cycles refused at any depth, descendants in one pass.
    Scene sc;
    uint32_t prev = 0;
    for (int i = 0; i < 1000; ++i) {
        Object o;
        o.kind = ObjectKind::Empty;
        o.position = {0.01f, 0, 0};
        o.parent = prev;
        int idx = sc.addObject(o);
        prev = sc.objects[idx].id;
    }
    std::vector<Mat4> worlds;
    sc.computeWorlds(worlds);
    CHECK(near(transformPoint(sc.world(999), Vec3()), Vec3(10.0f, 0, 0), 1e-3f));
    CHECK(near(transformPoint(worlds[999], Vec3()), Vec3(10.0f, 0, 0), 1e-3f));
    CHECK(sc.isAncestor(0, 999));
    CHECK(!sc.setParent(0, 999, true));
    CHECK(sc.descendants(0).size() == 999);
    CHECK(sc.depth(999) == 999);
    {
        WorldCacheScope cache(sc);
        CHECK(near(transformPoint(sc.world(999), Vec3()), Vec3(10.0f, 0, 0), 1e-3f));
    }
}

// Saving a dense mesh formats its lines in parallel chunks: the file must
// round-trip exactly like a serial write.
static void testParallelSave() {
    Scene sc;
    sc.add(catmullClark(catmullClark(catmullClark(primitives::uvSphere(1.0f, 64, 32)))), "Dense", {0.5f, 0.5f, 0.5f});
    const Mesh& src = sc.objects[0].mesh;
    CHECK(src.verts.size() > 40000);
    std::string err;
    CHECK(saveScene(sc, "test_parallel.m3d", err));
    Scene back;
    CHECK(loadScene(back, "test_parallel.m3d", err));
    std::remove("test_parallel.m3d");
    CHECK(back.objects.size() == 1);
    if (back.objects.size() == 1) {
        const Mesh& m = back.objects[0].mesh;
        CHECK(m.verts.size() == src.verts.size() && m.faces.size() == src.faces.size() && m.hasUVs());
        bool same = m.verts.size() == src.verts.size();
        for (size_t i = 0; same && i < m.verts.size(); ++i)
            same = m.verts[i].x == src.verts[i].x && m.verts[i].y == src.verts[i].y && m.verts[i].z == src.verts[i].z;
        for (size_t f = 0; same && f < m.faces.size(); ++f) same = m.faces[f] == src.faces[f] && m.uvs[f].size() == src.uvs[f].size();
        CHECK(same);
    }
}

// Found by the stress test's tool fuzzer: a boolean difference of a
// loop-cut cone and a cube ran out of memory (BSP splitting never ended).
static void testFuzzBooleanRegression() {
    Scene sc;
    std::string err;
    CHECK(loadScene(sc, std::string(MODELER_TEST_DATA) + "/fuzz_boolean_cone.m3d", err));
    if (sc.objects.size() != 2) return;
    csg::Result r = csg::apply(sc.objects[0].mesh, sc.objects[1].mesh, csg::Op::Difference);
    CHECK(meshedit::checkMesh(r.mesh).empty());
    CHECK(r.outputPolygons < 100000);
}

// Replays a failure dumped by the stress test's tool fuzzer (M3D_FUZZ_TRACE):
//   M3D_REPLAY=path/fuzz_fail_03 modeler_tests
static int replayFuzzCase(const std::string& base, bool verbose, bool& closedBefore, bool& closedAfter,
                          std::string& check) {
    using namespace meshedit;
    Scene sc;
    std::string err;
    if (!loadScene(sc, base + ".m3d", err) || sc.objects.empty()) {
        std::printf("cannot load %s.m3d: %s\n", base.c_str(), err.c_str());
        return 2;
    }
    Mesh m = sc.objects[0].mesh;
    FILE* f = std::fopen((base + ".txt").c_str(), "rb");
    if (!f) return 2;
    std::string tool, params, line;
    std::vector<int> faces, verts;
    std::vector<Edge> edges;
    char buf[1 << 16];
    while (std::fgets(buf, sizeof buf, f)) {
        line = buf;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        auto rest = [&](size_t n) { return line.size() > n ? line.substr(n) : std::string(); };
        if (line.rfind("tool ", 0) == 0) tool = rest(5);
        else if (line.rfind("params ", 0) == 0) params = rest(7);
        else if (line.rfind("faces", 0) == 0 || line.rfind("verts", 0) == 0 || line.rfind("edges", 0) == 0) {
            std::vector<int> nums;
            const char* p = line.c_str() + 5;
            char* e;
            for (long v = std::strtol(p, &e, 10); e != p; v = std::strtol(p, &e, 10)) nums.push_back((int)v), p = e;
            if (line[0] == 'f') faces = nums;
            else if (line[0] == 'v') verts = nums;
            else
                for (size_t i = 0; i + 1 < nums.size(); i += 2) edges.push_back({nums[i], nums[i + 1]});
        }
    }
    std::fclose(f);
    auto num = [&](const char* key) {
        size_t p = params.find(key);
        return p == std::string::npos ? 0.0f : std::strtof(params.c_str() + p + std::string(key).size() + 1, nullptr);
    };
    if (verbose)
        std::printf("replay %s: %s [%s] on %d faces\n", base.c_str(), tool.c_str(), params.c_str(), (int)m.faces.size());
    const bool wasClosed = closedAndConsistent(m);
    std::vector<int> nf;
    std::vector<char> sel;
    bool ok = false;
    if (tool == "bevel edges" || tool == "bevel verts") {
        BevelParams p;
        p.width = num("width");
        p.segments = (int)num("segments");
        p.profile = num("profile");
        p.clamp = num("clamp") != 0;
        ok = tool == "bevel edges" ? bevel(m, edges, {}, p, nf, sel) : bevel(m, {}, verts, p, nf, sel);
    } else if (tool == "extrude faces") {
        ok = extrudeFaces(m, faces, sel, nullptr);
    } else if (tool == "inset") {
        InsetParams p;
        p.thickness = num("thickness");
        p.depth = num("depth");
        p.dish = num("dish");
        p.individual = num("individual") != 0;
        ok = insetFaces(m, faces, p, nf, sel);
    } else if (tool == "connect") {
        std::vector<char> vs(m.verts.size(), 0);
        for (int v : verts) vs[v] = 1;
        ok = connectVertices(m, vs) > 0;
    } else if (tool == "bridge faces") {
        BridgeParams p;
        p.segments = (int)num("segments");
        ok = bridgeRegions(m, faces, p, nf, sel, err);
    } else if (tool == "loop cut") {
        Edge e((int)num("edge"), 0);
        size_t q = params.find("edge ");
        if (q != std::string::npos) std::sscanf(params.c_str() + q, "edge %d %d", &e.first, &e.second);
        ok = loopCut(m, e, (int)num("cuts"), sel);
    } else if (tool == "subdivide") {
        ok = subdivideEdges(m, edges, (int)num("cuts"), num("connect") != 0, sel);
    } else if (tool == "triangulate") {
        ok = poly::triangulateMesh(m, 4) > 0;
    }
    closedBefore = wasClosed;
    closedAfter = closedAndConsistent(m);
    check = checkMesh(m);
    if (verbose)
        std::printf("  ok %d, check \"%s\", closed before %d after %d\n", (int)ok, check.c_str(), (int)wasClosed,
                    (int)closedAfter);
    if (std::getenv("M3D_REPLAY_EDGES")) {
        std::map<std::pair<int, int>, int> d;
        for (const auto& fc : m.faces)
            for (size_t i = 0; i < fc.size(); ++i) d[{fc[i], fc[(i + 1) % fc.size()]}]++;
        int shown = 0;
        for (const auto& kv : d)
            if ((kv.second != 1 || !d.count({kv.first.second, kv.first.first})) && shown++ < 12)
                std::printf("  open/dup half-edge %d -> %d (x%d)\n", kv.first.first, kv.first.second, kv.second);
        std::printf("  input %d verts, %d edges selected\n", (int)sc.objects[0].mesh.verts.size(), (int)edges.size());
    }
    return 0;
}

static int replayFuzz(const std::string& base) {
    bool before = false, after = false;
    std::string check;
    return replayFuzzCase(base, true, before, after, check);
}

// Failures the fuzzer found (pinched selections, collinear n-gon corners,
// two holes meeting at a vertex, chords along existing edges, a profile
// between identical points) - each must now give a valid mesh that stays
// closed when it was closed.
static void testFuzzRegressions() {
    const std::string dir = std::string(MODELER_TEST_DATA) + "/fuzz/";
    int cases = 0;
    for (int n = 0; n < 100; ++n) {
        char name[32];
        std::snprintf(name, sizeof name, "fuzz_fail_%02d", n);
        FILE* f = std::fopen((dir + name + ".txt").c_str(), "rb");
        if (!f) continue;
        std::fclose(f);
        ++cases;
        bool before = false, after = false;
        std::string check = "?";
        replayFuzzCase(dir + name, false, before, after, check);
        CHECK(check.empty());
        CHECK(!before || after);
        if (!check.empty() || (before && !after)) std::printf("  regression %s: \"%s\" closed %d -> %d\n", name, check.c_str(), (int)before, (int)after);
    }
    CHECK(cases >= 20);
}
