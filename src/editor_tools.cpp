// Edit-mode modeling tools built on meshtools.h: bevel, loop cut, subdivide,
// connect, poke, bridge, fill, merge, push through, loop / ring selection,
// grow / shrink, and Join in object mode.
//
// Every tool that has settings becomes the adjustable "last operation":
// the mesh before the tool and the selection it ran on are kept, so changing
// a value in the Last operation panel (or with the mouse while the tool is
// modal) re-runs it from scratch - the way Blender's "Adjust last operation"
// works.
#include "editor_internal.h"
#include "profiler.h"
#include "skin.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

using namespace ed;

std::vector<meshedit::Edge> Editor::editEdgeList() {
    std::vector<meshedit::Edge> edges;
    Object* o = activeMesh();
    if (!o) return edges;
    syncEditSelection();
    if (selMode_ == SelMode::Edge) return esel_;
    if (selMode_ == SelMode::Face) {
        std::unordered_set<uint64_t> seen;
        for (int f = 0; f < (int)fsel_.size(); ++f) {
            if (!fsel_[f]) continue;
            const auto& face = o->mesh.faces[f];
            for (size_t i = 0; i < face.size(); ++i) {
                meshedit::Edge e = meshedit::makeEdge(face[i], face[(i + 1) % face.size()]);
                if (seen.insert((uint64_t(uint32_t(e.first)) << 32) | uint32_t(e.second)).second) edges.push_back(e);
            }
        }
        return edges;
    }
    return meshedit::edgesFromVerts(o->mesh, vertSel());
}

void Editor::selectResult(const std::vector<int>& newFaces, const std::vector<char>& vsel) {
    Object* o = activeMesh();
    if (!o) return;
    vsel_ = vsel;
    vsel_.resize(o->mesh.verts.size(), 0);
    selectionFromVertices();
    if (selMode_ == SelMode::Face && !newFaces.empty()) {
        std::fill(fsel_.begin(), fsel_.end(), 0);
        for (int f : newFaces)
            if (f >= 0 && f < (int)fsel_.size()) fsel_[f] = 1;
        verticesFromSelection();
    }
}

bool Editor::beginMeshOp(int type) {
    if (mode_ != Mode::Edit) {
        setStatus("This tool works in Edit mode (Tab)", true);
        return false;
    }
    Object* o = activeMesh();
    if (!o) return false;
    syncEditSelection();
    pushUndo();
    lastOp_ = MeshOp();
    lastOp_.type = (MeshOp::Type)type;
    lastOp_.objectId = o->id;
    lastOp_.before = o->mesh;
    lastOp_.faces = editFaceList();
    lastOp_.edges = editEdgeList();
    auto& sel = vertSel();
    for (int v = 0; v < (int)sel.size(); ++v)
        if (sel[v]) lastOp_.verts.push_back(v);
    lastOp_.resultMode = selMode_;
    return true;
}

// Re-runs the last operation on the saved mesh with its current settings.
void Editor::applyLastOp() {
    if (lastOp_.type == MeshOp::None) return;
    int i = scene_.indexOf(lastOp_.objectId);
    if (i < 0) return;
    PROF_SCOPE("mesh tool");
    Object& o = scene_.objects[i];
    o.mesh = lastOp_.before;
    std::vector<int> newFaces;
    std::vector<char> vsel;
    std::string error;
    bool ok = true;
    switch (lastOp_.type) {
        case MeshOp::Inset:
            ok = meshedit::insetFaces(o.mesh, lastOp_.faces, lastOp_.inset, newFaces, vsel);
            if (selMode_ == SelMode::Edge) selMode_ = SelMode::Face;
            break;
        case MeshOp::Bevel:
            if (selMode_ == SelMode::Vertex && lastOp_.edges.empty())
                ok = meshedit::bevel(o.mesh, {}, lastOp_.verts, lastOp_.bevel, newFaces, vsel);
            else if (selMode_ == SelMode::Vertex)
                ok = meshedit::bevel(o.mesh, {}, lastOp_.verts, lastOp_.bevel, newFaces, vsel);
            else
                ok = meshedit::bevel(o.mesh, lastOp_.edges, {}, lastOp_.bevel, newFaces, vsel);
            break;
        case MeshOp::LoopCut:
            ok = !lastOp_.edges.empty() && meshedit::loopCut(o.mesh, lastOp_.edges[0], lastOp_.cuts, vsel);
            selMode_ = SelMode::Edge;
            break;
        case MeshOp::Subdivide:
            ok = meshedit::subdivideEdges(o.mesh, lastOp_.edges, lastOp_.cuts, true, vsel);
            break;
        case MeshOp::Bridge:
            ok = lastOp_.regions ? meshedit::bridgeRegions(o.mesh, lastOp_.faces, lastOp_.bridge, newFaces, vsel, error)
                                 : meshedit::bridgeLoops(o.mesh, lastOp_.edges, lastOp_.bridge, newFaces, vsel, error);
            break;
        case MeshOp::PushThrough:
            ok = meshedit::pushThrough(o.mesh, lastOp_.faces, lastOp_.push, newFaces, vsel, error);
            break;
        case MeshOp::Poke:
            ok = meshedit::pokeFaces(o.mesh, lastOp_.faces, lastOp_.offset, newFaces, vsel);
            break;
        default:
            break;
    }
    lastOp_.error = error;
    if (!ok) {
        o.mesh = lastOp_.before;
        vsel_.assign(o.mesh.verts.size(), 0);
        for (int v : lastOp_.verts)
            if (v < (int)vsel_.size()) vsel_[v] = 1;
        selTopology_ = 0;
        syncEditSelection();
        if (!error.empty()) setStatus(error, true);
    } else {
        selectResult(newFaces, vsel);
        // A setting that fixed an earlier refusal: replace the error message.
        if (statusError_) {
            static const char* const kDone[] = {"", "Inset", "Bevel", "Loop cut", "Subdivide", "Bridge", "Pushed through",
                                                "Poke"};
            setStatus(std::string(kDone[lastOp_.type]) + " - done");
        }
    }
    lastOp_.resultVersion = o.mesh.version;
    markDirty();
}

bool Editor::lastOpAdjustable() {
    if (lastOp_.type == MeshOp::None || mode_ != Mode::Edit) return false;
    int i = scene_.indexOf(lastOp_.objectId);
    return i >= 0 && i == scene_.active && scene_.objects[i].mesh.version == lastOp_.resultVersion;
}

// ---------------------------------------------------------------------------
// Inset / bevel: modal (the mouse sets the width), then adjustable
// ---------------------------------------------------------------------------
void Editor::insetSelected(bool interactive) {
    Object* o = activeMesh();
    if (!o || mode_ != Mode::Edit) {
        setStatus("Inset works in Edit mode (Tab)", true);
        return;
    }
    if (editFaceList().empty()) {
        setStatus("Select faces to inset", true);
        return;
    }
    if (!beginMeshOp(MeshOp::Inset)) return;
    lastOp_.inset = insetDefaults_;
    if (interactive) {
        lastOp_.inset.thickness = 0.0f;
        lastOp_.inset.depth = 0.0f;
        modalOp_ = MeshOp::Inset;
        insetModal_ = true;
        Vec3 centre;
        for (int f : lastOp_.faces) centre += faceCenter(o->mesh, o->mesh.faces[f]);
        centre = transformPoint(scene_.world(scene_.active), centre / (float)lastOp_.faces.size());
        worldToScreen(centre, insetCenter_);
        insetStartMouse_ = mouse_;
        insetWorldPerPixel_ = worldPerPixel(centre);
        setStatus("Inset: move towards the centre (Ctrl: depth). Click / Enter confirms, Esc cancels");
    }
    applyLastOp();
}

void Editor::bevelSelected(bool interactive) {
    Object* o = activeMesh();
    if (!o || mode_ != Mode::Edit) {
        setStatus("Bevel works in Edit mode (Tab)", true);
        return;
    }
    syncEditSelection();
    const bool vertexBevel = selMode_ == SelMode::Vertex;
    if (vertexBevel ? countEditSelection() == 0 : editEdgeList().empty()) {
        setStatus(vertexBevel ? "Select vertices to bevel (or edges in edge mode)" : "Select edges or faces to bevel", true);
        return;
    }
    if (!beginMeshOp(MeshOp::Bevel)) return;
    if (vertexBevel) lastOp_.edges.clear();
    lastOp_.bevel = bevelDefaults_;
    if (interactive) {
        lastOp_.bevel.width = 0.0001f;
        modalOp_ = MeshOp::Bevel;
        insetModal_ = true;
        Vec3 centre;
        int n = 0;
        for (int v : lastOp_.verts) centre += o->mesh.verts[v], ++n;
        centre = transformPoint(scene_.world(scene_.active), n ? centre / (float)n : Vec3());
        worldToScreen(centre, insetCenter_);
        insetStartMouse_ = mouse_;
        insetWorldPerPixel_ = worldPerPixel(centre);
        setStatus("Bevel: move the mouse away from the selection; wheel or +/- = segments. Click / Enter confirms");
    }
    applyLastOp();
}

bool Editor::updateInsetModal(const Input& in) {
    if (!insetModal_) return false;
    const bool bevelling = modalOp_ == MeshOp::Bevel;
    if (in.pressed(KEY_ESCAPE) || in.mousePressed[MOUSE_RIGHT]) {
        insetModal_ = false;
        modalOp_ = 0;
        int i = scene_.indexOf(lastOp_.objectId);
        if (i >= 0) scene_.objects[i].mesh = lastOp_.before;
        if (!undo_.empty()) undo_.pop_back();
        lastOp_ = MeshOp();
        selTopology_ = 0;
        syncEditSelection();
        setStatus(bevelling ? "Bevel cancelled" : "Inset cancelled");
        return true;
    }
    if (in.mousePressed[MOUSE_LEFT] || in.pressed(KEY_ENTER)) {
        insetModal_ = false;
        modalOp_ = 0;
        if (bevelling) {
            bevelDefaults_ = lastOp_.bevel;
            setStatus(strf("Bevel %.3f, %d segment(s) - fine-tune it in Last operation", lastOp_.bevel.width,
                           lastOp_.bevel.segments));
        } else {
            insetDefaults_.thickness = lastOp_.inset.thickness;
            setStatus(strf("Inset %.3f, depth %.3f - fine-tune it in Last operation", lastOp_.inset.thickness,
                           lastOp_.inset.depth));
        }
        return true;
    }
    const float startDist = length(insetStartMouse_ - insetCenter_);
    const float dist = length(mouse_ - insetCenter_);
    if (bevelling) {
        int segs = lastOp_.bevel.segments;
        if (in.wheel > 0 || in.pressed('=')) ++segs;
        if (in.wheel < 0 || in.pressed('-')) --segs;
        lastOp_.bevel.segments = std::max(1, std::min(segs, 32));
        lastOp_.bevel.width = std::max(1e-4f, (dist - startDist * 0.5f) * insetWorldPerPixel_);
    } else {
        const float amount = std::max(0.0f, (startDist - dist)) * insetWorldPerPixel_;
        if (in.ctrl()) lastOp_.inset.depth = -(mouse_.y - insetStartMouse_.y) * insetWorldPerPixel_;
        else lastOp_.inset.thickness = amount;
    }
    applyLastOp();
    return true;
}

// ---------------------------------------------------------------------------
// Loop cut, subdivide, connect, poke
// ---------------------------------------------------------------------------
void Editor::loopCutAt(bool underMouse) {
    Object* o = activeMesh();
    if (!o || mode_ != Mode::Edit) {
        setStatus("Loop cut works in Edit mode (Tab)", true);
        return;
    }
    meshedit::Edge e{-1, -1};
    if (underMouse && viewport_.contains(mouse_.x + viewport_.x, mouse_.y + viewport_.y)) pickEdge(mouse_, e);
    if (e.first < 0) {
        auto edges = editEdgeList();
        if (!edges.empty()) e = edges[0];
    }
    if (e.first < 0) {
        setStatus("Loop cut: point at an edge (or select one) - the cut runs across it", true);
        return;
    }
    if (!beginMeshOp(MeshOp::LoopCut)) return;
    lastOp_.edges = {e};
    lastOp_.cuts = 1;
    applyLastOp();
    setStatus("Loop cut - set the number of cuts in Last operation");
}

void Editor::subdivideEdit() {
    if (editEdgeList().empty()) {
        setStatus("Select edges or faces to subdivide", true);
        return;
    }
    if (!beginMeshOp(MeshOp::Subdivide)) return;
    lastOp_.cuts = 1;
    applyLastOp();
}

void Editor::connectSelected() {
    Object* o = activeMesh();
    if (!o || mode_ != Mode::Edit) return;
    syncEditSelection();
    pushUndo();
    int n = meshedit::connectVertices(o->mesh, vertSel());
    if (!n) {
        undo_.pop_back();
        setStatus("Connect: select two (or more) vertices of the same face that are not already joined", true);
        return;
    }
    lastOp_ = MeshOp();
    selTopology_ = 0;
    syncEditSelection();
    markDirty();
    setStatus(strf("Connected vertices across %d face(s)", n));
}

void Editor::pokeSelected() {
    if (editFaceList().empty()) {
        setStatus("Select faces to poke", true);
        return;
    }
    if (!beginMeshOp(MeshOp::Poke)) return;
    applyLastOp();
}

// ---------------------------------------------------------------------------
// Bridge, fill, merge, push through
// ---------------------------------------------------------------------------
void Editor::bridgeSelected() {
    if (mode_ != Mode::Edit || !activeMesh()) {
        setStatus("Bridge works in Edit mode: select two edge loops, or two groups of faces", true);
        return;
    }
    syncEditSelection();
    const bool regions = selMode_ == SelMode::Face || (selMode_ == SelMode::Vertex && !editFaceList().empty());
    if (!beginMeshOp(MeshOp::Bridge)) return;
    lastOp_.regions = regions;
    applyLastOp();
    if (!lastOp_.error.empty()) {
        // Failed: drop the undo step and the (unchanged) last operation.
        if (!undo_.empty()) undo_.pop_back();
        lastOp_ = MeshOp();
    } else {
        setStatus("Bridged - add segments or twist it in Last operation");
    }
}

void Editor::fillSelected() {
    Object* o = activeMesh();
    if (!o || mode_ != Mode::Edit) return;
    auto edges = editEdgeList();
    pushUndo();
    std::vector<int> faces;
    std::string err;
    int n = meshedit::fillHoles(o->mesh, edges, faces, err);
    if (!n) {
        undo_.pop_back();
        setStatus(err.empty() ? "Fill: select a border loop" : err, true);
        return;
    }
    lastOp_ = MeshOp();
    std::vector<char> vsel;
    vsel.assign(o->mesh.verts.size(), 0);
    for (int f : faces)
        for (int v : o->mesh.faces[f]) vsel[v] = 1;
    selTopology_ = 0;
    syncEditSelection();
    selectResult(faces, vsel);
    markDirty();
    setStatus(strf("Filled %d hole(s)", n));
}

void Editor::mergeSelected(bool perGroup) {
    Object* o = activeMesh();
    if (!o || mode_ != Mode::Edit) return;
    syncEditSelection();
    pushUndo();
    int removed = meshedit::mergeVertices(o->mesh, vertSel(), perGroup);
    if (!removed) {
        undo_.pop_back();
        setStatus("Merge: select two or more vertices", true);
        return;
    }
    lastOp_ = MeshOp();
    vsel_.assign(o->mesh.verts.size(), 0);
    selTopology_ = 0;
    syncEditSelection();
    markDirty();
    setStatus(strf("Merged: %d vertices removed", removed));
}

void Editor::pushThroughSelected() {
    if (editFaceList().empty()) {
        setStatus("Push through: select the faces to punch through (face mode)", true);
        return;
    }
    if (!beginMeshOp(MeshOp::PushThrough)) return;
    lastOp_.push.inset = 0.0f;
    applyLastOp();
    if (!lastOp_.error.empty()) {
        // Keep it adjustable: an inset > 0 often fixes "whole side" / "touches the edge".
        setStatus(lastOp_.error + " - try Inset in Last operation", true);
    } else {
        setStatus("Pushed through - Inset in Last operation leaves a frame");
    }
}

// ---------------------------------------------------------------------------
// Selection helpers
// ---------------------------------------------------------------------------
void Editor::selectLoopRing(bool ring, bool underMouse, bool extend) {
    Object* o = activeMesh();
    if (!o || mode_ != Mode::Edit) return;
    syncEditSelection();
    meshedit::Edge e{-1, -1};
    if (underMouse) pickEdge(mouse_, e);
    if (e.first < 0 && !esel_.empty()) e = esel_.back();
    if (e.first < 0) {
        auto edges = editEdgeList();
        if (!edges.empty()) e = edges.back();
    }
    if (e.first < 0) {
        setStatus(ring ? "Select ring: pick an edge first" : "Select loop: pick an edge first", true);
        return;
    }
    auto edges = ring ? meshedit::edgeRing(o->mesh, e) : meshedit::edgeLoop(o->mesh, e);
    if (selMode_ != SelMode::Edge) selMode_ = SelMode::Edge;
    if (!extend) esel_.clear();
    for (const auto& x : edges)
        if (std::find(esel_.begin(), esel_.end(), x) == esel_.end()) esel_.push_back(x);
    std::sort(esel_.begin(), esel_.end());
    verticesFromSelection();
    setStatus(strf("%s: %d edges", ring ? "Edge ring" : "Edge loop", (int)edges.size()));
}

void Editor::growSelection(bool grow) {
    Object* o = activeMesh();
    if (!o || mode_ != Mode::Edit) return;
    syncEditSelection();
    auto& sel = vertSel();
    std::vector<char> next = sel;
    for (const auto& f : o->mesh.faces) {
        bool any = false, all = true;
        for (int v : f) any = any || sel[v], all = all && sel[v];
        if (grow && any)
            for (int v : f) next[v] = 1;
        if (!grow && any && !all)
            for (int v : f) next[v] = 0;
    }
    sel = next;
    selectionFromVertices();
    if (selMode_ != SelMode::Vertex) verticesFromSelection();
    setStatus(strf("%d selected", countEditSelection()));
}

// ---------------------------------------------------------------------------
// Object mode: join, cycle through objects (keyboard selection)
// ---------------------------------------------------------------------------
void Editor::joinSelected() {
    if (mode_ != Mode::Object) {
        setStatus("Join works in Object mode", true);
        return;
    }
    Object* target = activeMesh();
    if (!target || !target->selected) {
        setStatus("Join: select meshes, the active one (last clicked) receives the others", true);
        return;
    }
    std::vector<int> others;
    for (int i = 0; i < (int)scene_.objects.size(); ++i)
        if (i != scene_.active && scene_.objects[i].selected && scene_.objects[i].isMesh()) others.push_back(i);
    if (others.empty()) {
        setStatus("Join: select two or more meshes", true);
        return;
    }
    pushUndo();
    const uint32_t targetId = target->id;
    makeEditable(*target);
    if (!target->skinBones.empty()) unbindSkin(*target);
    const Mat4 toTarget = inverse(scene_.world(scene_.active));
    std::vector<uint32_t> ids;
    for (int i : others) {
        Object& src = scene_.objects[i];
        makeEditable(src);
        Mesh m = src.mesh;
        if (!src.skinBones.empty()) m.weights.clear();
        meshedit::appendMesh(scene_.objects[scene_.indexOf(targetId)].mesh, m, toTarget * scene_.world(i));
        ids.push_back(src.id);
    }
    // Children of the joined objects move up to the nearest surviving ancestor.
    std::unordered_set<uint32_t> gone(ids.begin(), ids.end());
    auto isGone = [&](int i) { return i >= 0 && gone.count(scene_.objects[i].id) > 0; };
    for (int i = 0; i < (int)scene_.objects.size(); ++i) {
        if (isGone(i)) continue;
        int p = scene_.parentIndex(i), original = p;
        while (isGone(p)) p = scene_.parentIndex(p);
        if (p != original) scene_.setParent(i, p, true);
    }
    scene_.objects.erase(std::remove_if(scene_.objects.begin(), scene_.objects.end(),
                                        [&](const Object& o) { return gone.count(o.id) > 0; }),
                         scene_.objects.end());
    int t = scene_.indexOf(targetId);
    selectOnly(t);
    markDirty();
    setStatus(strf("Joined %d object(s) into %s", (int)ids.size(), scene_.objects[t].name.c_str()));
}

void Editor::cycleObject(int dir) {
    const int n = (int)scene_.objects.size();
    if (n == 0 || mode_ != Mode::Object) return;
    int i = scene_.active < 0 ? (dir > 0 ? 0 : n - 1) : ((scene_.active + dir) % n + n) % n;
    selectOnly(i);
    setStatus("Selected " + scene_.objects[i].name + "  ([ and ] cycle, F frames)");
}
