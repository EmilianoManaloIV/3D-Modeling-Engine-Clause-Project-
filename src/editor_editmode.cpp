// Edit mode: vertex / edge / face selection, picking, edge extrusion, inset
// (interactive, then adjustable as the "last operation").
//
// The vertex selection (vsel_) stays the canonical set that transforms and
// most tools use. In edge and face mode, the explicit edge / face selection
// is the source of truth and the vertex set is derived from it, so selecting
// two faces never implicitly selects a third face whose corners happen to be
// selected (the same model Blender uses).
#include "editor_internal.h"
#include "profiler.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

using namespace ed;

namespace {
uint64_t edgeKey64(const meshedit::Edge& e) { return (uint64_t(uint32_t(e.first)) << 32) | uint32_t(e.second); }
}  // namespace

const MeshAccel& Editor::meshAccel(const Object& o) {
    MeshAccel& a = meshAccel_[o.id];
    if (a.key != o.mesh.version) {
        PROF_SCOPE("pick bvh build");
        a.build(o.mesh, o.mesh.verts);
        a.key = o.mesh.version;
    }
    return a;
}

void Editor::syncEditSelection() {
    Object* o = activeMesh();
    if (!o) return;
    auto& vs = vertSel();
    if (selTopology_ == o->mesh.topology && selObject_ == o->id && fsel_.size() == o->mesh.faces.size()) return;
    // Topology changed (undo, a tool, another object): rebuild from vertices.
    fsel_ = meshedit::facesFromVerts(o->mesh, vs);
    esel_ = meshedit::edgesFromVerts(o->mesh, vs);
    selTopology_ = o->mesh.topology;
    selObject_ = o->id;
}

void Editor::selectionFromVertices() {
    Object* o = activeMesh();
    if (!o) return;
    fsel_ = meshedit::facesFromVerts(o->mesh, vertSel());
    esel_ = meshedit::edgesFromVerts(o->mesh, vertSel());
    selTopology_ = o->mesh.topology;
    selObject_ = o->id;
}

void Editor::verticesFromSelection() {
    Object* o = activeMesh();
    if (!o) return;
    if (selMode_ == SelMode::Edge) vsel_ = meshedit::vertsFromEdges(o->mesh, esel_);
    else if (selMode_ == SelMode::Face) vsel_ = meshedit::vertsFromFaces(o->mesh, fsel_);
}

void Editor::setSelMode(SelMode m) {
    if (m == selMode_) return;
    if (mode_ == Mode::Edit && activeMesh()) {
        syncEditSelection();
        // Like Blender: going "up" keeps only fully selected elements.
        selectionFromVertices();
        verticesFromSelection();
    }
    selMode_ = m;
    static const char* names[] = {"Vertex", "Edge", "Face"};
    setStatus(std::string(names[(int)m]) + " select mode");
}

std::vector<int> Editor::editFaceList() {
    std::vector<int> faces;
    Object* o = activeMesh();
    if (!o) return faces;
    syncEditSelection();
    if (selMode_ == SelMode::Face) {
        for (int f = 0; f < (int)fsel_.size(); ++f)
            if (fsel_[f]) faces.push_back(f);
    } else {
        faces = selectedFaces(o->mesh, vertSel());
    }
    return faces;
}

int Editor::countEditSelection() {
    syncEditSelection();
    if (selMode_ == SelMode::Edge) return (int)esel_.size();
    if (selMode_ == SelMode::Face) return (int)std::count(fsel_.begin(), fsel_.end(), 1);
    return (int)std::count(vertSel().begin(), vertSel().end(), 1);
}

// Screen-space nearest edge (within 10 px), skipping edges hidden behind the mesh.
int Editor::pickEdge(Vec2 p, meshedit::Edge& out) {
    Object* o = activeMesh();
    if (!o) return -1;
    if (editEdgesVersion_ != o->mesh.topology) {
        editEdges_ = uniqueEdges(o->mesh);
        editEdgesVersion_ = o->mesh.topology;
    }
    const Mat4 m = scene_.world(scene_.active), inv = inverse(m);
    const float radius = 10.0f * dpi_;
    std::vector<std::pair<float, int>> cand;
    for (int e = 0; e < (int)editEdges_.size(); ++e) {
        Vec2 a, b;
        if (!worldToScreen(transformPoint(m, o->mesh.verts[editEdges_[e].first]), a) ||
            !worldToScreen(transformPoint(m, o->mesh.verts[editEdges_[e].second]), b))
            continue;
        float d = distanceToSegment2D(p, a, b);
        if (d <= radius) cand.push_back({d, e});
    }
    std::sort(cand.begin(), cand.end());
    const MeshAccel& accel = meshAccel(*o);
    for (size_t c = 0; c < cand.size() && c < 32; ++c) {
        const auto& e = editEdges_[cand[c].second];
        Vec3 mid = transformPoint(m, (o->mesh.verts[e.first] + o->mesh.verts[e.second]) * 0.5f);
        if (!wireframe_) {
            Vec3 origin = cam_.ortho ? mid - camForward() * (cam_.distance * 4.0f + 100.0f) : cam_.eye();
            Vec3 dir = mid - origin;
            float tol = (0.002f * cam_.distance + 1e-4f) / std::max(1e-6f, length(dir));
            float t;
            if (accel.raycast(transformPoint(inv, origin), transformDir(inv, dir), t) && t < 1.0f - tol) continue;
        }
        out = e;
        return cand[c].second;
    }
    return -1;
}

int Editor::pickFace(Vec2 p) {
    Object* o = activeMesh();
    if (!o) return -1;
    Vec3 ro, rd;
    viewRay(p, ro, rd);
    const Mat4 inv = inverse(scene_.world(scene_.active));
    float t;
    int face = -1;
    if (meshAccel(*o).raycast(transformPoint(inv, ro), transformDir(inv, rd), t, &face)) return face;
    return -1;
}

void Editor::clickSelectEdit(Vec2 p, bool extend) {
    Object* o = activeMesh();
    if (!o) return;
    syncEditSelection();
    auto& sel = vertSel();
    if (selMode_ == SelMode::Vertex) {
        int v = pickVertex(p);
        if (!extend) std::fill(sel.begin(), sel.end(), 0);
        if (v >= 0) sel[v] = extend ? !sel[v] : 1;
        selectionFromVertices();
        return;
    }
    if (selMode_ == SelMode::Edge) {
        meshedit::Edge e;
        bool hit = pickEdge(p, e) >= 0;
        if (!extend) esel_.clear();
        if (hit) {
            auto it = std::find(esel_.begin(), esel_.end(), e);
            if (it != esel_.end() && extend) esel_.erase(it);
            else if (it == esel_.end()) esel_.push_back(e);
            // Double-click an edge: select its whole loop (Maya / 3ds Max style).
            if (e == lastEdgeClick_ && clock_ - lastEdgeClickTime_ < 0.4) {
                std::vector<meshedit::Edge> loop = meshedit::edgeLoop(o->mesh, e);
                if (!extend) esel_.clear();
                for (const auto& le : loop)
                    if (std::find(esel_.begin(), esel_.end(), le) == esel_.end()) esel_.push_back(le);
                std::sort(esel_.begin(), esel_.end());
                lastEdgeClickTime_ = -1;
                setStatus(strf("Edge loop: %d edges (double-click)", (int)loop.size()));
            } else {
                lastEdgeClick_ = e;
                lastEdgeClickTime_ = clock_;
            }
        }
    } else {
        int f = pickFace(p);
        if (!extend) std::fill(fsel_.begin(), fsel_.end(), 0);
        if (f >= 0) fsel_[f] = extend ? !fsel_[f] : 1;
    }
    verticesFromSelection();
}

void Editor::boxSelectEdit(Vec2 a, Vec2 b, bool extend, bool subtract) {
    Object* o = activeMesh();
    if (!o) return;
    syncEditSelection();
    const float x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
    const Mat4 m = scene_.world(scene_.active);
    std::vector<char> inside(o->mesh.verts.size(), 0);
    for (size_t v = 0; v < inside.size(); ++v) {
        Vec2 s;
        inside[v] = worldToScreen(transformPoint(m, o->mesh.verts[v]), s) && s.x >= x0 && s.x <= x1 && s.y >= y0 &&
                    s.y <= y1;
    }
    auto& sel = vertSel();
    if (selMode_ == SelMode::Vertex) {
        if (!extend && !subtract) std::fill(sel.begin(), sel.end(), 0);
        for (size_t v = 0; v < sel.size(); ++v)
            if (inside[v]) sel[v] = subtract ? 0 : 1;
        selectionFromVertices();
        return;
    }
    if (selMode_ == SelMode::Edge) {
        std::unordered_set<uint64_t> cur;
        if (extend || subtract)
            for (const auto& e : esel_) cur.insert(edgeKey64(e));
        for (const auto& e : uniqueEdges(o->mesh)) {
            if (!inside[e.first] || !inside[e.second]) continue;
            if (subtract) cur.erase(edgeKey64(e));
            else cur.insert(edgeKey64(e));
        }
        esel_.clear();
        for (uint64_t k : cur) esel_.push_back({(int)(k >> 32), (int)(k & 0xFFFFFFFFu)});
        std::sort(esel_.begin(), esel_.end());
    } else {
        if (!extend && !subtract) std::fill(fsel_.begin(), fsel_.end(), 0);
        for (size_t f = 0; f < fsel_.size(); ++f) {
            bool all = true;
            for (int v : o->mesh.faces[f]) all = all && inside[v];
            if (all) fsel_[f] = subtract ? 0 : 1;
        }
    }
    verticesFromSelection();
}

void Editor::selectAllEdit() {
    Object* o = activeMesh();
    if (!o) return;
    syncEditSelection();
    const bool any = countEditSelection() > 0;
    auto& sel = vertSel();
    std::fill(sel.begin(), sel.end(), any ? 0 : 1);
    selectionFromVertices();
}

// ---------------------------------------------------------------------------
// Extrude (faces, or edges in edge mode / when no whole face is selected)
// ---------------------------------------------------------------------------
void Editor::extrudeEdit() {
    Object* o = activeMesh();
    if (!o) return;
    syncEditSelection();
    std::vector<int> faces = editFaceList();
    if (selMode_ != SelMode::Edge && !faces.empty()) {
        pushUndo();
        Vec3 n;
        extrudeFaces(o->mesh, faces, vsel_, &n);
        selectionFromVertices();
        if (selMode_ == SelMode::Face) {
            std::fill(fsel_.begin(), fsel_.end(), 0);
            for (int f : faces) fsel_[f] = 1;  // the extruded caps keep their face indices
            verticesFromSelection();
        }
        markDirty();
        Mat4 normalMatrix = transpose(inverse(scene_.world(scene_.active)));
        xfCustomAxis_ = normalize(transformDir(normalMatrix, n));
        if (beginTransform(Xform::Grab, false) && dot(xfCustomAxis_, xfCustomAxis_) > 0.5f) xfAxis_ = 3;
        setStatus("Extruded faces - move the mouse, click to confirm");
        return;
    }
    std::vector<meshedit::Edge> edges = selMode_ == SelMode::Edge ? esel_ : meshedit::edgesFromVerts(o->mesh, vsel_);
    if (edges.empty()) {
        setStatus("Select faces or edges to extrude", true);
        return;
    }
    pushUndo();
    std::vector<meshedit::Edge> newEdges;
    if (!meshedit::extrudeEdges(o->mesh, edges, newEdges, vsel_)) {
        undo_.pop_back();
        setStatus("Nothing to extrude", true);
        return;
    }
    selectionFromVertices();
    esel_ = newEdges;
    std::sort(esel_.begin(), esel_.end());
    if (selMode_ == SelMode::Face) selMode_ = SelMode::Edge;  // new edges only make sense as edges
    verticesFromSelection();
    markDirty();
    xfCustomAxis_ = Vec3();
    beginTransform(Xform::Grab, false);
    setStatus(strf("Extruded %d edge(s) - move the mouse, click to confirm", (int)newEdges.size()));
}

// ---------------------------------------------------------------------------
// Delete in edit mode
// ---------------------------------------------------------------------------
void Editor::deleteEdit() {
    Object* o = activeMesh();
    if (!o) return;
    syncEditSelection();
    if (selMode_ == SelMode::Vertex) {
        auto& sel = vertSel();
        int count = (int)std::count(sel.begin(), sel.end(), 1);
        if (!count) {
            setStatus("No vertices selected", true);
            return;
        }
        pushUndo();
        deleteVertices(o->mesh, sel);
        markDirty();
        setStatus(strf("Deleted %d vertices", count));
        return;
    }
    std::vector<char> kill(o->mesh.faces.size(), 0);
    if (selMode_ == SelMode::Face) {
        kill = fsel_;
    } else {
        std::unordered_set<uint64_t> keys;
        for (const auto& e : esel_) keys.insert(edgeKey64(e));
        for (size_t f = 0; f < o->mesh.faces.size(); ++f) {
            const auto& face = o->mesh.faces[f];
            for (size_t i = 0; i < face.size() && !kill[f]; ++i)
                kill[f] = keys.count(edgeKey64(meshedit::makeEdge(face[i], face[(i + 1) % face.size()]))) > 0;
        }
    }
    int count = (int)std::count(kill.begin(), kill.end(), 1);
    if (!count) {
        setStatus("Nothing selected to delete", true);
        return;
    }
    pushUndo();
    Mesh& m = o->mesh;
    meshedit::deleteFaces(m, kill);
    vsel_.assign(m.verts.size(), 0);
    selectionFromVertices();
    markDirty();
    setStatus(strf("Deleted %d face(s)", count));
}

