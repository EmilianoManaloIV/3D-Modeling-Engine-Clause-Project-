// Performance benchmark (--benchmark report.md).
//
// Runs a fixed list of scenarios inside the real editor (real UI, real
// renderer, vsync off, glFinish each frame so GPU time is included), records
// the profiler sections for each, then times a set of one-shot heavy
// operations, and writes a Markdown report.
#include "editor_internal.h"
#include "csg.h"
#include "polygon.h"
#include "pathtracer.h"
#include "jobs.h"
#include "gl.h"
#include "profiler.h"
#include "skin.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdio>

using namespace ed;

namespace {
constexpr int kWarmupFrames = 15;
constexpr int kMeasureFrames = 90;

const char* const kScenarioNames[] = {
    "Default scene (baseline)",
    "1000 objects in hierarchies, all selected",
    "Dense mesh idle (262k tris), selected",
    "Dense mesh idle (262k tris), not selected",
    "Dense mesh, edit-mode vertex drag every frame",
    "Skinned mesh idle (49k tris, 2 bones)",
    "Skinned mesh, posing a bone every frame",
    "Particles: 10 emitters, ~50k live",
    "Lit view: dense mesh + 8 point lights",
    "Picking: 50 click-selects per frame, 1000 objects",
    "Gizmo drag: rotating 200 objects with the Rotate handle",
};
constexpr int kScenarioCount = (int)(sizeof kScenarioNames / sizeof kScenarioNames[0]);

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
}  // namespace

void Editor::clearForBenchmark() {
    if (xf_ != Xform::None) endTransform(false);
    scene_ = Scene();
    mode_ = Mode::Object;
    vsel_.clear();
    particles_.clear();
    undo_.clear();
    redo_.clear();
    cam_ = Camera();
    shading_ = SHADE_STUDIO;
    playing_ = true;
    wireframe_ = uvEditor_ = false;
}

void Editor::benchmarkSetup(int s) {
    auto denseSphere = [&](const char* name) {
        int i = scene_.add(catmullClark(primitives::uvSphere(1.5f, 256, 128)), name, kPalette[4]);
        selectOnly(i);
        return i;
    };
    auto manyObjects = [&](int n, bool selectAllOfThem) {
        for (int k = 0; k < n; ++k) {
            Mesh m = (k % 3 == 0) ? primitives::cube(0.8f)
                   : (k % 3 == 1) ? primitives::uvSphere(0.5f, 12, 8)
                                  : primitives::torus(0.4f, 0.15f, 12, 8);
            int i = scene_.add(std::move(m), "Obj", kPalette[1 + k % 9]);
            Object& o = scene_.objects[i];
            if (k % 5 != 0) {  // chains of 5: each object is a child of the previous one
                o.parent = scene_.objects[i - 1].id;
                o.position = {0, 0, 1.5f};
            } else {
                o.position = {(float)(k % 40) * 3 - 60, 0, (float)(k / 40) * 9 - 60};
            }
            o.selected = selectAllOfThem;
        }
        scene_.active = selectAllOfThem ? 0 : -1;
        cam_.distance = 110;
        cam_.pitch = 40;
    };
    switch (s) {
        case 0: buildDemo(0); break;
        case 1: manyObjects(1000, true); break;
        case 2: denseSphere("Dense"); cam_.distance = 6; break;
        case 3:
            denseSphere("Dense");
            selectOnly(-1);
            cam_.distance = 6;
            break;
        case 4: {
            int i = denseSphere("Dense");
            cam_.distance = 6;
            mode_ = Mode::Edit;
            vsel_.assign(scene_.objects[i].mesh.verts.size(), 1);
            break;
        }
        case 5:
        case 6:
            buildDemo(4);
            for (Object& o : scene_.objects)
                if (o.isMesh()) {  // rebuild the column much denser and rebind
                    unbindSkin(o);
                    o.mesh = primitives::box(0.7f, 4, 0.7f, 64);
                }
            {
                int col = 0, root = 1, tip = 2;
                scene_.objects[root].rotation = scene_.objects[tip].rotation = Vec3();
                std::string err;
                bindSkin(scene_, col, {root, tip}, err);
            }
            shading_ = SHADE_STUDIO;
            break;
        case 7:
            for (int k = 0; k < 10; ++k) {
                Object e;
                e.kind = ObjectKind::Emitter;
                e.name = "Emitter";
                e.position = {(float)(k % 5) * 3 - 6, 0, (float)(k / 5) * 3 - 1.5f};
                e.particles.rate = 2500;
                e.particles.lifetime = 2;
                scene_.addObject(e);
            }
            cam_.distance = 18;
            break;
        case 8: {
            denseSphere("Dense");
            for (int k = 0; k < 8; ++k) {
                Object l;
                l.kind = ObjectKind::Light;
                l.name = "Light";
                float a = k * kPi / 4;
                l.position = {3 * std::cos(a), 1.5f, 3 * std::sin(a)};
                l.light.color = {0.5f + 0.5f * std::cos(a), 0.6f, 0.5f + 0.5f * std::sin(a)};
                scene_.addObject(l);
            }
            shading_ = SHADE_LIT;
            cam_.distance = 7;
            break;
        }
        case 9: manyObjects(1000, false); break;
        case 10:
            manyObjects(200, true);
            cam_.distance = 60;
            break;
        default: break;
    }
    updateMatrices();
}

void Editor::benchmarkFrame(int s, int frame) {
    switch (s) {
        case 4: {  // what a vertex drag does each frame: move vertices, mark the mesh changed
            Object* o = activeMesh();
            if (!o) break;
            const float dy = 0.002f * std::sin(frame * 0.2f);
            for (Vec3& v : o->mesh.verts) v.y += dy;
            o->mesh.touchPositions();
            break;
        }
        case 6:
            for (Object& o : scene_.objects)
                if (o.kind == ObjectKind::Bone && o.parent) o.rotation.z = 50.0f * std::sin(frame * 0.1f);
            break;
        case 9: {
            PROF_SCOPE("picking (50 rays)");
            uint32_t r = 12345u + (uint32_t)frame * 7919u;
            for (int k = 0; k < 50; ++k) {
                r = r * 1664525u + 1013904223u;
                float x = (r >> 8) % 1000 / 1000.0f * viewport_.w;
                r = r * 1664525u + 1013904223u;
                float y = (r >> 8) % 1000 / 1000.0f * viewport_.h;
                clickSelect({x, y}, false);
            }
            break;
        }
        case 10: {  // a rotate-handle drag: one transform, updated every frame
            if (frame == 0) {
                tool_ = Tool::Rotate;
                beginTransform(Xform::Rotate, true);
            }
            if (xf_ != Xform::None) {
                XfDelta d;
                d.rotAxis = {0, 1, 0};
                d.rotDeg = 2.0f * frame;
                applyTransform(d);
            }
            break;
        }
        default: break;
    }
}

void Editor::benchmarkTick() {
    if (benchScenario_ < 0 || benchScenario_ >= kScenarioCount) return;
    if (benchFrame_ == 0) {
        clearForBenchmark();
        benchmarkSetup(benchScenario_);
    }
    if (benchFrame_ == kWarmupFrames) prof::resetTotals();
    if (benchFrame_ == kWarmupFrames + kMeasureFrames) {
        // Collect this scenario.
        const int frames = std::max(1, prof::framesSinceReset());
        std::vector<prof::Stat> st = prof::stats();
        double frameMs = 0;
        for (const auto& x : st)
            if (std::string(x.name) == "frame total") frameMs = x.totalMs / frames;
        std::sort(st.begin(), st.end(), [](const prof::Stat& a, const prof::Stat& b) { return a.totalMs > b.totalMs; });
        benchText_ += strf("\n### %d. %s\n\n**%.2f ms/frame (%.0f fps)** over %d frames\n\n", benchScenario_ + 1,
                           kScenarioNames[benchScenario_], frameMs, frameMs > 0 ? 1000.0 / frameMs : 0.0, frames);
        benchText_ += "| Section | ms/frame | share |\n|---|---:|---:|\n";
        int rows = 0;
        for (const auto& x : st) {
            if (std::string(x.name) == "frame total" || x.totalMs <= 0.0) continue;
            double ms = x.totalMs / frames;
            if (ms < 0.005 || rows >= 10) continue;
            benchText_ += strf("| %s | %.3f | %.0f%% |\n", x.name, ms, frameMs > 0 ? 100.0 * ms / frameMs : 0.0);
            ++rows;
        }
        std::string counters;
        for (const auto& c : prof::counters())
            if (c.total > 0) counters += strf("%s %.0f, ", c.name, c.total / frames);
        if (!counters.empty()) benchText_ += "\nPer frame: " + counters.substr(0, counters.size() - 2) + "\n";
        if (viewportTimer_.supported())
            benchText_ += strf("\nGPU time for the viewport (GL timer queries): %.3f ms/frame\n", viewportTimer_.avgMs());
        if (xf_ != Xform::None) endTransform(true);

        ++benchScenario_;
        benchFrame_ = 0;
        if (benchScenario_ >= kScenarioCount) {
            benchmarkOperations();
            std::string header = "# Modeler3D performance report\n\n";
            const GLubyte* renderer = gl::GetString(GL_RENDERER);
            header += strf("GPU: %s, window %dx%d, UI scale %d, vsync off, glFinish every frame.\n",
                           renderer ? (const char*)renderer : "?", screenW_, screenH_, fontScale_);
            header += strf("Each scenario: %d warm-up frames, then %d measured frames.\n\n## Scenarios\n",
                           kWarmupFrames, kMeasureFrames);
            std::string all = header + benchText_;
            if (FILE* f = std::fopen(benchReport_.c_str(), "wb")) {
                std::fwrite(all.data(), 1, all.size(), f);
                std::fclose(f);
            }
            std::fwrite(all.data(), 1, all.size(), stdout);
            exit_ = true;
        }
        return benchmarkTick();  // set up the next scenario this same frame
    }
    benchmarkFrame(benchScenario_, benchFrame_);
    ++benchFrame_;
}

void Editor::benchmarkOperations() {
    benchText_ += "\n## One-shot operations\n\n| Operation | ms |\n|---|---:|\n";
    auto timeIt = [&](const char* name, auto&& fn) {
        auto t0 = std::chrono::steady_clock::now();
        fn();
        benchText_ += strf("| %s | %.1f |\n", name, msSince(t0));
    };
    Mesh base = primitives::uvSphere(1.5f, 256, 128);
    Mesh dense;
    timeIt("Catmull-Clark (32k faces -> 131k quads)", [&] { dense = catmullClark(base); });
    std::vector<RenderVertex> rv;
    std::vector<uint32_t> ri;
    timeIt("buildRenderMesh, smooth (262k tris)", [&] { buildRenderMesh(dense, dense.verts, true, 40, -1, rv, ri); });
    timeIt("uniqueEdges (131k quads)", [&] { (void)uniqueEdges(dense); });
    {
        Mesh m = dense;
        timeIt("Smart UV unwrap (131k quads)", [&] { uv::unwrap(m, uv::Method::Smart); });
        timeIt("Pack UV islands (131k quads)", [&] { uv::pack(m); });
    }
    timeIt("Raycast 100 rays vs 262k tris (brute force)", [&] {
        float t;
        for (int k = 0; k < 100; ++k) raycastMesh(dense, {0.01f * k, 0.3f, 5}, {0, 0, -1}, t);
    });
    MeshAccel accel;
    timeIt("Picking BVH build (262k tris, once per edit)", [&] { accel.build(dense, dense.verts); });
    timeIt("Raycast 100 rays vs 262k tris (picking BVH)", [&] {
        float t;
        for (int k = 0; k < 100; ++k) accel.raycast({0.01f * k, 0.3f, 5}, {0, 0, -1}, t);
    });

    clearForBenchmark();
    scene_.add(dense, "Dense", kPalette[1]);
    timeIt("Undo snapshot (scene with 131k-quad mesh)", [&] { pushUndo(); });
    timeIt("Save .m3d (131k quads)", [&] {
        std::string err;
        saveScene(scene_, "bench_tmp.m3d", err);
    });
    timeIt("Load .m3d (131k quads)", [&] {
        Scene s;
        std::string err;
        loadScene(s, "bench_tmp.m3d", err);
    });
    timeIt("Export OBJ (131k quads)", [&] {
        std::string err;
        exportOBJ(scene_, "bench_tmp.obj", err);
    });
    timeIt("Import OBJ (131k quads)", [&] {
        Scene s;
        std::string err;
        importOBJ(s, "bench_tmp.obj", err);
    });
    std::remove("bench_tmp.m3d");
    std::remove("bench_tmp.obj");
    std::remove("bench_tmp.mtl");

    clearForBenchmark();
    int col = scene_.add(primitives::box(0.7f, 8, 0.7f, 64), "Column", kPalette[3]);
    std::vector<int> bones;
    for (int k = 0; k < 20; ++k) {
        Object b;
        b.kind = ObjectKind::Bone;
        b.boneLength = 0.4f;
        b.position = k ? Vec3(0, 0.4f, 0) : Vec3(0, -4, 0);
        if (k) b.parent = scene_.objects.back().id;
        bones.push_back(scene_.addObject(b));
    }
    timeIt("Bind + automatic weights (24k quads, 20 bones)", [&] {
        std::string err;
        bindSkin(scene_, col, bones, err);
    });
    std::vector<Vec3> scratch;
    Mat4 model;
    timeIt("CPU skinning x10 (24k quads, 20 bones)", [&] {
        for (int k = 0; k < 10; ++k) evaluateMesh(scene_, col, false, scratch, model);
    });
    {
        Mesh a = primitives::uvSphere(1.0f, 64, 32), b = primitives::cylinder(0.5f, 3.0f, 64);
        timeIt("Boolean difference (sphere 64x32 - cylinder 64) + clean-up", [&] {
            csg::Result r = csg::apply(a, b, csg::Op::Difference);
            poly::cleanup(r.mesh);
            poly::triangulateMesh(r.mesh, 4);
        });
    }

    // --- the same work on 1 thread and on every hardware thread ---
    const int hw = jobs::hardwareThreads();
    benchText_ += strf("\n## Multithreading (job system, 1 vs %d threads)\n\n"
                       "Best of 3 runs each.\n\n| Operation | 1 thread ms | %d threads ms | speed-up |\n|---|---:|---:|---:|\n", hw, hw);
    auto bestOf3 = [&](auto&& fn) {
        double best = 1e30;
        for (int k = 0; k < 3; ++k) {
            auto t0 = std::chrono::steady_clock::now();
            fn();
            best = std::min(best, msSince(t0));
        }
        return best;
    };
    auto compare = [&](const char* name, auto&& fn) {
        jobs::setThreadCount(1);
        double one = bestOf3(fn);
        jobs::setThreadCount(hw);
        double all = bestOf3(fn);
        benchText_ += strf("| %s | %.1f | %.1f | %.2fx |\n", name, one, all, all > 0 ? one / all : 0.0);
    };
    compare("CPU skinning x10 (24k quads, 20 bones)", [&] {
        for (int k = 0; k < 10; ++k) evaluateMesh(scene_, col, false, scratch, model);
    });
    compare("Automatic weights (24k quads, 20 bones)", [&] { autoWeights(scene_, col); });
    compare("buildRenderMesh, smooth (262k tris)", [&] { buildRenderMesh(dense, dense.verts, true, 40, -1, rv, ri); });
    clearForBenchmark();
    scene_.add(dense, "Dense", kPalette[1]);
    rt::Settings rs;
    rt::SceneData rtData;
    compare("Path-tracer scene build + BVH (262k tris)", [&] { rt::buildScene(scene_, rs, rtData); });
    const int bvhNodes = (int)rtData.bvh.nodes.size();

    // --- path tracing: CPU (1 / N threads) vs GPU on the showcase scene ---
    clearForBenchmark();
    buildDemo(1);
    updateMatrices();
    auto showcase = std::make_shared<rt::SceneData>();
    rt::buildScene(scene_, rs, *showcase);
    const int rw = 320, rh = 200, spp = 16;
    rt::View view =
        rt::makeView(cam_.eye(), cam_.target, cam_.fovY, cam_.ortho, cam_.orthoHalfHeight(), (float)rw / rh);
    auto cpuTrace = [&]() {
        rt::CpuRenderer r;
        r.start(showcase, view, rs, rw, rh, spp);
        while (r.running()) {
            r.update();
            std::this_thread::yield();
        }
    };
    benchText_ += strf("\n## Path tracing (%dx%d, %d samples/pixel, %d triangles, max %d bounces)\n\n"
                       "| Device | ms | Msamples/s |\n|---|---:|---:|\n",
                       rw, rh, spp, (int)showcase->triangleCount(), rs.maxBounces);
    const double pixelSamples = (double)rw * rh * spp;
    for (int threads : {1, hw}) {
        jobs::setThreadCount(threads);
        auto t0 = std::chrono::steady_clock::now();
        cpuTrace();
        double ms = msSince(t0);
        benchText_ += strf("| CPU, %d thread%s | %.0f | %.2f |\n", threads, threads > 1 ? "s" : "", ms,
                           pixelSamples / (ms * 1e3));
    }
    if (gpuTracerOk_) {
        // Warm up (the driver finishes compiling the shader on first use, and
        // the adaptive batch size starts small until the GPU timer has
        // measured the scene), then time a clean upload + run.
        gpuTracer_.start(*showcase, view, rs, rw, rh, spp);
        for (int k = 0; k < 6 && gpuTracer_.running(); ++k) {
            gpuTracer_.step(30.0);
            gl::Finish();
        }
        gl::Finish();
        auto tu = std::chrono::steady_clock::now();
        gpuTracer_.start(*showcase, view, rs, rw, rh, spp);
        gl::Finish();
        const double uploadMs = msSince(tu);
        auto t0 = std::chrono::steady_clock::now();
        while (gpuTracer_.running()) {
            gpuTracer_.step(30.0);
            gl::Finish();
        }
        double ms = msSince(t0);
        benchText_ += strf("| GPU (%s) | %.0f | %.2f |\n", glRenderer_.c_str(), ms, pixelSamples / (ms * 1e3));
        benchText_ += strf("\nGPU data uploaded for tracing: %.2f MB (triangles, BVH, materials as RGBA32F textures) "
                           "in %.1f ms. GPU time per frame while tracing (timer queries): %.1f ms of a %.0f ms budget.\n",
                           gpuTracer_.uploadedBytes() / 1048576.0, uploadMs, gpuTracer_.gpuMsPerFrame(), 30.0);
        gpuTracer_.stop();
    }
    benchText_ += strf("BVH of the 262k-triangle mesh: %d nodes.\n", bvhNodes);
    clearForBenchmark();
}
