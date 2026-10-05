// Round 6: typed expressions, edge extrude, inset, picking BVH, ... Included from tests.cpp.

static void testExpressions() {
    double v = 0;
    CHECK(expr::evaluate("2*3", 0, v) && v == 6.0);
    CHECK(expr::evaluate(" (1+2)/4 ", 0, v) && v == 0.75);
    CHECK(expr::evaluate("-0.5", 0, v) && v == -0.5);
    CHECK(expr::evaluate("2^3^2", 0, v) && v == 512.0);  // right associative
    CHECK(expr::evaluate("-2^2", 0, v) && v == -4.0);  // like Python / math: -(2^2)
    CHECK(expr::evaluate("2^-1", 0, v) && v == 0.5);
    CHECK(expr::evaluate("pi/2", 0, v) && std::fabs(v - 1.5707963) < 1e-6);
    CHECK(expr::evaluate("1e-3*2", 0, v) && std::fabs(v - 0.002) < 1e-12);
    CHECK(expr::evaluate("+=0.5", 2, v) && v == 2.5);
    CHECK(expr::evaluate("-=1", 2, v) && v == 1.0);
    CHECK(expr::evaluate("*=3", 2, v) && v == 6.0);
    CHECK(expr::evaluate("/=4", 2, v) && v == 0.5);
    v = 42;
    CHECK(!expr::evaluate("", 0, v) && !expr::evaluate("2*", 0, v) && !expr::evaluate("1/0", 0, v));
    CHECK(!expr::evaluate("(1+2", 0, v) && !expr::evaluate("abc", 0, v) && v == 42);
}

static void testEdgeExtrudeAndInset() {
    // Extrude the top edge of a single quad: one new quad, same orientation.
    Mesh q;
    q.verts = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    q.faces = {{0, 1, 2, 3}};
    q.uvs = {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    q.touch();
    std::vector<meshedit::Edge> newEdges;
    std::vector<char> vsel;
    CHECK(meshedit::extrudeEdges(q, {meshedit::makeEdge(2, 3)}, newEdges, vsel));
    CHECK(q.faces.size() == 2 && q.verts.size() == 6 && newEdges.size() == 1 && q.hasUVs());
    CHECK(dot(faceNormalRaw(q, q.faces[1]), Vec3(0, 0, 1)) >= 0.0f);  // degenerate until moved, but not flipped
    for (int v : {newEdges[0].first, newEdges[0].second}) q.verts[v] += Vec3(0, 1, 0);
    CHECK(faceNormalRaw(q, q.faces[1]).z > 0.5f);  // continues the surface with the same normal
    CHECK(vsel[newEdges[0].first] && vsel[newEdges[0].second] && !vsel[0]);
    // The shared edge is used once in each direction: the strip stays manifold.
    {
        std::map<std::pair<int, int>, int> d;
        for (auto& f : q.faces)
            for (size_t i = 0; i < f.size(); ++i) d[{f[i], f[(i + 1) % f.size()]}]++;
        CHECK((d[{2, 3}] == 1 && d[{3, 2}] == 1));
    }
    // Two connected edges share the copied corner vertex.
    Mesh g = primitives::grid(2, 2, 2, 2);
    std::vector<meshedit::Edge> border;
    for (const auto& e : uniqueEdges(g)) {
        Vec3 a = g.verts[e.first], b = g.verts[e.second];
        if (std::fabs(a.z - 1) < 1e-4f && std::fabs(b.z - 1) < 1e-4f) border.push_back(e);
    }
    size_t vBefore = g.verts.size(), fBefore = g.faces.size();
    CHECK(border.size() == 2 && meshedit::extrudeEdges(g, border, newEdges, vsel));
    CHECK(g.verts.size() == vBefore + 3 && g.faces.size() == fBefore + 2);

    // Inset of a cube's top face: 4 ring faces, closed, volume kept.
    Mesh c = primitives::cube(2.0f);
    int top = -1;
    for (int f = 0; f < 6; ++f)
        if (faceNormalRaw(c, c.faces[f]).y > 0.5f) top = f;
    meshedit::InsetParams p;
    p.thickness = 0.25f;
    std::vector<int> inner;
    CHECK(meshedit::insetFaces(c, {top}, p, inner, vsel));
    CHECK(c.faces.size() == 10 && c.verts.size() == 12 && inner.size() == 1 && inner[0] == top);
    CHECK(closedAndConsistent(c) && near(signedVolume(c), 8.0f, 1e-4f) && c.hasUVs());
    // The inner face is a 1.5 x 1.5 square (constant ring width 0.25).
    CHECK(near(length(faceNormalRaw(c, c.faces[top])) * 0.5f, 1.5f * 1.5f, 1e-4f));
    // Depth: the ring becomes a frustum between the 2x2 border and the 1.5x1.5
    // inner face: volume h/3 * (A1 + A2 + sqrt(A1 A2)) = h * 9.25 / 3.
    for (float depth : {-0.5f, 0.5f}) {
        Mesh d = primitives::cube(2.0f);
        p.depth = depth;
        CHECK(meshedit::insetFaces(d, {top}, p, inner, vsel));
        CHECK(closedAndConsistent(d) && near(signedVolume(d), 8.0f + depth * 9.25f / 3.0f, 1e-3f));
    }
    // Dish: a concave centre (fan of triangles) lowers the volume by the pyramid.
    {
        Mesh d = primitives::cube(2.0f);
        p.depth = 0;
        p.dish = -0.3f;
        CHECK(meshedit::insetFaces(d, {top}, p, inner, vsel));
        CHECK(inner.size() == 4 && closedAndConsistent(d));
        CHECK(near(signedVolume(d), 8.0f - 2.25f * 0.3f / 3.0f, 1e-3f));
        p.dish = 0;
    }
    // Region vs individual on two adjacent top faces of a split box.
    for (bool individual : {false, true}) {
        Mesh b = primitives::box(2, 2, 2, 2);
        std::vector<int> tops;
        for (int f = 0; f < (int)b.faces.size(); ++f)
            if (faceNormalRaw(b, b.faces[f]).y > 0.5f && faceCenter(b, b.faces[f]).x > -1e-4f) tops.push_back(f);
        CHECK(tops.size() == 2);
        size_t before = b.faces.size();
        p.individual = individual;
        p.thickness = 0.1f;
        CHECK(meshedit::insetFaces(b, tops, p, inner, vsel));
        CHECK(closedAndConsistent(b) && near(signedVolume(b), 8.0f, 1e-3f));
        CHECK(b.faces.size() == before + (individual ? 8 : 6));  // region: 6 border edges; individual: 4 + 4
    }
    // Selection helpers.
    Mesh cube = primitives::cube(2.0f);
    std::vector<char> fs(6, 0);
    fs[0] = 1;
    auto vs = meshedit::vertsFromFaces(cube, fs);
    CHECK(std::count(vs.begin(), vs.end(), 1) == 4);
    CHECK(meshedit::edgesFromVerts(cube, vs).size() == 4);
    auto back = meshedit::facesFromVerts(cube, vs);
    CHECK(std::count(back.begin(), back.end(), 1) == 1 && back[0] == 1);
}

static void testMeshAccel() {
    Mesh s = primitives::uvSphere(1.0f, 48, 24);
    MeshAccel a;
    a.build(s, s.verts);
    Rng01 rng(3);
    int agree = 0;
    for (int i = 0; i < 300; ++i) {
        Vec3 o(rng.next() * 6 - 3, rng.next() * 6 - 3, 4);
        Vec3 d = normalize(Vec3(rng.next() - 0.5f, rng.next() - 0.5f, -1));
        float t1 = 0, t2 = 0;
        int face = -1;
        bool h1 = raycastMesh(s, o, d, t1), h2 = a.raycast(o, d, t2, &face);
        if (h1 == h2 && (!h1 || (near(t1, t2, 1e-4f) && face >= 0))) ++agree;
    }
    CHECK(agree == 300);
}

static void testHierarchyDragDrop() {
    Scene sc;
    int a = sc.add(primitives::cube(), "A", {1, 1, 1});
    int b = sc.add(primitives::cube(), "B", {1, 1, 1});
    int c = sc.add(primitives::cube(), "C", {1, 1, 1});
    sc.objects[b].position = {3, 1, 0};
    const uint32_t A = sc.objects[a].id, B = sc.objects[b].id, C = sc.objects[c].id;
    Vec3 bWorld = transformPoint(sc.world(b), Vec3());
    // Drop B onto A: B becomes A's child and stays where it is in the world.
    sc.objects[a].position = {1, 0, 0};
    sc.objects[a].rotation = {0, 45, 0};
    bWorld = transformPoint(sc.world(sc.indexOf(B)), Vec3());
    CHECK(moveInHierarchy(sc, {B}, A, HierarchyDrop::Onto));
    CHECK(sc.parentIndex(sc.indexOf(B)) == sc.indexOf(A));
    CHECK(near(transformPoint(sc.world(sc.indexOf(B)), Vec3()), bWorld, 1e-4f));
    // A onto B (its own child) is refused and changes nothing.
    CHECK(!moveInHierarchy(sc, {A}, B, HierarchyDrop::Onto) && sc.parentIndex(sc.indexOf(A)) < 0);
    CHECK(!moveInHierarchy(sc, {A}, A, HierarchyDrop::Onto));
    // Drop C before A: C stays a root and comes first in the list.
    CHECK(moveInHierarchy(sc, {C}, A, HierarchyDrop::Before));
    CHECK(sc.objects[0].id == C && sc.parentIndex(0) < 0);
    // Drop C after B (B is A's child): C becomes A's child too, right after B.
    CHECK(moveInHierarchy(sc, {C}, B, HierarchyDrop::After));
    CHECK(sc.parentIndex(sc.indexOf(C)) == sc.indexOf(A) && sc.indexOf(C) == sc.indexOf(B) + 1);
    // Dragging A together with its child B moves only A (B follows); Root unparents.
    CHECK(moveInHierarchy(sc, {A, B}, 0, HierarchyDrop::Root));
    CHECK(sc.parentIndex(sc.indexOf(B)) == sc.indexOf(A) && sc.objects.back().id == A);
    CHECK(moveInHierarchy(sc, {C}, 0, HierarchyDrop::Root) && sc.parentIndex(sc.indexOf(C)) < 0);
}

static bool readExpect(const std::string& path, Image& img) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint32_t hdr[4];
    bool ok = std::fread(hdr, 4, 4, f) == 4;
    if (ok) {
        img.width = (int)hdr[0];
        img.height = (int)hdr[1];
        img.rgba.resize((size_t)img.width * img.height * 4);
        ok = std::fread(img.rgba.data(), 1, img.rgba.size(), f) == img.rgba.size();
    }
    std::fclose(f);
    return ok;
}

static void testImageDecoding() {
    const std::string dir = MODELER_TEST_DATA;
    // Lossless formats must match exactly.
    for (const char* name : {"rgba8", "rgba8_stored", "rgb8_interlaced", "rgba16", "gray8", "gray_alpha8", "gray1",
                             "gray2", "gray4", "palette2"}) {
        Image img, want;
        std::string err;
        bool ok = loadImage(dir + "/" + name + ".png", img, err) && readExpect(dir + "/" + name + ".rgba", want);
        if (!ok) std::printf("  %s: %s\n", name, err.c_str());
        CHECK(ok && img.width == want.width && img.height == want.height && img.rgba == want.rgba);
    }
    // JPEG is lossy (and our test encoder averages chroma that the decoder
    // upsamples by replication): small average error, bounded peaks.
    for (const char* name : {"jpeg444", "jpeg420", "jpeg_gray"}) {
        Image img, want;
        std::string err;
        bool ok = loadImage(dir + "/" + name + ".jpg", img, err) && readExpect(dir + "/" + name + ".rgba", want);
        if (!ok) std::printf("  %s: %s\n", name, err.c_str());
        CHECK(ok && img.width == want.width && img.height == want.height);
        if (!ok || img.rgba.size() != want.rgba.size()) continue;
        double sum = 0;
        int peak = 0;
        for (size_t i = 0; i < img.rgba.size(); ++i) {
            int d = std::abs((int)img.rgba[i] - (int)want.rgba[i]);
            sum += d;
            peak = std::max(peak, d);
        }
        double mean = sum / img.rgba.size();
        CHECK(mean < 4.0 && peak < 60);
    }
    // Round trip through our own PNG writer (fixed-Huffman deflate).
    {
        std::vector<uint8_t> rgb(23 * 17 * 3);
        for (size_t i = 0; i < rgb.size(); ++i) rgb[i] = (uint8_t)(i * 7 + i / 13);
        const std::string path = "test_roundtrip.png";
        CHECK(writePNG(path, 23, 17, rgb));
        Image img;
        std::string err;
        CHECK(loadImage(path, img, err) && img.width == 23 && img.height == 17);
        bool same = img.rgba.size() == (size_t)23 * 17 * 4;
        for (int y = 0; same && y < 17; ++y)  // the writer takes rows bottom-up
            for (int x = 0; x < 23; ++x)
                for (int c = 0; c < 3; ++c)
                    same = same && img.rgba[((size_t)y * 23 + x) * 4 + c] == rgb[((size_t)(16 - y) * 23 + x) * 3 + c];
        CHECK(same);
        std::remove(path.c_str());
    }
    // TGA (RLE) and BMP built in memory.
    {
        std::vector<uint8_t> tga = {0, 0, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 1, 0, 32, 0x20};
        tga.insert(tga.end(), {0x82, 10, 20, 30, 40});  // 3 x the same BGRA pixel
        Image img;
        std::string err;
        CHECK(decodeImage(tga.data(), tga.size(), img, err) && img.width == 3 && img.height == 1);
        CHECK(img.rgba.size() == 12 && img.rgba[0] == 30 && img.rgba[1] == 20 && img.rgba[2] == 10 && img.rgba[3] == 40 &&
              img.rgba[8] == 30);
        std::vector<uint8_t> bmp(54 + 8, 0);  // 2x1, 24-bit, rows padded to 8 bytes
        bmp[0] = 'B';
        bmp[1] = 'M';
        bmp[10] = 54;
        bmp[14] = 40;
        bmp[18] = 2;
        bmp[22] = 1;
        bmp[26] = 1;
        bmp[28] = 24;
        bmp[54] = 1, bmp[55] = 2, bmp[56] = 3, bmp[57] = 4, bmp[58] = 5, bmp[59] = 6;
        CHECK(decodeImage(bmp.data(), bmp.size(), img, err) && img.width == 2 && img.height == 1);
        CHECK(img.rgba[0] == 3 && img.rgba[1] == 2 && img.rgba[2] == 1 && img.rgba[4] == 6);
    }
    // Garbage is rejected, not crashed on.
    {
        std::vector<uint8_t> junk(100, 0xAB);
        Image img;
        std::string err;
        CHECK(!decodeImage(junk.data(), junk.size(), img, err));
        std::vector<uint8_t> png = {137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
        CHECK(!decodeImage(png.data(), png.size(), img, err));
        uint8_t z[] = {0x78, 0x9C, 0xFF, 0xFF};
        std::vector<uint8_t> out;
        CHECK(!zlibInflate(z, sizeof z, out));
    }
}

// The path tracers agree: the DirectX ray-tracing renderer (on an RTX-class
// GPU, or Microsoft's WARP software adapter) against the CPU tracer.
static void testHardwareRayTracingMatchesCpu() {
    HwRtInfo info = HwRayTracer::probe(true);
    if (!info.available) {
        std::printf("  (skipped DXR comparison: %s)\n", info.reason.c_str());
        return;
    }
    Scene sc;
    int floor = sc.add(primitives::plane(10.0f), "Floor", {0.7f, 0.7f, 0.7f});
    sc.objects[floor].roughness = 0.8f;
    int ball = sc.add(primitives::uvSphere(1.0f, 24, 12), "Ball", {0.9f, 0.3f, 0.2f});
    sc.objects[ball].position = {0, 1, 0};
    sc.objects[ball].metallic = 0.5f;
    sc.objects[ball].roughness = 0.3f;
    // See-through materials exercise the second (non-opaque) BLAS geometry.
    int glass = sc.add(primitives::uvSphere(0.6f, 24, 12), "Glass", {0.9f, 0.95f, 1.0f});
    sc.objects[glass].position = {-1.6f, 0.6f, 0.8f};
    sc.objects[glass].transmission = 1.0f;
    sc.objects[glass].roughness = 0.05f;
    int veil = sc.add(primitives::box(1.0f, 1.2f, 0.1f, 1), "Veil", {0.2f, 0.4f, 1.0f});
    sc.objects[veil].position = {1.5f, 0.6f, 1.0f};
    sc.objects[veil].opacity = 0.5f;
    Object sun;
    sun.kind = ObjectKind::Light;
    sun.light.type = LightType::Sun;
    sun.light.intensity = 2.0f;
    sun.rotation = {-40, 30, 0};
    sc.addObject(sun);
    sc.ambient = {0.2f, 0.22f, 0.25f};
    rt::Settings s;
    s.maxBounces = 2;
    auto data = std::make_shared<rt::SceneData>();
    rt::buildScene(sc, s, *data);
    const int W = 32, H = 20, spp = 128;
    rt::View view = rt::makeView({0, 2.5f, 6}, {0, 0.7f, 0}, 45.0f, false, 1.0f, (float)W / H);
    rt::CpuRenderer cpu;
    cpu.start(data, view, s, W, H, spp);
    while (cpu.running()) {
        cpu.update();
        std::this_thread::yield();
    }
    HwRayTracer hw;
    std::string err;
    bool ok = hw.init(true, err) && hw.start(*data, view, s, W, H, spp, err);
    if (!ok) std::printf("  DXR: %s\n", err.c_str());
    CHECK(ok);
    if (!ok) return;
    hw.finish();
    std::vector<uint8_t> img;
    CHECK(hw.takeImage(img) && img.size() == (size_t)W * H * 4 && hw.samples() == spp);
    if (img.size() != (size_t)W * H * 4) return;
    double diff = 0, meanCpu = 0, meanHw = 0;
    for (size_t i = 0; i < img.size(); ++i) {
        if (i % 4 == 3) continue;
        diff += std::abs((int)img[i] - (int)cpu.image()[i]);
        meanCpu += cpu.image()[i];
        meanHw += img[i];
    }
    const double n = W * H * 3;
    std::printf("  DXR on %s: mean %.1f vs CPU %.1f, mean abs diff %.2f / 255\n", info.adapter.c_str(), meanHw / n,
                meanCpu / n, diff / n);
    CHECK(std::fabs(meanHw - meanCpu) / n < 2.0);  // same average brightness
    CHECK(diff / n < 8.0);                        // per pixel: only Monte Carlo noise differs
}

static void testLightsCameraMaterials() {
    // Colour temperature: warm below ~6500 K, cool above, max channel 1.
    Vec3 warm = kelvinToRGB(2700), d65 = kelvinToRGB(6500), cool = kelvinToRGB(12000);
    CHECK(near(std::max(warm.x, std::max(warm.y, warm.z)), 1.0f) && warm.x > warm.y && warm.y > warm.z);
    CHECK(d65.x > 0.9f && d65.y > 0.85f && d65.z > 0.85f);
    CHECK(cool.z >= cool.x);
    LightSettings L;
    L.color = {1, 1, 1};
    L.useTemperature = true;
    L.temperature = 2700;
    CHECK(near(L.finalColor(), warm));
    L.useTemperature = false;
    CHECK(near(L.finalColor(), Vec3(1, 1, 1)));

    // Physical camera: exposure relative to f/8, 1/125 s, ISO 100.
    PhysicalCamera c;
    CHECK(near(c.exposure(), 1.0f, 1e-5f));
    c.fStop = 4.0f;  // two stops more light
    CHECK(near(c.exposure(), 4.0f, 1e-4f));
    c.shutter = 1.0f / 500.0f;  // two stops less
    CHECK(near(c.exposure(), 1.0f, 1e-4f));
    c.iso = 400;
    c.exposureComp = -1;
    CHECK(near(c.exposure(), 2.0f, 1e-4f));
    // 50 mm on 36 mm full frame: 39.6 deg horizontal; vertical at 3:2 = 27.0 deg.
    c = PhysicalCamera();
    CHECK(near(c.verticalFovDeg(1.5f), 26.99f, 0.05f));
    CHECK(near(c.apertureRadius(), 0.05f / 8.0f * 0.5f, 1e-6f));

    // Depth of field: rays through different lens points meet on the focal plane.
    Mat4 camWorld = translation({0, 0, 5});
    c.focusDistance = 5.0f;
    c.fStop = 1.4f;
    rt::View v = rt::cameraView(camWorld, c, 1.5f);
    CHECK(v.lensRadius > 0.01f && near(v.forward, Vec3(0, 0, -1)));
    Vec3 o1, d1, o2, d2;
    rt::primaryRay(v, 0.3f, 0.6f, 0.1f, 0.2f, o1, d1);
    rt::primaryRay(v, 0.3f, 0.6f, 0.9f, 0.7f, o2, d2);
    CHECK(length(o1 - o2) > 1e-3f);  // different lens points
    float t1 = (0.0f - o1.z) / d1.z, t2 = (0.0f - o2.z) / d2.z;  // the plane z = 0 is 5 m away
    CHECK(near(o1 + d1 * t1, o2 + d2 * t2, 1e-4f));
    // Polygonal aperture samples stay inside the unit disc.
    bool inside = true;
    rt::Rng rng(5);
    for (int i = 0; i < 1000; ++i) inside = inside && length(rt::sampleAperture(6, rng.uniform(), rng.uniform())) <= 1.0001f;
    CHECK(inside);

    // Area light: lights a floor below it; brighter in the middle than at the edge.
    Scene sc;
    int floor = sc.add(primitives::plane(20.0f), "Floor", {1, 1, 1});
    sc.objects[floor].roughness = 1.0f;
    Object area;
    area.kind = ObjectKind::Light;
    area.light.type = LightType::Area;
    area.light.width = area.light.height = 2.0f;
    area.light.intensity = 10.0f;
    area.light.color = {1, 1, 1};
    area.light.range = 100.0f;
    area.position = {0, 2, 0};
    sc.addObject(area);
    sc.ambient = {0, 0, 0};
    rt::Settings s;
    s.maxBounces = 0;
    rt::SceneData data;
    rt::buildScene(sc, s, data);
    CHECK(data.lights.size() == 1 && data.lights[0].type == 3);
    auto avg = [&](Vec3 o, Vec3 d) {
        Vec3 sum;
        for (int i = 0; i < 2000; ++i) sum += rt::trace(data, s, o, d, rng);
        return sum.x / 2000;
    };
    float centre = avg({0, 0.5f, 0}, {0, -1, 0}), edge = avg({5, 0.5f, 0}, {0, -1, 0});
    CHECK(centre > 0.5f && centre > 3.0f * edge);
    // Visible to camera rays from below; one-sided, so not from above.
    CHECK(rt::trace(data, s, {0, 0.5f, 0}, {0, 1, 0}, rng).x > 1.0f);
    CHECK(rt::trace(data, s, {0, 5, 0}, {0, -1, 0}, rng).x < 2.0f);

    // Glass: a clear pane lets most light through (shadow rays and paths).
    Scene g;
    int fl = g.add(primitives::plane(20.0f), "Floor", {1, 1, 1});
    g.objects[fl].roughness = 1.0f;
    int pane = g.add(primitives::box(4, 0.05f, 4, 1), "Pane", {1, 1, 1});
    g.objects[pane].position = {0, 1, 0};
    g.objects[pane].transmission = 1.0f;
    g.objects[pane].roughness = 0.0f;
    Object sun;
    sun.kind = ObjectKind::Light;
    sun.light.type = LightType::Sun;
    sun.light.intensity = 1.0f;
    sun.light.color = {1, 1, 1};
    g.addObject(sun);
    g.ambient = {0, 0, 0};
    rt::Settings gs;
    gs.maxBounces = 0;
    gs.lightSize = 0;
    rt::SceneData gd;
    rt::buildScene(g, gs, gd);
    float lit = rt::trace(gd, gs, {3, 0.5f, 3}, {0, -1, 0}, rng).x;    // outside the shadow of the pane
    float under = rt::trace(gd, gs, {0, 0.5f, 0}, {0, -1, 0}, rng).x;  // below the glass
    CHECK(under > 0.8f * lit && under <= lit + 1e-4f);
    g.objects[pane].transmission = 0.0f;  // now opaque: full shadow
    rt::buildScene(g, gs, gd);
    CHECK(rt::trace(gd, gs, {0, 0.5f, 0}, {0, -1, 0}, rng).x < 1e-4f);

    // Texture packing for the GPU tracers.
    Scene t;
    int q = t.add(primitives::plane(2.0f), "Q", {1, 1, 1});
    t.objects[q].textures[TEX_BASE] = "builtin:bricks_color";
    t.objects[q].textures[TEX_NORMAL] = "builtin:bricks_normal";
    rt::SceneData td;
    rt::buildScene(t, gs, td);
    CHECK(td.textures.size() == 2 && td.materials[0].tex[TEX_BASE] == 0 && td.materials[0].tex[TEX_NORMAL] == 1);
    rt::PackedScene packed;
    rt::packScene(td, 256, packed);
    CHECK(packed.texSize == 256 && packed.texLayers == 2 && packed.texels.size() == 256u * 256 * 4 * 2);
    CHECK(packed.tris.size() == td.triangleCount() * rt::kTriTexels * 4);
    CHECK(packed.mats.size() == td.materials.size() * rt::kMatTexels * 4 && packed.mats[14] == 0.0f &&
          packed.mats[15] == 1.0f);  // layer indices of base / normal
    // Built-in maps: a bricks normal map points mostly up (+Z) and is normalised.
    auto nm = textureCache().get("builtin:bricks_normal");
    CHECK(nm && nm->width == 512);
    if (nm) {
        Vec4 c4 = rt::sampleTexture(*nm, Vec2(0.37f, 0.61f));
        Vec3 n(c4.x * 2 - 1, c4.y * 2 - 1, c4.z * 2 - 1);
        CHECK(n.z > 0.3f && near(length(n), 1.0f, 0.05f));
    }
    CHECK(!textureCache().get("builtin:nonsense_color") && !textureCache().error("builtin:nonsense_color").empty());
}

static void writeText(const char* path, const char* text) {
    FILE* f = std::fopen(path, "wb");
    std::fputs(text, f);
    std::fclose(f);
}

static void testSceneFilesV3() {
    Scene s;
    int m = s.add(primitives::cube(), "Mat", {0.2f, 0.4f, 0.6f});
    Object& o = s.objects[m];
    o.roughness = 0.33f;
    o.metallic = 0.75f;
    o.opacity = 0.5f;
    o.transmission = 0.25f;
    o.ior = 1.6f;
    o.normalStrength = 1.5f;
    o.uvScale = {2, 3};
    o.textures[TEX_BASE] = "builtin:tiles_color";
    o.textures[TEX_ROUGHNESS] = "maps/rough.png";  // relative: resolved against the scene folder
    Object light;
    light.kind = ObjectKind::Light;
    light.light.type = LightType::Area;
    light.light.width = 3;
    light.light.height = 1.5f;
    light.light.useTemperature = true;
    light.light.temperature = 3200;
    s.addObject(light);
    Object cam;
    cam.kind = ObjectKind::Camera;
    cam.camera.focalLength = 85;
    cam.camera.fStop = 1.8f;
    cam.camera.iso = 800;
    cam.camera.shutter = 1.0f / 60;
    cam.camera.blades = 7;
    cam.camera.depthOfField = false;
    s.addObject(cam);
    std::string err;
    CHECK(saveScene(s, "test_v3.m3d", err));
    Scene l;
    CHECK(loadScene(l, "test_v3.m3d", err));
    CHECK(l.objects.size() == 3);
    if (l.objects.size() == 3) {
        const Object& a = l.objects[0];
        CHECK(near(a.roughness, 0.33f) && near(a.metallic, 0.75f) && near(a.opacity, 0.5f) &&
              near(a.transmission, 0.25f) && near(a.ior, 1.6f) && near(a.normalStrength, 1.5f) &&
              near(a.uvScale.x, 2) && near(a.uvScale.y, 3));
        CHECK(a.textures[TEX_BASE] == "builtin:tiles_color" && a.textures[TEX_ROUGHNESS] == "maps/rough.png");
        const LightSettings& LL = l.objects[1].light;
        CHECK(LL.type == LightType::Area && near(LL.width, 3) && near(LL.height, 1.5f) && LL.useTemperature &&
              near(LL.temperature, 3200));
        const Object& cc = l.objects[2];
        CHECK(cc.kind == ObjectKind::Camera && near(cc.camera.focalLength, 85) && near(cc.camera.fStop, 1.8f) &&
              near(cc.camera.iso, 800) && near(cc.camera.shutter, 1.0f / 60) && cc.camera.blades == 7 &&
              !cc.camera.depthOfField);
    }
    std::remove("test_v3.m3d");
    // Older files: "gloss" becomes roughness = 1 - gloss.
    writeText("test_old.m3d", "modeler3d 2\nobject Old\nkind mesh\ngloss 0.8\nlight 0 1 1 1 5 10 40 0.25\nend\n");
    Scene old;
    CHECK(loadScene(old, "test_old.m3d", err) && old.objects.size() == 1 && near(old.objects[0].roughness, 0.2f));
    std::remove("test_old.m3d");
    // OBJ + MTL with PBR values and texture maps.
    writeText("test_mat.mtl",
              "newmtl shiny\nKd 0.5 0.25 1\nPr 0.2\nPm 1\nd 0.8\nNi 1.33\nmap_Kd tex/base.png\n"
              "map_Bump -bm 1.0 tex/nrm.png\nnewmtl glow\nKe 2 1 0\n");
    writeText("test_mat.obj",
              "mtllib test_mat.mtl\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nusemtl shiny\nf 1 2 3\nusemtl glow\nf 1 3 4\n");
    Scene imp;
    CHECK(importOBJ(imp, "test_mat.obj", err));
    CHECK(imp.objects.size() == 2);
    if (imp.objects.size() == 2) {
        const Object& a = imp.objects[0];
        CHECK(near(a.color, Vec3(0.5f, 0.25f, 1)) && near(a.roughness, 0.2f) && near(a.metallic, 1) &&
              near(a.opacity, 0.8f) && near(a.ior, 1.33f));
        CHECK(a.textures[TEX_BASE] == "tex/base.png" && a.textures[TEX_NORMAL] == "tex/nrm.png");
        const Object& b = imp.objects[1];
        CHECK(near(b.emissionStrength, 2.0f) && near(b.emission, Vec3(1, 0.5f, 0)));
    }
    std::remove("test_mat.obj");
    std::remove("test_mat.mtl");
}
