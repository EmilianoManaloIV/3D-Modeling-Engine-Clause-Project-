// Stress test (--stress report.md): pushes the editor to its limits and
// reports where it stops being interactive, and whether anything breaks.
//
//   A. Mesh size ladder      ~4k .. ~4M triangles: frame time idle / while
//                            dragging every vertex, picking BVH, click,
//                            undo snapshot, bevel, loop cut, save / load,
//                            path-tracer BVH, memory
//   B. Object count ladder   1k .. 50k objects: idle, all selected, rotating
//                            all of them, duplicate / undo / delete
//   C. Deep hierarchies      parent chains 1k .. 50k deep: frame time,
//                            cycle refusal, descendants, delete the root
//   D. Tool fuzzing          thousands of random modeling operations with
//                            random selections and settings on many kinds of
//                            mesh (closed, open, degenerate); every result is
//                            checked (indices, attribute arrays, finite
//                            positions, and that closed meshes stay closed)
//   E. Input fuzzing         thousands of frames of random mouse / keyboard /
//                            wheel input plus random commands from the
//                            command registry, scene invariants checked after
//                            every frame; then an undo / redo round trip
//   F. Layout extremes       window sizes 320x240 .. 3840x2160 at UI sizes 1 .. 6
//   G. Extreme values        huge / tiny transforms, camera limits, a
//                            20000-corner concave polygon
#include "editor_internal.h"
#include "csg.h"
#include "gl.h"
#include "jobs.h"
#include "pathtracer.h"
#include "polygon.h"
#include "profiler.h"
#include "skin.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <unordered_map>

using namespace ed;

namespace {
double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
template <class F>
double timeMs(F&& f) {
    auto t0 = std::chrono::steady_clock::now();
    f();
    return msSince(t0);
}
std::string fmtMs(double ms) {
    if (ms < 0) return "-";
    if (ms < 10) return strf("%.2f", ms);
    if (ms < 1000) return strf("%.1f", ms);
    return strf("%.0f", ms);
}
bool closedConsistent(const Mesh& m) {
    std::unordered_map<uint64_t, int> directed;
    directed.reserve(m.faces.size() * 4);
    for (const auto& f : m.faces)
        for (size_t i = 0; i < f.size(); ++i)
            directed[(uint64_t(uint32_t(f[i])) << 32) | uint32_t(f[(i + 1) % f.size()])]++;
    for (const auto& kv : directed) {
        if (kv.second != 1) return false;
        auto rev = directed.find((kv.first << 32) | (kv.first >> 32));
        if (rev == directed.end() || rev->second != 1) return false;
    }
    return !m.faces.empty();
}
uint32_t nextRand(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}
}  // namespace

// ============================================================================
// Invariants checked after every fuzzed frame / tool
// ============================================================================
bool Editor::stressCheck(std::string& problem) {
    auto finite3 = [](Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
    if (!finite3(cam_.target) || !std::isfinite(cam_.distance) || !std::isfinite(cam_.yaw) || !std::isfinite(cam_.pitch) ||
        cam_.distance <= 0) {
        problem = "camera is not finite";
        return false;
    }
    if (scene_.active >= (int)scene_.objects.size()) {
        problem = "active index out of range";
        return false;
    }
    for (const Object& o : scene_.objects) {
        if (!finite3(o.position) || !finite3(o.rotation) || !finite3(o.scale)) {
            problem = "object " + o.name + " has a non-finite transform";
            return false;
        }
        if (o.isMesh()) {
            std::string e = meshedit::checkMesh(o.mesh);
            if (!e.empty()) {
                problem = "mesh " + o.name + ": " + e;
                return false;
            }
        }
    }
    if (mode_ == Mode::Edit && !activeMesh()) {
        problem = "edit mode without an active mesh";
        return false;
    }
    if (layoutDone_ && !(viewport_.w >= 1 && viewport_.h >= 1)) {  // (before the first layout it is unset)
        problem = "viewport has no size";
        return false;
    }
    return true;
}

// ============================================================================
// Random input for the fuzzing phase
// ============================================================================
Input Editor::stressInput(const Input& real, int w, int h) {
    Input in = real;
    in.beginFrame();
    std::fill(std::begin(in.keyDown), std::end(in.keyDown), false);
    uint32_t& r = fuzzRng_;
    auto chance = [&](int pct) { return (int)(nextRand(r) % 100) < pct; };
    // Mouse: wander, sometimes jump; buttons held for a few frames (drags).
    float nx = fuzzMouse_.x, ny = fuzzMouse_.y;
    if (chance(10)) nx = (float)(nextRand(r) % (uint32_t)std::max(1, w)), ny = (float)(nextRand(r) % (uint32_t)std::max(1, h));
    else nx += (float)((int)(nextRand(r) % 61) - 30), ny += (float)((int)(nextRand(r) % 61) - 30);
    nx = clampf(nx, 0, (float)w - 1), ny = clampf(ny, 0, (float)h - 1);
    in.hasMouse = true;
    in.mouseDX = nx - fuzzMouse_.x;
    in.mouseDY = ny - fuzzMouse_.y;
    in.mouseX = fuzzMouse_.x = nx;
    in.mouseY = fuzzMouse_.y = ny;
    for (int b = 0; b < 3; ++b) {
        if (fuzzMouse_.down[b]) {
            if (--fuzzMouse_.hold[b] <= 0) {
                fuzzMouse_.down[b] = false;
                in.mouseReleased[b] = true;
            }
        } else if (chance(b == 0 ? 8 : 2)) {
            fuzzMouse_.down[b] = true;
            fuzzMouse_.hold[b] = 1 + (int)(nextRand(r) % 12);
            in.mousePressed[b] = true;
        }
        in.mouseDown[b] = fuzzMouse_.down[b];
    }
    if (chance(6)) in.wheel = (float)((int)(nextRand(r) % 7) - 3) * 0.5f;
    if (chance(3)) in.wheelX = (float)((int)(nextRand(r) % 5) - 2) * 0.5f;
    if (chance(2)) in.pinch = true, in.wheel = 1.0f;
    // Keys: letters, digits, punctuation, navigation, Esc / Enter / Tab, with
    // random modifiers. Never F12 / Ctrl+Shift+P (screenshots) - files.
    static const int kKeys[] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'Q', 'R', 'S',
                                'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
                                '-', '=', '[', ']', '/', KEY_ESCAPE, KEY_ENTER, KEY_TAB, KEY_SPACE, KEY_DELETE,
                                KEY_BACKSPACE, KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_F1, KEY_F3, KEY_F5,
                                KEY_PAGEUP, KEY_PAGEDOWN};
    if (chance(22)) {
        const int k = kKeys[nextRand(r) % (sizeof kKeys / sizeof kKeys[0])];
        const bool ctrl = chance(25), shift = chance(20), alt = chance(15);
        if (!(ctrl && shift && k == 'P')) {
            in.keyDown[KEY_CONTROL] = ctrl;
            in.keyDown[KEY_SHIFT] = shift;
            in.keyDown[KEY_ALT] = alt;
            in.keyDown[k] = in.keyPressed[k] = in.keyRepeat[k] = true;
            if (k < 128 && !ctrl && !alt) in.text.push_back((char)(shift ? k : std::tolower(k)));
        }
    }
    if (chance(4)) {  // typed numbers / expressions (fields, modal transforms, palette)
        static const char* const kText[] = {"2", "-1.5", "0.25", "1/0", "0/0", "1e30", "-1e30", "2*pi", "+=0.5", "abc", "nan"};
        in.text += kText[nextRand(r) % (sizeof kText / sizeof kText[0])];
    }
    return in;
}

// ============================================================================
// Tool fuzzing (synchronous): random tools, selections and settings
// ============================================================================
void Editor::stressToolFuzz(int ops) {
    using namespace meshedit;
    struct Stat {
        int runs = 0, ok = 0, refused = 0, broken = 0, openedClosed = 0;
        double maxMs = 0, totalMs = 0;
    };
    std::map<std::string, Stat> stats;
    std::vector<std::string> problems;
    uint32_t r = 0xC0FFEEu;
    auto rnd = [&](int n) { return n > 0 ? (int)(nextRand(r) % (uint32_t)n) : 0; };
    auto rndf = [&](float lo, float hi) { return lo + (hi - lo) * (float)(nextRand(r) % 10000) / 9999.0f; };
    // Base meshes: closed, open, with n-gons, triangles, degenerate pieces.
    std::vector<std::pair<std::string, Mesh>> bases;
    bases.push_back({"cube", primitives::cube(2.0f)});
    bases.push_back({"sphere", primitives::uvSphere(1.0f, 12, 8)});
    bases.push_back({"cylinder", primitives::cylinder(1.0f, 2.0f, 16)});
    bases.push_back({"torus", primitives::torus(1.0f, 0.35f, 16, 8)});
    bases.push_back({"plane", primitives::plane(2.0f, 4)});
    bases.push_back({"cone", primitives::cone(1.0f, 2.0f, 12)});
    {
        Mesh tri;
        tri.verts = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
        tri.faces = {{0, 1, 2}};
        tri.touch();
        bases.push_back({"triangle", tri});
    }
    {
        Mesh d = primitives::cube(1.0f);
        d.verts.push_back(d.verts[0]);  // a duplicate position and a sliver face
        d.faces.push_back({0, (int)d.verts.size() - 1, 1});
        d.validate();
        d.touch();
        bases.push_back({"degenerate", d});
    }
    {
        Mesh two;
        appendMesh(two, primitives::cube(1.0f), translation({-2, 0, 0}));
        appendMesh(two, primitives::cube(1.0f), translation({2, 0, 0}));
        bases.push_back({"two cubes", two});
    }
    {
        Mesh ngon;  // a concave 12-gon prism (n-gon caps)
        const int n = 12;
        for (int k = 0; k < 2 * n; ++k) {
            float a = 2 * kPi * (k % n) / n, rad = (k % 2) ? 1.0f : 0.5f;
            ngon.verts.push_back({rad * std::cos(a), k < n ? -0.5f : 0.5f, rad * std::sin(a)});
        }
        std::vector<int> bottom, top;
        for (int k = 0; k < n; ++k) bottom.push_back(n - 1 - k), top.push_back(n + k);
        ngon.faces.push_back(bottom);
        ngon.faces.push_back(top);
        for (int k = 0; k < n; ++k) ngon.faces.push_back({k, (k + 1) % n, n + (k + 1) % n, n + k});
        ngon.touch();
        bases.push_back({"star prism", ngon});
    }
    Mesh m;
    std::string baseName;
    int sinceReset = 1 << 30;
    static const char* const kTools[] = {"extrude faces", "extrude edges", "inset", "bevel edges", "bevel verts",
                                         "subdivide", "loop cut", "connect", "poke", "bridge loops", "bridge faces",
                                         "fill", "push through", "merge", "delete faces", "catmull-clark",
                                         "triangulate", "clean up", "tris to quads", "flip", "unwrap", "boolean diff"};
    const int toolCount = (int)(sizeof kTools / sizeof kTools[0]);
    for (int op = 0; op < ops; ++op) {
        if (sinceReset > 6 || m.faces.empty() || m.faces.size() > 20000) {
            const auto& b = bases[rnd((int)bases.size())];
            m = b.second;
            baseName = b.first;
            sinceReset = 0;
        }
        ++sinceReset;
        const std::string tool = kTools[rnd(toolCount)];
        Stat& st = stats[tool];
        ++st.runs;
        const bool wasClosed = closedConsistent(m);
        if (std::getenv("M3D_FUZZ_TRACE"))
            std::fprintf(stderr, "op %d %s on %s (%d faces, %d verts)\n", op, tool.c_str(), baseName.c_str(),
                         (int)m.faces.size(), (int)m.verts.size()),
                std::fflush(stderr);
        // Random selections.
        const int nf = (int)m.faces.size(), nv = (int)m.verts.size();
        std::vector<int> faces;
        for (int k = 0, c = 1 + rnd(std::max(1, nf / 3)); k < c; ++k) faces.push_back(rnd(nf));
        std::sort(faces.begin(), faces.end());
        faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
        std::vector<Edge> all = uniqueEdges(m), edges;
        for (int k = 0, c = 1 + rnd(std::max(1, (int)all.size() / 4)); k < c && !all.empty(); ++k)
            edges.push_back(all[rnd((int)all.size())]);
        std::sort(edges.begin(), edges.end());
        edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
        std::vector<char> vsel(nv, 0);
        for (int k = 0, c = 1 + rnd(std::max(1, nv / 3)); k < c && nv > 0; ++k) vsel[rnd(nv)] = 1;
        std::vector<int> verts;
        for (int v = 0; v < nv; ++v)
            if (vsel[v]) verts.push_back(v);
        std::vector<int> newFaces;
        std::vector<char> outSel;
        std::string err;
        bool ok = true;
        const Mesh before = m;
        std::string detail;  // the tool's settings, for reproducing a failure
        double ms = timeMs([&] {
            if (tool == "extrude faces") ok = extrudeFaces(m, faces, outSel, nullptr);
            else if (tool == "extrude edges") {
                std::vector<Edge> ne;
                ok = extrudeEdges(m, edges, ne, outSel);
            } else if (tool == "inset") {
                InsetParams p;
                p.thickness = rndf(0.0f, 0.6f);
                p.depth = rndf(-0.5f, 0.5f);
                p.dish = rnd(3) == 0 ? rndf(-0.3f, 0.3f) : 0.0f;
                p.individual = rnd(2) == 0;
                detail = strf("thickness %g depth %g dish %g individual %d", p.thickness, p.depth, p.dish, (int)p.individual);
                ok = insetFaces(m, faces, p, newFaces, outSel);
            } else if (tool == "bevel edges" || tool == "bevel verts") {
                BevelParams p;
                p.width = rndf(0.01f, 1.5f);
                p.segments = 1 + rnd(4);
                p.profile = rndf(0.0f, 1.0f);
                p.clamp = rnd(4) != 0;
                detail = strf("width %g segments %d profile %g clamp %d", p.width, p.segments, p.profile, (int)p.clamp);
                ok = tool == "bevel edges" ? bevel(m, edges, {}, p, newFaces, outSel) : bevel(m, {}, verts, p, newFaces, outSel);
            } else if (tool == "subdivide") {
                const int cuts = 1 + rnd(4);
                const bool connect = rnd(2) == 0;
                detail = strf("cuts %d connect %d", cuts, (int)connect);
                ok = subdivideEdges(m, edges, cuts, connect, outSel);
            } else if (tool == "loop cut") {
                if (all.empty()) {
                    ok = false;
                } else {
                    const Edge e = all[rnd((int)all.size())];
                    const int cuts = 1 + rnd(3);
                    detail = strf("edge %d %d cuts %d", e.first, e.second, cuts);
                    ok = loopCut(m, e, cuts, outSel);
                }
            }
            else if (tool == "connect") ok = connectVertices(m, vsel) > 0;
            else if (tool == "poke") ok = pokeFaces(m, faces, rndf(-0.5f, 0.5f), newFaces, outSel);
            else if (tool == "bridge loops") {
                BridgeParams p;
                p.segments = 1 + rnd(4);
                p.twist = rnd(5) - 2;
                ok = bridgeLoops(m, edges, p, newFaces, outSel, err);
            } else if (tool == "bridge faces") {
                BridgeParams p;
                p.segments = 1 + rnd(3);
                detail = strf("segments %d", p.segments);
                ok = bridgeRegions(m, faces, p, newFaces, outSel, err);
            } else if (tool == "fill") ok = fillHoles(m, edges, newFaces, err) > 0;
            else if (tool == "push through") {
                PushThroughParams p;
                p.inset = rnd(2) ? rndf(0.0f, 0.6f) : 0.0f;
                ok = pushThrough(m, std::vector<int>(faces.begin(), faces.begin() + std::min<size_t>(faces.size(), 1 + rnd(3))),
                                 p, newFaces, outSel, err);
            } else if (tool == "merge") {
                const bool groups = rnd(2) == 0;
                detail = strf("per group %d", (int)groups);
                ok = mergeVertices(m, vsel, groups) > 0;
            }
            else if (tool == "delete faces") {
                std::vector<char> kill(m.faces.size(), 0);
                for (int f : faces) kill[f] = 1;
                deleteFaces(m, kill);
            } else if (tool == "catmull-clark") {
                if (m.faces.size() < 4000) m = catmullClark(m);
                else ok = false;
            } else if (tool == "triangulate") ok = poly::triangulateMesh(m, 4) > 0;
            else if (tool == "clean up") poly::cleanup(m);
            else if (tool == "tris to quads") ok = poly::trisToQuads(m) > 0;
            else if (tool == "flip") flipNormals(m);
            else if (tool == "unwrap") uv::unwrap(m, uv::Method::Smart);
            else if (tool == "boolean diff") {
                if (wasClosed && m.faces.size() < 3000) {
                    Mesh cutter = primitives::cube(rndf(0.3f, 1.6f));
                    for (Vec3& p : cutter.verts) p += Vec3(rndf(-1, 1), rndf(-1, 1), rndf(-1, 1));
                    if (std::getenv("M3D_FUZZ_TRACE")) {  // keep the inputs of the last boolean for a repro
                        Scene dump;
                        dump.add(m, "A", Vec3(1, 1, 1));
                        dump.add(cutter, "B", Vec3(1, 1, 1));
                        std::string e;
                        saveScene(dump, "fuzz_last_boolean.m3d", e);
                    }
                    csg::Result out = csg::apply(m, cutter, csg::Op::Difference);
                    ok = !out.mesh.faces.empty();
                    if (ok) m = std::move(out.mesh);
                } else {
                    ok = false;
                }
            }
        });
        st.totalMs += ms;
        st.maxMs = std::max(st.maxMs, ms);
        if (ok) ++st.ok;
        else ++st.refused;
        const bool trace = std::getenv("M3D_FUZZ_TRACE") != nullptr;
        auto dump = [&](const std::string& what) {
            if (!trace) return;
            static int n = 0;
            if (n >= 40) return;
            std::string base = strf("fuzz_fail_%02d", n++);
            Scene sc;
            sc.add(before, "Input", Vec3(1, 1, 1));
            std::string err2;
            saveScene(sc, base + ".m3d", err2);
            if (FILE* f = std::fopen((base + ".txt").c_str(), "wb")) {
                std::fprintf(f, "tool %s\nbase %s\nproblem %s\nparams %s\nfaces", tool.c_str(), baseName.c_str(),
                             what.c_str(), detail.c_str());
                for (int x : faces) std::fprintf(f, " %d", x);
                std::fprintf(f, "\nedges");
                for (const Edge& x : edges) std::fprintf(f, " %d %d", x.first, x.second);
                std::fprintf(f, "\nverts");
                for (int x : verts) std::fprintf(f, " %d", x);
                std::fprintf(f, "\n");
                std::fclose(f);
            }
        };
        const std::string e = checkMesh(m);
        if (!e.empty()) {
            ++st.broken;
            dump(e);
            if (problems.size() < 12) problems.push_back(tool + " on " + baseName + ": " + e);
            m = before;  // continue from a valid mesh
            continue;
        }
        // Tools that must keep a closed, consistently oriented mesh closed.
        static const char* const kKeepsClosed[] = {"extrude faces", "inset", "bevel edges", "bevel verts", "subdivide",
                                                   "loop cut", "connect", "poke", "bridge faces", "push through",
                                                   "catmull-clark", "triangulate", "flip", "unwrap"};
        bool keeps = false;
        for (const char* k : kKeepsClosed) keeps = keeps || tool == k;
        if (ok && keeps && wasClosed && !closedConsistent(m)) {
            ++st.openedClosed;
            dump("a closed mesh is no longer closed");
            if (problems.size() < 12) problems.push_back(tool + " on " + baseName + ": a closed mesh is no longer closed");
        }
    }
    stressText_ += strf("\n## D. Tool fuzzing\n\n%d random operations on %d kinds of mesh (closed, open, n-gons, a "
                        "single triangle, degenerate faces, two separate pieces) with random selections and "
                        "settings. After every operation the mesh is checked: indices in range, faces of 3+ distinct "
                        "corners, UV / weight arrays in step, finite positions; and tools that should keep a closed "
                        "mesh closed are checked for that.\n\n",
                        ops, (int)bases.size());
    stressText_ += "| Tool | Runs | Done | Refused | Invalid mesh | Closed mesh opened | Avg ms | Max ms |\n"
                   "|---|---:|---:|---:|---:|---:|---:|---:|\n";
    int broken = 0, opened = 0;
    for (const auto& kv : stats) {
        const Stat& s = kv.second;
        broken += s.broken;
        opened += s.openedClosed;
        stressText_ += strf("| %s | %d | %d | %d | %d | %d | %s | %s |\n", kv.first.c_str(), s.runs, s.ok, s.refused,
                            s.broken, s.openedClosed, fmtMs(s.runs ? s.totalMs / s.runs : 0).c_str(),
                            fmtMs(s.maxMs).c_str());
    }
    stressText_ += strf("\nInvalid meshes: **%d**. Closed meshes opened by a tool that should keep them closed: **%d**.\n",
                        broken, opened);
    for (const std::string& p : problems) stressText_ += "- " + p + "\n";
    stressFailures_ += broken;
}

// ============================================================================
// Steps
// ============================================================================
void Editor::stressInit() {
    stressing_ = true;
    configPath_.clear();                // never overwrite the user's settings
    platform::setDialogsEnabled(false);  // never block on a file dialog
    fileField_ = filePath_ = "stress_tmp.m3d";
    stressSteps_.clear();
    char group = 'A';  // groups A..G (see the top of this file); --stress-only picks some
    auto add = [&](const std::string& name, std::function<bool()> setup, std::function<void(int)> perFrame, int frames,
                   std::function<void(double, double, double)> done) {
        if (!stressOnly_.empty() && stressOnly_.find(group) == std::string::npos) return;
        StressStep s;
        s.name = name;
        s.setup = std::move(setup);
        s.perFrame = std::move(perFrame);
        s.frames = frames;
        s.done = std::move(done);
        stressSteps_.push_back(std::move(s));
    };

    // ---------------- A. mesh size ladder ----------------
    ladder_.assign(6, LadderRow());
    for (int level = 0; level < 6; ++level) {
        add(
            strf("mesh level %d idle", level),
            [this, level] {
                LadderRow& row = ladder_[level];
                if (level > 0 && (ladder_[level - 1].skipped || ladder_[level - 1].idleMs > 400.0 ||
                                  platform::memoryMB() > 6000.0)) {
                    row.skipped = true;
                    return false;
                }
                Mesh prev;
                if (level > 0 && !scene_.objects.empty()) prev = std::move(scene_.objects[0].mesh);
                if (xf_ != Xform::None) endTransform(true);
                setMode(Mode::Object);
                clearForBenchmark();
                meshAccel_.clear();
                meshShare_.clear();
                Mesh m;
                row.genMs = timeMs([&] { m = level == 0 ? primitives::uvSphere(1.5f, 64, 32) : catmullClark(prev); });
                prev = Mesh();
                row.tris = (int)m.triangleCount();
                int i = scene_.add(std::move(m), "Dense", kPalette[4]);
                selectOnly(i);
                cam_.distance = 6;
                updateMatrices();
                return true;
            },
            nullptr, 6,
            [this, level](double first, double avg, double) {
                ladder_[level].firstMs = first;
                ladder_[level].idleMs = avg;
            });
        add(
            strf("mesh level %d drag", level), [this, level] {
                if (ladder_[level].skipped || scene_.objects.empty()) return false;
                selectOnly(0);
                setMode(Mode::Edit);
                vsel_.assign(scene_.objects[0].mesh.verts.size(), 1);
                selTopology_ = 0;
                return true;
            },
            [this](int frame) {
                Object* o = activeMesh();
                if (!o) return;
                const float dy = 0.002f * std::sin(frame * 0.7f);
                for (Vec3& v : o->mesh.verts) v.y += dy;
                o->mesh.touchPositions();
            },
            6, [this, level](double, double avg, double) { ladder_[level].dragMs = avg; });
        add(
            strf("mesh level %d ops", level), [this, level] {
                LadderRow& row = ladder_[level];
                if (row.skipped || scene_.objects.empty()) return false;
                setMode(Mode::Object);
                Object& o = scene_.objects[0];
                meshAccel_.erase(o.id);
                row.bvhMs = timeMs([&] { meshAccel(o); });
                row.pickMs = timeMs([&] { clickSelect({viewport_.w * 0.5f, viewport_.h * 0.5f}, false); });
                selectOnly(0);
                undo_.clear();
                meshShare_.clear();
                row.undoFirst = timeMs([&] { pushUndo(); });
                row.undoAgain = timeMs([&] { pushUndo(); });
                undo_.clear();
                {
                    // Bevel the equator loop and loop-cut it, on a copy.
                    meshedit::Edge e{-1, -1};
                    const Mesh& src = scene_.objects[0].mesh;
                    float best = 1e30f;
                    for (const auto& f : src.faces)
                        for (size_t k = 0; k < f.size(); ++k) {
                            int a = f[k], b = f[(k + 1) % f.size()];
                            float y = std::fabs(src.verts[a].y) + std::fabs(src.verts[b].y);
                            if (y < best) best = y, e = meshedit::makeEdge(a, b);
                        }
                    Mesh copy = src;
                    std::vector<meshedit::Edge> loop;
                    row.loopSelMs = timeMs([&] { loop = meshedit::edgeLoop(copy, e); });
                    meshedit::BevelParams bp;
                    bp.width = 0.01f;
                    bp.segments = 2;
                    std::vector<int> nf;
                    std::vector<char> sel;
                    row.bevelMs = timeMs([&] { meshedit::bevel(copy, loop, {}, bp, nf, sel); });
                    row.bevelOk = meshedit::checkMesh(copy).empty();
                    Mesh copy2 = src;
                    row.loopCutMs = timeMs([&] { meshedit::loopCut(copy2, e, 1, sel); });
                }
                if (level <= 4) {
                    std::string err;
                    row.saveMs = timeMs([&] { saveScene(scene_, "stress_tmp.m3d", err); });
                    Scene s;
                    row.loadMs = timeMs([&] { loadScene(s, "stress_tmp.m3d", err); });
                    std::remove("stress_tmp.m3d");
                    rt::Settings rs;
                    rt::SceneData data;
                    row.rtMs = timeMs([&] { rt::buildScene(scene_, rs, data); });
                }
                row.memMB = platform::memoryMB();
                return true;
            },
            nullptr, 1, nullptr);
    }
    add(
        "mesh ladder report", [this] {
            stressText_ += "\n## A. Mesh size ladder\n\nA UV sphere, Catmull-Clark subdivided once per level. *Idle* = "
                           "the dense mesh selected in Object mode; *drag* = Edit mode with every vertex selected and "
                           "moved each frame (what dragging a handle does). Frame times include the GPU (glFinish).\n\n";
            stressText_ += "| Triangles | Build ms | First frame | Idle frame | Drag frame | Pick BVH | Click pick | "
                           "Undo (1st / again) | Edge loop | Bevel loop | Loop cut | Save | Load | Path-trace BVH | Memory MB |\n"
                           "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
            for (const LadderRow& r : ladder_) {
                if (r.skipped) {
                    stressText_ += "| (skipped: previous level too slow or memory limit) |||||||||||||||\n";
                    break;
                }
                stressText_ += strf("| %d | %s | %s | %s | %s | %s | %s | %s / %s | %s | %s%s | %s | %s | %s | %s | %.0f |\n",
                                    r.tris, fmtMs(r.genMs).c_str(), fmtMs(r.firstMs).c_str(), fmtMs(r.idleMs).c_str(),
                                    fmtMs(r.dragMs).c_str(), fmtMs(r.bvhMs).c_str(), fmtMs(r.pickMs).c_str(),
                                    fmtMs(r.undoFirst).c_str(), fmtMs(r.undoAgain).c_str(), fmtMs(r.loopSelMs).c_str(),
                                    fmtMs(r.bevelMs).c_str(), r.bevelOk ? "" : " (invalid!)", fmtMs(r.loopCutMs).c_str(),
                                    r.saveMs > 0 ? fmtMs(r.saveMs).c_str() : "-", r.loadMs > 0 ? fmtMs(r.loadMs).c_str() : "-",
                                    r.rtMs > 0 ? fmtMs(r.rtMs).c_str() : "-", r.memMB);
                if (!r.bevelOk) ++stressFailures_;
            }
            // Interactivity limits.
            int idleLimit = 0, dragLimit = 0;
            for (const LadderRow& r : ladder_)
                if (!r.skipped) {
                    if (r.idleMs <= 33.4) idleLimit = std::max(idleLimit, r.tris);
                    if (r.dragMs <= 100.0) dragLimit = std::max(dragLimit, r.tris);
                }
            stressText_ += strf("\nLargest mesh still at 30+ fps idle: **%d triangles**; largest mesh whose vertices "
                                "can all be dragged at 10+ fps: **%d triangles**.\n",
                                idleLimit, dragLimit);
            setMode(Mode::Object);
            clearForBenchmark();
            meshAccel_.clear();
            meshShare_.clear();
            return false;
        },
        nullptr, 0, nullptr);

    // ---------------- B. object count ladder ----------------
    group = 'B';
    static const int kCounts[4] = {1000, 5000, 20000, 50000};
    objLadder_.assign(4, ObjRow());
    for (int k = 0; k < 4; ++k) {
        const int n = kCounts[k];
        add(
            strf("%d objects idle", n),
            [this, k, n] {
                ObjRow& row = objLadder_[k];
                row.n = n;
                if (k > 0 && (objLadder_[k - 1].skipped || objLadder_[k - 1].idleMs > 500.0)) {
                    row.skipped = true;
                    return false;
                }
                clearForBenchmark();
                meshShare_.clear();
                row.memBeforeMB = platform::memoryMB();
                const int side = (int)std::ceil(std::sqrt((double)n));
                row.buildMs = timeMs([&] {
                    const Mesh cube = primitives::cube(0.8f);
                    for (int i = 0; i < n; ++i) {
                        Object o;
                        o.kind = ObjectKind::Mesh;
                        o.mesh = cube;
                        o.name = "Cube";
                        o.color = kPalette[1 + i % 9];
                        o.position = {(float)(i % side) * 1.5f - side * 0.75f, 0, (float)(i / side) * 1.5f - side * 0.75f};
                        scene_.addObject(std::move(o));
                    }
                });
                selectOnly(-1);
                cam_.distance = side * 1.4f;
                cam_.pitch = 45;
                updateMatrices();
                row.memCreatedMB = platform::memoryMB();
                return true;
            },
            nullptr, 6,
            [this, k](double first, double avg, double) {
                objLadder_[k].firstMs = first;
                objLadder_[k].idleMs = avg;
                objLadder_[k].memDrawnMB = platform::memoryMB();
            });
        add(
            strf("%d objects selected", n), [this, k] {
                if (objLadder_[k].skipped) return false;
                for (Object& o : scene_.objects) o.selected = true;
                scene_.active = 0;
                return true;
            },
            nullptr, 6, [this, k](double, double avg, double) { objLadder_[k].selMs = avg; });
        add(
            strf("%d objects rotate", n), [this, k] {
                if (objLadder_[k].skipped) return false;
                tool_ = Tool::Rotate;
                pivotCenter_ = true;
                return beginTransform(Xform::Rotate, false);
            },
            [this](int frame) {
                if (xf_ == Xform::None) return;
                XfDelta d;
                d.rotAxis = {0, 1, 0};
                d.rotDeg = 1.0f * frame;
                applyTransform(d);
            },
            6,
            [this, k](double, double avg, double) {
                objLadder_[k].rotMs = avg;
                if (xf_ != Xform::None) endTransform(true);
                tool_ = Tool::Move;
                pivotCenter_ = false;
            });
        add(
            strf("%d objects ops", n), [this, k] {
                ObjRow& row = objLadder_[k];
                if (row.skipped) return false;
                undo_.clear();
                row.dupMs = timeMs([&] {
                    duplicateSelected();
                    if (xf_ != Xform::None) endTransform(true);  // duplicate starts a move: place it
                });
                row.undoMs = timeMs([&] { undo(); });
                for (Object& o : scene_.objects) o.selected = true;
                row.delMs = timeMs([&] { deleteSelected(); });
                row.memMB = platform::memoryMB();
                undo_.clear();
                redo_.clear();
                return true;
            },
            nullptr, 1, nullptr);
    }
    add(
        "object ladder report", [this] {
            stressText_ += "\n## B. Object count ladder\n\nCubes on a grid, each with its own mesh. *Rotate* = all of "
                           "them rotated by the transform tool every frame.\n\n"
                           "| Objects | Create ms | First frame | Idle frame | All selected | Rotate all | Duplicate all | "
                           "Undo it | Delete all | MB: objects / +drawn / end |\n|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
            for (const ObjRow& r : objLadder_) {
                if (r.skipped) {
                    stressText_ += "| (skipped: previous count too slow) |||||||||\n";
                    break;
                }
                stressText_ += strf("| %d | %s | %s | %s | %s | %s | %s | %s | %s | %.0f / %.0f / %.0f |\n", r.n,
                                    fmtMs(r.buildMs).c_str(), fmtMs(r.firstMs).c_str(), fmtMs(r.idleMs).c_str(),
                                    fmtMs(r.selMs).c_str(), fmtMs(r.rotMs).c_str(), fmtMs(r.dupMs).c_str(),
                                    fmtMs(r.undoMs).c_str(), fmtMs(r.delMs).c_str(), r.memCreatedMB - r.memBeforeMB,
                                    r.memDrawnMB - r.memCreatedMB, r.memMB);
            }
            int limit = 0;
            for (const ObjRow& r : objLadder_)
                if (!r.skipped && r.idleMs <= 33.4) limit = std::max(limit, r.n);
            stressText_ += strf("\nMost objects still at 30+ fps idle: **%d**.\n", limit);
            clearForBenchmark();
            meshShare_.clear();
            return false;
        },
        nullptr, 0, nullptr);

    // ---------------- C. deep hierarchies ----------------
    group = 'C';
    static const int kDepths[3] = {1000, 10000, 50000};
    for (int k = 0; k < 3; ++k) {
        const int depth = kDepths[k];
        add(
            strf("chain %d idle", depth),
            [this, k, depth] {
                ChainRow& row = chains_[k];
                row.depth = depth;
                clearForBenchmark();
                row.buildMs = timeMs([&] {
                    uint32_t prev = 0;
                    for (int i = 0; i < depth; ++i) {
                        Object o;
                        o.kind = ObjectKind::Empty;
                        o.name = "Link";
                        o.boneLength = 0.05f;
                        o.position = i ? Vec3(0.02f, 0.01f, 0) : Vec3();
                        o.rotation = {0, 0.05f, 0};
                        o.parent = prev;
                        int idx = scene_.addObject(std::move(o));
                        prev = scene_.objects[idx].id;
                    }
                });
                selectOnly(depth - 1);
                cam_.distance = 30;
                updateMatrices();
                return true;
            },
            nullptr, 6,
            [this, k](double first, double avg, double) {
                chains_[k].firstMs = first;
                chains_[k].idleMs = avg;
            });
        add(
            strf("chain %d ops", depth), [this, k, depth] {
                ChainRow& row = chains_[k];
                const int tail = depth - 1;
                Mat4 w;
                row.worldMs = timeMs([&] { w = scene_.world(tail); });
                std::vector<Mat4> all;
                row.allWorldsMs = timeMs([&] { scene_.computeWorlds(all); });
                // The tail's world position must equal the accumulated chain.
                Vec3 p = transformPoint(w, Vec3()), q = transformPoint(all[tail], Vec3());
                row.worldOk = length(p - q) <= 1e-3f * std::max(1.0f, length(p));
                std::vector<int> desc;
                row.descMs = timeMs([&] { desc = scene_.descendants(0); });
                row.descOk = (int)desc.size() == depth - 1;
                bool moved = true;
                row.cycleMs = timeMs([&] { moved = moveInHierarchy({scene_.objects[0].id}, scene_.objects[tail].id, DropOnto); });
                row.cycleRefused = !moved;
                for (Object& o : scene_.objects) o.selected = true;
                row.rootsMs = timeMs([&] { (void)transformRoots(); });
                selectOnly(0);
                undo_.clear();
                row.deleteMs = timeMs([&] { deleteSelected(); });
                row.deleteOk = (int)scene_.objects.size() == depth - 1 && scene_.parentIndex(0) < 0;
                undo_.clear();
                if (!row.worldOk || !row.descOk || !row.cycleRefused || !row.deleteOk) ++stressFailures_;
                return true;
            },
            nullptr, 1, nullptr);
    }
    add(
        "chain report", [this] {
            stressText_ += "\n## C. Deep hierarchies\n\nA chain of empties, each parented to the previous one, the deepest "
                           "selected. Checks: world matrix of the deepest equals the batch computation, descendants "
                           "of the root, parenting the root under the deepest is refused (cycle), deleting the root "
                           "keeps the rest.\n\n| Depth | Create ms | First frame | Idle frame | world(deepest) | All worlds | "
                           "Descendants | Cycle refused | Transform roots | Delete root | Checks |\n"
                           "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|\n";
            for (const ChainRow& r : chains_) {
                bool ok = r.worldOk && r.descOk && r.cycleRefused && r.deleteOk;
                stressText_ += strf("| %d | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n", r.depth,
                                    fmtMs(r.buildMs).c_str(), fmtMs(r.firstMs).c_str(), fmtMs(r.idleMs).c_str(),
                                    fmtMs(r.worldMs).c_str(), fmtMs(r.allWorldsMs).c_str(), fmtMs(r.descMs).c_str(),
                                    fmtMs(r.cycleMs).c_str(), fmtMs(r.rootsMs).c_str(), fmtMs(r.deleteMs).c_str(),
                                    ok ? "pass" : "**FAIL**");
            }
            clearForBenchmark();
            return false;
        },
        nullptr, 0, nullptr);

    // ---------------- D. tool fuzzing ----------------
    group = 'D';
    add("tool fuzz", [this] {
            stressToolFuzz(6000);
            return false;
        },
        nullptr, 0, nullptr);

    // ---------------- E. input + command fuzzing ----------------
    group = 'E';
    const int fuzzFrames = 3000;
    add(
        "input fuzz", [this] {
            clearForBenchmark();
            buildDemo(1);
            undo_.clear();
            fuzzInput_ = true;
            fuzzRng_ = 0xBADC0DEu;
            fuzzCommands_ = 0;
            fuzzProblems_.clear();
            fuzzFrameMs_.clear();
            return true;
        },
        [this](int frame) {
            std::string problem;
            if (frame > 0 && !stressCheck(problem) && fuzzProblems_.size() < 12) {
                fuzzProblems_.push_back(strf("frame %d: %s", frame, problem.c_str()));
                // Recover so the run can continue: a fresh scene.
                if (xf_ != Xform::None) endTransform(false);
                clearForBenchmark();
                buildDemo(1);
            }
            // A random available command every few frames (not quit / files / dialogs).
            if (frame % 5 == 2 && !commands_.empty()) {
                static const char* const kSkip[] = {"file.quit", "file.screenshot", "file.open", "file.import",
                                                    "mat.folder", "set.unity", "set.blender"};
                const Command& c = commands_[nextRand(fuzzRng_) % commands_.size()];
                bool skip = false;
                for (const char* s : kSkip) skip = skip || c.id == s;
                // Keep geometry bounded: no growing tools once the scene is big.
                const bool grows = c.id == "obj.subdivide" || c.id == "mesh.subdivide" || c.id == "mesh.bevel" ||
                                   c.id == "mesh.poke" || c.id == "mesh.loopcut" || c.id == "mesh.inset" ||
                                   c.id == "mesh.extrude" || c.id.compare(0, 4, "add.") == 0 || c.id == "edit.duplicate";
                if (grows && (scene_.triangleCount() > 300000 || scene_.objects.size() > 400)) skip = true;
                if (!skip && xf_ == Xform::None && !insetModal_) {
                    if (!c.available || c.available()) {
                        runCommand(c.id);
                        ++fuzzCommands_;
                    }
                }
            }
        },
        fuzzFrames,
        [this, fuzzFrames](double, double avg, double worst) {
            fuzzInput_ = false;
            if (xf_ != Xform::None) endTransform(false);
            insetModal_ = false;
            modalOp_ = 0;
            paletteOpen_ = false;
            showHelp_ = false;
            confirmQuit_ = false;
            openMenu_ = -1;
            flyToggle_ = false;
            std::string problem;
            if (!stressCheck(problem) && fuzzProblems_.size() < 12) fuzzProblems_.push_back("end: " + problem);
            std::vector<double> t = fuzzFrameMs_;
            std::sort(t.begin(), t.end());
            const double p95 = t.empty() ? 0 : t[std::min(t.size() - 1, (size_t)(t.size() * 0.95))];
            stressText_ += strf("\n## E. Input fuzzing\n\n%d frames of random mouse moves / clicks / drags, wheel and "
                                "trackpad gestures, key presses with random Ctrl / Shift / Alt, typed numbers and "
                                "expressions (including 1/0, 0/0, 1e30, nan), plus %d random commands from the command "
                                "registry. After every frame: finite camera and transforms, valid meshes, a consistent "
                                "mode.\n\nFrame time: average %s ms, 95th percentile %s ms, worst %s ms. Scene at the end: "
                                "%d objects, %d triangles.\n\nProblems found: **%d**\n",
                                fuzzFrames, fuzzCommands_, fmtMs(avg).c_str(), fmtMs(p95).c_str(), fmtMs(worst).c_str(),
                                (int)scene_.objects.size(), (int)scene_.triangleCount(), (int)fuzzProblems_.size());
            for (const std::string& p : fuzzProblems_) stressText_ += "- " + p + "\n";
            stressFailures_ += (int)fuzzProblems_.size();
        });
    add(
        "undo round trip", [this] {
            // 30 random scene-changing commands, then undo all of them and
            // redo all of them: the scene must come back exactly.
            if (xf_ != Xform::None) endTransform(true);
            if (mode_ == Mode::Edit) setMode(Mode::Object);
            clearForBenchmark();
            buildDemo(1);
            undo_.clear();
            redo_.clear();
            auto hashScene = [this] {
                uint64_t h = 1469598103934665603ull;
                auto mix = [&](double v) {
                    int64_t q = (int64_t)std::llround(v * 1000.0);
                    h = (h ^ (uint64_t)q) * 1099511628211ull;
                };
                mix((double)scene_.objects.size());
                for (const Object& o : scene_.objects) {
                    mix(o.id), mix(o.parent), mix((int)o.kind);
                    for (int a = 0; a < 3; ++a) mix(o.position[a]), mix(o.rotation[a]), mix(o.scale[a]), mix(o.color[a]);
                    mix((double)o.mesh.verts.size()), mix((double)o.mesh.faces.size());
                    double s = 0;
                    for (const Vec3& v : o.mesh.verts) s += v.x * 0.37 + v.y * 0.61 + v.z * 0.83;
                    mix(s);
                }
                return h;
            };
            const uint64_t start = hashScene();
            static const char* const kOps[] = {"add.cube", "add.sphere", "add.torus", "add.point", "edit.duplicate",
                                               "edit.delete", "obj.flip", "obj.smooth", "obj.flat", "obj.subdivide",
                                               "mat.metal", "mat.glass", "edit.next", "obj.parent", "obj.unparent",
                                               "obj.join", "edit.selectall"};
            uint32_t r = 777u;
            int ran = 0;
            for (int k = 0; k < 30; ++k) {
                const char* id = kOps[nextRand(r) % (sizeof kOps / sizeof kOps[0])];
                if (scene_.triangleCount() > 200000 && std::string(id) == "obj.subdivide") continue;
                if (runCommand(id)) ++ran;
                if (xf_ != Xform::None) endTransform(true);  // duplicate / extrude start a move: place it
            }
            const uint64_t after = hashScene();
            const int steps = (int)undo_.size();
            for (int k = 0; k < steps; ++k) undo();
            const bool backOk = hashScene() == start;
            for (int k = 0; k < steps; ++k) redo();
            const bool forwardOk = hashScene() == after;
            stressText_ += strf("\n### Undo / redo round trip\n\n%d random commands (%d undo steps), all undone, all "
                                "redone: back to the start **%s**, forward to the end **%s**.\n",
                                ran, steps, backOk ? "yes" : "NO", forwardOk ? "yes" : "NO");
            if (!backOk || !forwardOk) ++stressFailures_;
            stressText_ += strf("Undo history memory with shared meshes: %.1f MB.\n", undoBytes() / (1024.0 * 1024.0));
            undo_.clear();
            redo_.clear();
            return false;
        },
        nullptr, 0, nullptr);

    // ---------------- F. layout extremes ----------------
    group = 'F';
    static const int kSizes[5][2] = {{320, 240}, {640, 480}, {1024, 600}, {1920, 1080}, {3840, 2160}};
    for (int s = 0; s < 5; ++s)
        for (int scale : {1, 3, 6}) {
            add(
                strf("layout %dx%d scale %d", kSizes[s][0], kSizes[s][1], scale),
                [this, s, scale] {
                    stressW_ = kSizes[s][0];
                    stressH_ = kSizes[s][1];
                    access_.uiScale = scale;
                    applyUiScale();
                    if (scene_.objects.empty()) buildDemo(1);
                    return true;
                },
                nullptr, 3,
                [this, s, scale](double, double avg, double) {
                    std::string problem;
                    bool ok = stressCheck(problem);
                    auto inside = [&](const Rect& r) {
                        return r.w >= 0 && r.h >= 0 && r.x >= -0.5f && r.y >= -0.5f && r.x + r.w <= stressW_ + 0.5f &&
                               r.y + r.h <= stressH_ + 0.5f;
                    };
                    for (const Rect* r : {&topBar_, &leftPanel_, &rightPanel_, &statusBar_, &viewport_})
                        if (!inside(*r)) ok = false, problem = "a panel is outside the window";
                    layoutRows_ += strf("| %dx%d | %d | %s | %s |\n", kSizes[s][0], kSizes[s][1], scale, fmtMs(avg).c_str(),
                                        ok ? "ok" : ("**" + problem + "**").c_str());
                    if (!ok) ++stressFailures_;
                });
        }
    add(
        "layout report", [this] {
            stressW_ = stressH_ = 0;
            access_.uiScale = 0;
            applyUiScale();
            stressText_ += "\n## F. Layout extremes\n\nEvery window size at UI sizes 1, 3 and 6: all panels inside the "
                           "window, the viewport keeps a size, the scene stays valid.\n\n| Window | UI size | Frame ms | "
                           "Result |\n|---|---:|---:|---|\n" + layoutRows_;
            return false;
        },
        nullptr, 0, nullptr);

    // ---------------- G. extreme values ----------------
    group = 'G';
    add(
        "extreme values", [this] {
            clearForBenchmark();
            std::string rows;
            auto row = [&](const char* what, double ms, bool ok) {
                rows += strf("| %s | %s | %s |\n", what, fmtMs(ms).c_str(), ok ? "ok" : "**FAIL**");
                if (!ok) ++stressFailures_;
            };
            int i = scene_.add(primitives::cube(1.0f), "Far", kPalette[2]);
            scene_.objects[i].position = {1e6f, -1e6f, 1e6f};
            int j = scene_.add(primitives::cube(1.0f), "Tiny", kPalette[3]);
            scene_.objects[j].scale = {1e-5f, 1e-5f, 1e-5f};
            int k = scene_.add(primitives::cube(1.0f), "Huge", kPalette[4]);
            scene_.objects[k].scale = {1e5f, 1e5f, 1e5f};
            selectOnly(i);
            double ms = timeMs([&] { frameSelected(); });
            row("Frame an object at 1e6", ms, stressCheck(problemScratch_));
            cam_.distance = 0.05f;
            row("Camera at the minimum distance", 0, stressCheck(problemScratch_));
            cam_.distance = 5000.0f;
            row("Camera at the maximum distance", 0, stressCheck(problemScratch_));
            // A 20000-corner concave (star) polygon: triangulation and render data.
            Mesh star;
            const int n = 20000;
            std::vector<int> face;
            for (int c = 0; c < n; ++c) {
                float a = 2 * kPi * c / n, rad = (c % 2) ? 1.0f : 0.6f;
                star.verts.push_back({rad * std::cos(a), 0, rad * std::sin(a)});
                face.push_back(n - 1 - c);
            }
            star.faces.push_back(face);
            star.touch();
            std::vector<RenderVertex> rv;
            std::vector<uint32_t> ri;
            ms = timeMs([&] { buildRenderMesh(star, star.verts, false, 40, -1, rv, ri); });
            row("Render data of a 20000-corner star polygon", ms, ri.size() == (size_t)(n - 2) * 3);
            Mesh tri = star;
            ms = timeMs([&] { poly::triangulateMesh(tri, 4); });
            row("Triangulate it (ear clipping)", ms, tri.faces.size() == (size_t)(n - 2));
            meshedit::InsetParams ip;
            ip.thickness = 0.01f;
            std::vector<int> inner;
            std::vector<char> sel;
            Mesh ins = star;
            ms = timeMs([&] { meshedit::insetFaces(ins, {0}, ip, inner, sel); });
            row("Inset it", ms, meshedit::checkMesh(ins).empty());
            meshedit::BevelParams bp;
            bp.width = 0.002f;
            Mesh bev = primitives::uvSphere(1.0f, 64, 32);
            ms = timeMs([&] { meshedit::bevel(bev, {}, {0, 1, 2, 3, 4, 5, 6, 7}, bp, inner, sel); });
            row("Bevel 8 pole vertices of a sphere", ms, meshedit::checkMesh(bev).empty());
            stressText_ += "\n## G. Extreme values\n\n| Case | ms | Result |\n|---|---:|---|\n" + rows;
            clearForBenchmark();
            return false;
        },
        nullptr, 0, nullptr);
}

void Editor::stressTick() {
    if (!stressing_) return;
    if (stressW_ == 0 && screenW_ > 0) stressRealW_ = screenW_, stressRealH_ = screenH_;
    auto now = std::chrono::steady_clock::now();
    if (stressStepActive_) {
        StressStep& s = stressSteps_[stressIndex_];
        if (stressFrame_ >= 0) {
            double ms = std::chrono::duration<double, std::milli>(now - stressLast_).count();
            stressTimes_.push_back(ms);
            if (fuzzInput_) fuzzFrameMs_.push_back(ms);
        }
        if (stressFrame_ + 1 >= s.frames) {
            double first = stressTimes_.empty() ? 0 : stressTimes_[0];
            double sum = 0, worst = 0;
            int count = 0;
            for (size_t i = 1; i < stressTimes_.size(); ++i) sum += stressTimes_[i], worst = std::max(worst, stressTimes_[i]), ++count;
            if (count == 0 && !stressTimes_.empty()) sum = worst = first, count = 1;
            if (s.done) s.done(first, count ? sum / count : 0, worst);
            stressStepActive_ = false;
            ++stressIndex_;
        } else {
            ++stressFrame_;
            if (s.perFrame) s.perFrame(stressFrame_);
            stressLast_ = std::chrono::steady_clock::now();
            return;
        }
    }
    // Next step(s): run setups until one wants frames.
    while (stressIndex_ < stressSteps_.size()) {
        StressStep& s = stressSteps_[stressIndex_];
        std::fprintf(stdout, "[stress] %s  (%.0f MB)\n", s.name.c_str(), platform::memoryMB());
        std::fflush(stdout);
        // Partial report after every step: a hang or crash still leaves the data so far.
        if (FILE* f = std::fopen(stressReport_.c_str(), "wb")) {
            std::fwrite(stressText_.data(), 1, stressText_.size(), f);
            std::fclose(f);
        }
        stressTimes_.clear();
        if (s.setup && s.setup() && s.frames > 0) {
            stressStepActive_ = true;
            stressFrame_ = 0;
            if (s.perFrame) s.perFrame(0);
            stressLast_ = std::chrono::steady_clock::now();
            return;
        }
        ++stressIndex_;
    }
    stressFinish();
}

void Editor::stressFinish() {
    stressing_ = false;
    std::remove("stress_tmp.m3d");
    std::remove("stress_tmp.obj");
    std::remove("stress_tmp.mtl");
    std::remove("stress_tmp_render.png");
    std::remove("stress_tmp_render.png.txt");
    const GLubyte* renderer = gl::GetString(GL_RENDERER);
    std::string header = "# Modeler3D stress test\n\n";
    header += strf("GPU: %s, %d CPU threads, window %dx%d. Frame times include the GPU (glFinish every frame).\n\n",
                   renderer ? (const char*)renderer : "?", jobs::hardwareThreads(), stressRealW_, stressRealH_);
    header += strf("**Failures: %d** (invalid meshes, failed checks, problems found while fuzzing).\n", stressFailures_);
    std::string all = header + stressText_;
    if (FILE* f = std::fopen(stressReport_.c_str(), "wb")) {
        std::fwrite(all.data(), 1, all.size(), f);
        std::fclose(f);
    }
    std::fwrite(all.data(), 1, all.size(), stdout);
    exit_ = true;
}
