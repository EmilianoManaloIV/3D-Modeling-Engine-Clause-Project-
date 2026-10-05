// Boolean (CSG) operations and the n-gon solver / clean-up tools.
#include "csg.h"
#include "editor_internal.h"
#include "polygon.h"
#include "profiler.h"
#include "skin.h"

#include <algorithm>
#include <chrono>

using namespace ed;

namespace {
// World-space copy of a mesh object (posed if skinned).
Mesh worldMesh(const Scene& s, int i, std::vector<Vec3>& scratch) {
    Mat4 model;
    const std::vector<Vec3>& pos = evaluateMesh(s, i, false, scratch, model);
    Mesh m = s.objects[i].mesh;
    m.weights.clear();
    for (size_t v = 0; v < m.verts.size(); ++v) m.verts[v] = transformPoint(model, pos[v]);
    return m;
}
}  // namespace

void Editor::booleanSelected(csg::Op op) {
    if (xf_ != Xform::None) return;
    if (mode_ == Mode::Edit) setMode(Mode::Object);
    Object* target = activeMesh();
    std::vector<int> cutters;
    for (int i = 0; i < (int)scene_.objects.size(); ++i)
        if (i != scene_.active && scene_.objects[i].selected && scene_.objects[i].isMesh()) cutters.push_back(i);
    if (!target || cutters.empty()) {
        setStatus("Boolean: select the cutter mesh(es), then the target last (the active object)", true);
        return;
    }
    auto t0 = std::chrono::steady_clock::now();
    pushUndo();
    makeEditable(*target);
    Mesh result = worldMesh(scene_, scene_.active, scratch_);
    int polysIn = 0, polysOut = 0;
    for (int c : cutters) {
        csg::Result r = csg::apply(result, worldMesh(scene_, c, scratch_), op);
        polysIn += r.inputPolygons;
        polysOut = r.outputPolygons;
        result = std::move(r.mesh);
    }
    poly::CleanupStats st = poly::cleanup(result);
    int split = 0;
    if (csgTriangulate_) split = poly::triangulateMesh(result, 4);
    else split = poly::triangulateMesh(result, 64);  // only truly huge polygons

    // Back into the target's local space.
    Mat4 toLocal = inverse(scene_.world(scene_.active));
    for (Vec3& v : result.verts) v = transformPoint(toLocal, v);
    result.touch();
    target = &scene_.objects[scene_.active];
    target->mesh = std::move(result);
    target->skinBones.clear();
    target->bindInverse.clear();
    const uint32_t targetId = target->id;

    if (!csgKeepCutters_) {
        for (int c : cutters) scene_.objects[c].selected = true;
        target->selected = false;
        for (int i = 0; i < (int)scene_.objects.size(); ++i) {
            if (scene_.objects[i].selected) continue;
            int p = scene_.parentIndex(i), original = p;
            while (p >= 0 && scene_.objects[p].selected) p = scene_.parentIndex(p);
            if (p != original) scene_.setParent(i, p, true);
        }
        scene_.objects.erase(std::remove_if(scene_.objects.begin(), scene_.objects.end(),
                                            [](const Object& o) { return o.selected; }),
                             scene_.objects.end());
    } else {
        for (int c : cutters) scene_.objects[c].selected = false;
    }
    int ti = scene_.indexOf(targetId);
    if (ti >= 0) selectOnly(ti);
    markDirty();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const Mesh& m = scene_.objects[ti].mesh;
    setStatus(strf("Boolean %s: %d -> %d polygons, welded %d, fixed %d T-junctions, %d faces / %d tris (%.0f ms)",
                   csg::opName(op), polysIn, polysOut, st.weldedVertices, st.tJunctionsFixed, (int)m.faces.size(),
                   (int)m.triangleCount(), ms));
    (void)split;
}

void Editor::meshCleanupSelected(int tool) {
    if (xf_ != Xform::None) return;
    std::vector<int> targets;
    for (int i = 0; i < (int)scene_.objects.size(); ++i)
        if (scene_.objects[i].isMesh() && (mode_ == Mode::Edit ? i == scene_.active : scene_.objects[i].selected))
            targets.push_back(i);
    if (targets.empty()) {
        setStatus("Select a mesh first", true);
        return;
    }
    pushUndo();
    int count = 0;
    poly::CleanupStats total;
    for (int i : targets) {
        Object& o = scene_.objects[i];
        makeEditable(o);
        switch (tool) {
            case 0: count += poly::triangulateMesh(o.mesh, 5); break;
            case 1: count += poly::triangulateMesh(o.mesh, 4); break;
            case 2: {
                bool skinned = o.mesh.hasWeights();
                poly::CleanupStats st = poly::cleanup(o.mesh);
                total.weldedVertices += st.weldedVertices;
                total.tJunctionsFixed += st.tJunctionsFixed;
                total.degenerateFaces += st.degenerateFaces;
                if (skinned && !o.mesh.hasWeights()) {
                    o.skinBones.clear();
                    o.bindInverse.clear();
                }
                break;
            }
            default: count += poly::trisToQuads(o.mesh); break;
        }
        o.mesh.touch();
    }
    if (mode_ == Mode::Edit && activeMesh()) vsel_.assign(activeMesh()->mesh.verts.size(), 0);
    markDirty();
    switch (tool) {
        case 0: setStatus(strf("N-gon solver: split %d n-gon(s) into triangles", count)); break;
        case 1: setStatus(strf("Triangulated %d face(s)", count)); break;
        case 2:
            setStatus(strf("Clean up: welded %d vertices, fixed %d T-junctions, removed %d degenerate faces",
                           total.weldedVertices, total.tJunctionsFixed, total.degenerateFaces));
            break;
        default: setStatus(strf("Merged %d triangle pair(s) into quads", count)); break;
    }
}
