// Round 7: tools that add vertices and faces (loops, rings, subdivide, loop
// cut, connect, poke, bevel, bridge, fill, push-in / punch-through, merge,
// join). Included from tests.cpp.

// Index of the vertex of `m` at position p (or -1).
static int vertexAt(const Mesh& m, Vec3 p) {
    for (size_t v = 0; v < m.verts.size(); ++v)
        if (near(m.verts[v], p, 1e-4f)) return (int)v;
    return -1;
}

static std::vector<char> vertsWhere(const Mesh& m, bool (*pred)(Vec3)) {
    std::vector<char> s(m.verts.size(), 0);
    for (size_t v = 0; v < m.verts.size(); ++v) s[v] = pred(m.verts[v]);
    return s;
}

// Faces whose every corner satisfies pred.
static std::vector<int> facesWhere(const Mesh& m, bool (*pred)(Vec3)) {
    std::vector<int> out;
    for (size_t f = 0; f < m.faces.size(); ++f) {
        bool all = true;
        for (int v : m.faces[f]) all = all && pred(m.verts[v]);
        if (all) out.push_back((int)f);
    }
    return out;
}

static bool uvsConsistent(const Mesh& m) {
    if (!m.hasUVs()) return false;
    for (size_t f = 0; f < m.faces.size(); ++f)
        if (m.uvs[f].size() != m.faces[f].size()) return false;
    return true;
}

static void testLoopsAndRings() {
    // A cylinder's side: a vertical edge's ring goes all the way round, a
    // horizontal edge's loop does too.
    Mesh cyl = primitives::cylinder(1, 2, 12);
    int top = -1, bottom = -1;
    for (size_t v = 0; v < cyl.verts.size(); ++v) {
        if (near(cyl.verts[v], Vec3(0, 1, 1), 1e-3f)) top = (int)v;
        if (near(cyl.verts[v], Vec3(0, -1, 1), 1e-3f)) bottom = (int)v;
    }
    CHECK(top >= 0 && bottom >= 0);
    if (top >= 0 && bottom >= 0) {
        auto ring = meshedit::edgeRing(cyl, meshedit::makeEdge(top, bottom));
        CHECK(ring.size() == 12);
        auto loop = meshedit::edgeLoop(cyl, meshedit::makeEdge(top, bottom));
        CHECK(loop.size() == 1);  // the loop ends at the caps (valence 3)
    }
    // A grid: a loop runs straight across, end to end.
    Mesh g = primitives::grid(4, 4, 4, 4);
    int a = vertexAt(g, {-1, 0, 0}), b = vertexAt(g, {0, 0, 0});
    CHECK(a >= 0 && b >= 0);
    if (a >= 0 && b >= 0) {
        auto loop = meshedit::edgeLoop(g, meshedit::makeEdge(a, b));
        CHECK(loop.size() == 4);
        for (const auto& e : loop) CHECK(near(g.verts[e.first].z, 0.0f) && near(g.verts[e.second].z, 0.0f));
        auto ring = meshedit::edgeRing(g, meshedit::makeEdge(a, b));
        CHECK(ring.size() == 5);  // 4 quads crossed -> 5 parallel edges
    }
    // Border loop of the grid: follows the open edge all the way round.
    int c0 = vertexAt(g, {-2, 0, -2}), c1 = vertexAt(g, {-1, 0, -2});
    if (c0 >= 0 && c1 >= 0) CHECK(meshedit::edgeLoop(g, meshedit::makeEdge(c0, c1)).size() == 16);
}

static void testSubdivideAndLoopCut() {
    Mesh c = primitives::cube(2);
    int a = vertexAt(c, {-1, 1, 1}), b = vertexAt(c, {1, 1, 1});
    CHECK(a >= 0 && b >= 0);
    std::vector<meshedit::Edge> pieces;
    std::vector<char> vsel;
    CHECK(meshedit::subdivideEdges(c, {meshedit::makeEdge(a, b)}, 2, pieces, vsel));
    CHECK(c.verts.size() == 10 && c.faces.size() == 6 && pieces.size() == 3);
    CHECK(closedAndConsistent(c) && near(signedVolume(c), 8.0f, 1e-3f) && uvsConsistent(c));
    CHECK(std::count(vsel.begin(), vsel.end(), 1) == 2);

    // Loop cut across a cube: the ring of 4 side quads gets one new loop.
    Mesh k = primitives::cube(2);
    a = vertexAt(k, {-1, -1, 1});
    b = vertexAt(k, {-1, 1, 1});
    std::vector<meshedit::Edge> loopEdges;
    meshedit::LoopCutParams lp;
    CHECK(meshedit::loopCut(k, {meshedit::makeEdge(a, b)}, lp, loopEdges, vsel));
    CHECK(k.faces.size() == 10 && k.verts.size() == 12 && loopEdges.size() == 4);
    CHECK(closedAndConsistent(k) && near(signedVolume(k), 8.0f, 1e-3f) && uvsConsistent(k));
    int onMid = 0;
    for (size_t v = 0; v < k.verts.size(); ++v) onMid += vsel[v] && near(k.verts[v].y, 0.0f);
    CHECK(onMid == 4);
    // Slide moves the single cut towards one side, consistently round the ring.
    Mesh s = primitives::cube(2);
    a = vertexAt(s, {-1, -1, 1});
    b = vertexAt(s, {-1, 1, 1});
    lp.slide = 0.5f;
    CHECK(meshedit::loopCut(s, {meshedit::makeEdge(a, b)}, lp, loopEdges, vsel));
    float y0 = -100;
    bool same = true;
    for (size_t v = 0; v < s.verts.size(); ++v)
        if (vsel[v]) {
            if (y0 == -100) y0 = s.verts[v].y;
            same = same && near(s.verts[v].y, y0);
        }
    CHECK(same && std::fabs(y0) > 0.3f);
    // Three cuts at once; non-quad neighbours (cylinder caps) get the vertices.
    Mesh cyl = primitives::cylinder(1, 2, 8);
    int t = vertexAt(cyl, {0, 1, 1}), u = -1;
    for (size_t v = 0; v < cyl.verts.size(); ++v)
        if (near(cyl.verts[v].y, 1.0f) && (int)v != t && length(cyl.verts[v] - cyl.verts[t]) < 0.8f &&
            length(cyl.verts[v] - cyl.verts[t]) > 0.1f)
            u = (int)v;
    CHECK(t >= 0 && u >= 0);
    lp = meshedit::LoopCutParams();
    lp.cuts = 3;
    size_t facesBefore = cyl.faces.size();
    CHECK(meshedit::loopCut(cyl, {meshedit::makeEdge(t, u)}, lp, loopEdges, vsel));
    CHECK(closedAndConsistent(cyl) && cyl.faces.size() == facesBefore + 3);
    CHECK(near(signedVolume(cyl), signedVolume(primitives::cylinder(1, 2, 8)), 1e-3f));

    // Weights survive and stay normalized.
    Mesh w = primitives::cube(2);
    w.weights.resize(w.verts.size());
    for (size_t v = 0; v < w.verts.size(); ++v) w.weights[v].add(w.verts[v].y > 0 ? 1 : 0, 1.0f);
    a = vertexAt(w, {-1, -1, 1});
    b = vertexAt(w, {-1, 1, 1});
    CHECK(meshedit::loopCut(w, {meshedit::makeEdge(a, b)}, meshedit::LoopCutParams(), loopEdges, vsel));
    CHECK(w.hasWeights());
    bool normalized = true;
    for (const auto& bw : w.weights) normalized = normalized && near(bw.total(), 1.0f, 1e-3f);
    CHECK(normalized);
}

static void testConnectAndPoke() {
    Mesh c = primitives::cube(2);
    std::vector<char> vsel(c.verts.size(), 0);
    vsel[vertexAt(c, {-1, 1, 1})] = vsel[vertexAt(c, {1, 1, -1})] = 1;  // diagonal of the top face
    std::vector<meshedit::Edge> ne;
    CHECK(meshedit::connectVertices(c, vsel, ne) == 1);
    CHECK(c.faces.size() == 7 && ne.size() == 1 && closedAndConsistent(c) && uvsConsistent(c));
    // Adjacent corners are already connected: nothing to do.
    Mesh d = primitives::cube(2);
    std::vector<char> adj(d.verts.size(), 0);
    adj[vertexAt(d, {-1, 1, 1})] = adj[vertexAt(d, {1, 1, 1})] = 1;
    CHECK(meshedit::connectVertices(d, adj, ne) == 0);

    Mesh p = primitives::cube(2);
    std::vector<int> nf;
    CHECK(meshedit::pokeFaces(p, facesWhere(p, [](Vec3 v) { return v.y > 0.9f; }), 0.5f, nf, vsel) == 1);
    CHECK(p.faces.size() == 9 && nf.size() == 4 && closedAndConsistent(p));
    CHECK(near(signedVolume(p), 8.0f + 4.0f * 0.5f / 3.0f, 1e-3f));  // a pyramid on top
}

static void testBevel() {
    std::vector<int> nf;
    std::vector<char> vsel;
    float used = 0;
    // All 12 edges of a cube: the classic chamfered cube.
    Mesh c = primitives::cube(2);
    std::vector<meshedit::Edge> all;
    for (const auto& e : uniqueEdges(c)) all.push_back(e);
    meshedit::BevelParams bp;
    bp.width = 0.2f;
    CHECK(meshedit::bevel(c, all, {}, bp, nf, vsel, &used));
    CHECK(near(used, 0.2f));
    CHECK(c.faces.size() == 26 && c.verts.size() == 24 && nf.size() == 20);
    CHECK(closedAndConsistent(c) && indicesValid(c) && uvsConsistent(c));
    float vol = signedVolume(c);
    CHECK(vol < 8.0f && vol > 7.5f);
    // One edge: one strip, the far corners get cut into their third face.
    Mesh one = primitives::cube(2);
    int a = vertexAt(one, {-1, 1, 1}), b = vertexAt(one, {1, 1, 1});
    CHECK(meshedit::bevel(one, {meshedit::makeEdge(a, b)}, {}, bp, nf, vsel));
    CHECK(one.faces.size() == 7 && one.verts.size() == 10 && closedAndConsistent(one));
    CHECK(near(signedVolume(one), 8.0f - 0.5f * 0.2f * 0.2f * 2.0f, 1e-3f));  // a triangular prism removed
    // Rounded: 3 segments per strip.
    Mesh r = primitives::cube(2);
    bp.segments = 3;
    CHECK(meshedit::bevel(r, all, {}, bp, nf, vsel));
    CHECK(closedAndConsistent(r) && indicesValid(r) && r.faces.size() == 6 + 12 * 3 + 8);
    float rv = signedVolume(r);
    CHECK(rv > vol && rv < 8.0f);  // round bevels remove less than flat ones
    // Width clamped to stop edges collapsing.
    Mesh big = primitives::cube(2);
    bp.segments = 1;
    bp.width = 5.0f;
    CHECK(meshedit::bevel(big, all, {}, bp, nf, vsel, &used));
    CHECK(used < 1.0f && closedAndConsistent(big));
    // Vertex bevel: chamfer one corner.
    Mesh vb = primitives::cube(2);
    std::vector<char> corner(vb.verts.size(), 0);
    corner[vertexAt(vb, {1, 1, 1})] = 1;
    bp.width = 0.5f;
    bp.vertexOnly = true;
    CHECK(meshedit::bevel(vb, {}, corner, bp, nf, vsel));
    CHECK(vb.faces.size() == 7 && vb.verts.size() == 10 && closedAndConsistent(vb));
    CHECK(near(signedVolume(vb), 8.0f - 0.5f * 0.5f * 0.5f / 6.0f, 1e-3f));
    // Border edges are ignored (nothing to bevel on an open edge).
    Mesh q = primitives::plane(2, 1);
    std::vector<meshedit::Edge> border;
    for (const auto& e : uniqueEdges(q)) border.push_back(e);
    bp = meshedit::BevelParams();
    CHECK(!meshedit::bevel(q, border, {}, bp, nf, vsel));
}

static void testBridgeFillPush() {
    std::vector<int> nf;
    std::vector<char> vsel;
    std::string err;
    // Two cubes side by side; bridging their facing faces makes one solid.
    Mesh two = primitives::cube(2);
    meshedit::appendMesh(two, primitives::cube(2), translation({4, 0, 0}));
    CHECK(two.faces.size() == 12 && two.hasUVs());
    std::vector<int> facing = facesWhere(two, [](Vec3 v) { return std::fabs(v.x - 1) < 1e-4f || std::fabs(v.x - 3) < 1e-4f; });
    CHECK(facing.size() == 2);
    meshedit::BridgeParams bp;
    CHECK(meshedit::bridgeFaces(two, facing, bp, nf, vsel, err));
    CHECK(two.faces.size() == 14 && nf.size() == 4 && closedAndConsistent(two) && uvsConsistent(two));
    CHECK(near(signedVolume(two), 8.0f * 3.0f, 1e-3f));  // a 6 x 2 x 2 bar
    // Segments add rings along the bridge.
    Mesh seg = primitives::cube(2);
    meshedit::appendMesh(seg, primitives::cube(2), translation({4, 0, 0}));
    bp.segments = 4;
    CHECK(meshedit::bridgeFaces(seg, facesWhere(seg, [](Vec3 v) { return std::fabs(v.x - 1) < 1e-4f || std::fabs(v.x - 3) < 1e-4f; }),
                                bp, nf, vsel, err));
    CHECK(nf.size() == 16 && closedAndConsistent(seg));
    // Wrong selections explain themselves.
    Mesh one = primitives::cube(2);
    CHECK(!meshedit::bridgeFaces(one, {0}, bp, nf, vsel, err) && !err.empty());

    // Bridging open borders of different sizes (8 and 12 segment tubes).
    Mesh tubes = primitives::cylinder(1, 2, 8);
    meshedit::appendMesh(tubes, primitives::cylinder(1, 2, 12), translation({0, 4, 0}));
    std::vector<char> kill(tubes.faces.size(), 0);
    for (size_t f = 0; f < tubes.faces.size(); ++f) {
        Vec3 c = faceCenter(tubes, tubes.faces[f]);
        kill[f] = tubes.faces[f].size() > 4 && (near(c.y, 1.0f) || near(c.y, 3.0f));  // the facing caps
    }
    meshedit::deleteFaces(tubes, kill);
    CHECK(!closedAndConsistent(tubes));
    std::vector<meshedit::Edge> borders;
    {
        std::map<std::pair<int, int>, int> d;
        for (auto& f : tubes.faces)
            for (size_t i = 0; i < f.size(); ++i) d[{f[i], f[(i + 1) % f.size()]}]++;
        for (auto& kv : d)
            if (!d.count({kv.first.second, kv.first.first})) borders.push_back(meshedit::makeEdge(kv.first.first, kv.first.second));
    }
    CHECK(borders.size() == 20);
    bp = meshedit::BridgeParams();
    CHECK(meshedit::bridgeEdges(tubes, borders, bp, nf, vsel, err));
    CHECK(closedAndConsistent(tubes) && signedVolume(tubes) > 0);

    // Fill: remove a face, select the hole's border, fill it back.
    Mesh holed = primitives::cube(2);
    std::vector<char> killTop(holed.faces.size(), 0);
    std::vector<int> topF = facesWhere(holed, [](Vec3 v) { return v.y > 0.9f; });
    killTop[topF[0]] = 1;
    meshedit::deleteFaces(holed, killTop);
    std::vector<meshedit::Edge> hole;
    for (const auto& e : uniqueEdges(holed))
        if (holed.verts[e.first].y > 0.9f && holed.verts[e.second].y > 0.9f) hole.push_back(e);
    CHECK(hole.size() == 4);
    CHECK(meshedit::fillFace(holed, hole, {}, nf, vsel, err));
    CHECK(closedAndConsistent(holed) && near(signedVolume(holed), 8.0f, 1e-3f));
    // Fill from loose vertices: a face pointing away from the mesh.
    Mesh loose;
    loose.verts = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0.5f, 0.5f, -1}};
    loose.faces = {{0, 1, 4}, {1, 2, 4}, {2, 3, 4}, {3, 0, 4}};
    loose.touch();
    std::vector<char> base(5, 0);
    base[0] = base[1] = base[2] = base[3] = 1;
    CHECK(meshedit::fillFace(loose, {}, base, nf, vsel, err));
    CHECK(faceNormalRaw(loose, loose.faces.back()).z > 0);

    // Push in: a pocket in the top of a cube.
    Mesh pocket = primitives::cube(2);
    meshedit::PushParams pp;
    pp.width = 0.25f;
    pp.depth = 0.5f;
    std::vector<int> inner;
    CHECK(meshedit::pushIn(pocket, facesWhere(pocket, [](Vec3 v) { return v.y > 0.9f; }), pp, inner, vsel, err));
    CHECK(closedAndConsistent(pocket) && inner.size() == 1);
    CHECK(near(signedVolume(pocket), 8.0f - 1.5f * 1.5f * 0.5f, 1e-3f));
    // Pushing every face of a closed mesh: each face goes in along its own normal.
    Mesh allIn = primitives::cube(2);
    meshedit::PushParams pa;
    pa.width = 0.2f;
    pa.depth = 0.3f;
    std::vector<int> every = {0, 1, 2, 3, 4, 5};
    CHECK(meshedit::pushIn(allIn, every, pa, inner, vsel, err));
    CHECK(closedAndConsistent(allIn) && inner.size() == 6);
    CHECK(near(signedVolume(allIn), 8.0f - 6 * 1.6f * 1.6f * 0.3f, 1e-3f));
    pa.through = true;
    Mesh allPunch = primitives::cube(2);
    CHECK(!meshedit::pushIn(allPunch, every, pa, inner, vsel, err) && !err.empty());
    // Punch through: a square hole all the way through.
    Mesh punched = primitives::cube(2);
    pp.through = true;
    CHECK(meshedit::pushIn(punched, facesWhere(punched, [](Vec3 v) { return v.y > 0.9f; }), pp, inner, vsel, err));
    CHECK(indicesValid(punched) && near(signedVolume(punched), 8.0f - 1.5f * 1.5f * 2.0f, 2e-2f));
    bool hitsHole = false;
    float t;
    hitsHole = raycastMesh(punched, {0, 5, 0}, {0, -1, 0}, t);
    CHECK(!hitsHole);  // a ray straight down the hole passes through
    CHECK(raycastMesh(punched, {0.9f, 5, 0.9f}, {0, -1, 0}, t));  // the frame is still there
    CHECK(closedAndConsistent(punched));  // walls are stitched to both sides
    // Through a curved, denser surface: a sphere (the far side is not a single face).
    Mesh ball = primitives::uvSphere(1.0f, 32, 16);
    std::vector<int> cap = facesWhere(ball, [](Vec3 v) { return v.y > 0.8f; });
    CHECK(meshedit::pushIn(ball, cap, pp, inner, vsel, err));
    CHECK(indicesValid(ball) && closedAndConsistent(ball));
    CHECK(!raycastMesh(ball, {0, 5, 0}, {0, -1, 0}, t));
    CHECK(raycastMesh(ball, {0.9f, 0, 5}, {0, 0, -1}, t));
    CHECK(signedVolume(ball) > 0 && signedVolume(ball) < signedVolume(primitives::uvSphere(1.0f, 32, 16)));
    // Deleting faces keeps the other faces intact (regression: a self move-assignment emptied them).
    Mesh del = primitives::cube(2);
    std::vector<char> killOne(del.faces.size(), 0);
    killOne[3] = 1;
    meshedit::deleteFaces(del, killOne);
    bool allQuads = del.faces.size() == 5;
    for (const auto& f : del.faces) allQuads = allQuads && f.size() == 4;
    CHECK(allQuads);
}

static void testMergeAndJoin() {
    Mesh c = primitives::cube(2);
    std::vector<char> vsel = vertsWhere(c, [](Vec3 v) { return v.y > 0.9f; });
    CHECK(meshedit::mergeAtCenter(c, vsel) == 3);
    CHECK(c.verts.size() == 5 && c.faces.size() == 5 && closedAndConsistent(c) && uvsConsistent(c));
    CHECK(near(signedVolume(c), 8.0f / 3.0f, 1e-3f));  // a pyramid
    CHECK(std::count(vsel.begin(), vsel.end(), 1) == 1);

    // Joining with a mirrored transform keeps both parts facing outwards.
    Mesh j = primitives::cube(2);
    meshedit::appendMesh(j, primitives::cube(2), translation({5, 0, 0}) * scaling({-1, 1, 1}));
    CHECK(closedAndConsistent(j) && near(signedVolume(j), 16.0f, 1e-3f));
    // Weights only survive when both sides have them.
    Mesh wa = primitives::cube(2);
    wa.weights.resize(wa.verts.size());
    meshedit::appendMesh(wa, primitives::cube(2), Mat4());
    CHECK(!wa.hasWeights());
}
