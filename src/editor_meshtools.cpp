// Edit-mode topology tools in the editor: inset, bevel, loop cut, edge
// subdivision, bridge, push-in / punch-through and poke run as an adjustable
// "last operation" (re-run from a copy of the mesh whenever a setting changes,
// like Blender's Adjust Last Operation panel), optionally driven
// interactively with the mouse or by typing a value. Connect, fill, merge,
// loop / linked selection and joining objects are one-shot commands.
#include "editor_internal.h"
#include "profiler.h"

#include <algorithm>
#include <cmath>

using namespace ed;

const char* Editor::opName(OpType t) {
    switch (t) {
        case OpType::Inset: return "Inset";
        case OpType::Bevel: return "Bevel";
        case OpType::LoopCut: return "Loop cut";
        case OpType::Subdivide: return "Subdivide edges";
        case OpType::Bridge: return "Bridge";
        case OpType::Push: return "Push in";
        case OpType::Poke: return "Poke";
        default: return "";
    }
}

std::vector<meshedit::Edge> Editor::editEdgeList() {
    Object* o = activeMesh();
    if (!o) return {};
    syncEditSelection();
    if (selMode_ == SelMode::Edge) return esel_;
    return meshedit::edgesFromVerts(o->mesh, vertSel());
}

void Editor::setEditResult(const std::vector<int>* faces, const std::vector<meshedit::Edge>* edges) {
    selectionFromVertices();
    if (faces) {
        std::fill(fsel_.begin(), fsel_.end(), 0);
        for (int f : *faces)
            if (f >= 0 && f < (int)fsel_.size()) fsel_[f] = 1;
        if (selMode_ == SelMode::Edge) selMode_ = SelMode::Face;
        if (selMode_ == SelMode::Face) verticesFromSelection();
    }
    if (edges) {
        esel_ = *edges;
        std::sort(esel_.begin(), esel_.end());
        if (selMode_ == SelMode::Edge) verticesFromSelection();
    }
}

// ---------------------------------------------------------------------------
// Starting / re-running the last operation
// ---------------------------------------------------------------------------
bool Editor::startMeshOp(OpType type, bool interactive) {
    if (mode_ != Mode::Edit) {
        setStatus(std::string(opName(type)) + " works in Edit mode (Tab)", true);
        return false;
    }
    Object* o = activeMesh();
    if (!o || xf_ != Xform::None || opModal_) return false;
    syncEditSelection();
    MeshOp op;
    op.type = type;
    op.objectId = o->id;
    op.inset = insetDefaults_;
    op.bevel = bevelDefaults_;
    op.push = pushDefaults_;
    op.loop = lastOp_.loop;
    op.cuts = std::max(1, lastOp_.cuts);
    op.bridge = lastOp_.bridge;
    switch (type) {
        case OpType::Inset:
        case OpType::Push:
        case OpType::Poke:
            op.faces = editFaceList();
            if (op.faces.empty()) {
                setStatus(std::string(opName(type)) + ": select faces first (face mode: 3)", true);
                return false;
            }
            break;
        case OpType::Bevel:
            op.edges = editEdgeList();
            op.bevel.vertexOnly = op.edges.empty() && selMode_ == SelMode::Vertex;
            if (op.bevel.vertexOnly) op.verts = vertSel();
            if (op.edges.empty() && std::count(op.verts.begin(), op.verts.end(), 1) == 0) {
                setStatus("Bevel: select edges (or vertices to chamfer corners)", true);
                return false;
            }
            break;
        case OpType::LoopCut:
        case OpType::Subdivide:
            op.edges = editEdgeList();
            if (op.edges.empty()) {
                setStatus(type == OpType::LoopCut ? "Loop cut: select an edge - the cut runs across its ring of quads"
                                                  : "Subdivide: select the edges to split",
                          true);
                return false;
            }
            break;
        case OpType::Bridge:
            if (selMode_ == SelMode::Face) op.faces = editFaceList();
            else op.edges = editEdgeList();
            if (op.faces.empty() && op.edges.empty()) {
                setStatus("Bridge: select two face groups (face mode) or the border edges of two openings", true);
                return false;
            }
            break;
        default:
            return false;
    }
    pushUndo();
    op.before = o->mesh;
    lastOp_ = std::move(op);
    applyLastOp();
    if (!lastOp_.error.empty()) {
        std::string err = lastOp_.error;
        int i = scene_.indexOf(lastOp_.objectId);
        if (i >= 0) scene_.objects[i].mesh = lastOp_.before;
        if (!undo_.empty()) undo_.pop_back();
        lastOp_ = MeshOp();
        selectionFromVertices();
        setStatus(err, true);
        return false;
    }
    if (interactive && (type == OpType::Inset || type == OpType::Bevel || type == OpType::Push)) {
        opModal_ = true;
        opTyped_.clear();
        Vec3 centre;
        int n = 0;
        const Mesh& m = lastOp_.before;
        for (int f : lastOp_.faces) centre += faceCenter(m, m.faces[f]), ++n;
        for (const auto& e : lastOp_.edges) centre += (m.verts[e.first] + m.verts[e.second]) * 0.5f, ++n;
        for (size_t v = 0; v < lastOp_.verts.size(); ++v)
            if (lastOp_.verts[v]) centre += m.verts[v], ++n;
        centre = transformPoint(scene_.world(scene_.active), centre / (float)std::max(1, n));
        worldToScreen(centre, opCenter_);
        opStartMouse_ = mouse_;
        opWorldPerPixel_ = worldPerPixel(centre);
        if (type == OpType::Inset) {
            lastOp_.inset.thickness = 0;
            lastOp_.inset.depth = 0;
        } else if (type == OpType::Bevel) {
            lastOp_.bevel.width = 0;
        } else {
            lastOp_.push.depth = 0;
        }
        applyLastOp();
        static const char* hint[] = {"move the mouse towards the centre (Ctrl: depth)",
                                     "move the mouse away to widen; wheel or +/- = segments",
                                     "move the mouse down to push deeper; Ctrl: border width"};
        int h = type == OpType::Inset ? 0 : type == OpType::Bevel ? 1 : 2;
        setStatus(std::string(opName(type)) + ": " + hint[h] +
                  ". Or type a value. Click / Enter confirms, Esc / right-click cancels");
    } else {
        setStatus(std::string(opName(type)) + (lastOp_.info.empty() ? "" : " - " + lastOp_.info) +
                  " - adjust it in Model > Last operation");
    }
    return true;
}

void Editor::applyLastOp() {
    if (lastOp_.type == OpType::None) return;
    int i = scene_.indexOf(lastOp_.objectId);
    if (i < 0) return;
    PROF_SCOPE("mesh tool");
    Object& o = scene_.objects[i];
    o.mesh = lastOp_.before;
    lastOp_.error.clear();
    lastOp_.info.clear();
    std::vector<int> faces;
    std::vector<meshedit::Edge> edges;
    bool ok = false;
    std::string err;
    Mesh& m = o.mesh;
    switch (lastOp_.type) {
        case OpType::Inset:
            ok = meshedit::insetFaces(m, lastOp_.faces, lastOp_.inset, faces, vsel_);
            setEditResult(&faces, nullptr);
            break;
        case OpType::Bevel: {
            float used = 0;
            ok = meshedit::bevel(m, lastOp_.edges, lastOp_.verts, lastOp_.bevel, faces, vsel_, &used);
            if (!ok) err = lastOp_.bevel.width <= 0 && !opModal_ ? "Bevel: width is 0" : "Bevel: nothing to bevel (border edges can't be beveled)";
            if (ok && used + 1e-6f < lastOp_.bevel.width) lastOp_.info = strf("width clamped to %.3f", used);
            if (!ok && opModal_) {  // width 0 while dragging: show the untouched mesh
                o.mesh = lastOp_.before;
                vsel_.assign(o.mesh.verts.size(), 0);
                ok = true;
                err.clear();
            }
            setEditResult(&faces, nullptr);
            break;
        }
        case OpType::LoopCut:
            ok = meshedit::loopCut(m, lastOp_.edges, lastOp_.loop, edges, vsel_);
            if (!ok) err = "Loop cut: the selected edges have no quads on either side";
            if (selMode_ == SelMode::Face) selMode_ = SelMode::Edge;
            setEditResult(nullptr, &edges);
            break;
        case OpType::Subdivide:
            ok = meshedit::subdivideEdges(m, lastOp_.edges, lastOp_.cuts, edges, vsel_);
            setEditResult(nullptr, &edges);
            break;
        case OpType::Bridge:
            ok = lastOp_.faces.empty() ? meshedit::bridgeEdges(m, lastOp_.edges, lastOp_.bridge, faces, vsel_, err)
                                       : meshedit::bridgeFaces(m, lastOp_.faces, lastOp_.bridge, faces, vsel_, err);
            setEditResult(&faces, nullptr);
            break;
        case OpType::Push:
            ok = meshedit::pushIn(m, lastOp_.faces, lastOp_.push, faces, vsel_, err);
            if (ok && lastOp_.push.through) {
                vsel_.assign(m.verts.size(), 0);
                o.skinBones.clear();
                o.bindInverse.clear();
                lastOp_.info = strf("hole cut: %d faces", (int)m.faces.size());
            }
            setEditResult(&faces, nullptr);
            break;
        case OpType::Poke:
            ok = meshedit::pokeFaces(m, lastOp_.faces, lastOp_.poke, faces, vsel_) > 0;
            setEditResult(&faces, nullptr);
            break;
        default:
            break;
    }
    if (!ok) {
        lastOp_.error = err.empty() ? std::string(opName(lastOp_.type)) + " failed" : err;
        o.mesh = lastOp_.before;
        vsel_.assign(o.mesh.verts.size(), 0);
        selectionFromVertices();
    }
    lastOp_.resultVersion = o.mesh.version;
    markDirty();
}

bool Editor::lastOpAdjustable() {
    if (lastOp_.type == OpType::None || mode_ != Mode::Edit) return false;
    int i = scene_.indexOf(lastOp_.objectId);
    return i >= 0 && i == scene_.active && scene_.objects[i].mesh.version == lastOp_.resultVersion;
}

// ---------------------------------------------------------------------------
// Interactive (modal) inset / bevel / push
// ---------------------------------------------------------------------------
bool Editor::updateOpModal(const Input& in) {
    if (!opModal_) return false;
    const OpType t = lastOp_.type;
    if (in.pressed(KEY_ESCAPE) || in.mousePressed[MOUSE_RIGHT]) {
        opModal_ = false;
        int i = scene_.indexOf(lastOp_.objectId);
        if (i >= 0) scene_.objects[i].mesh = lastOp_.before;
        if (!undo_.empty()) undo_.pop_back();
        lastOp_ = MeshOp();
        vertSel();
        selectionFromVertices();
        setStatus(std::string(opName(t)) + " cancelled");
        return true;
    }
    // Typed value (works without a mouse): digits, '.', '-', Backspace.
    for (char ch : in.text)
        if ((ch >= '0' && ch <= '9') || ch == '.' || (ch == '-' && opTyped_.empty())) opTyped_ += ch;
    if (in.keyRepeat[KEY_BACKSPACE] && !opTyped_.empty()) opTyped_.pop_back();
    if (t == OpType::Bevel) {
        int seg = lastOp_.bevel.segments;
        if (in.wheel > 0 || in.text.find('+') != std::string::npos || in.text.find('=') != std::string::npos) ++seg;
        if (in.wheel < 0 || (in.text.find('-') != std::string::npos && !opTyped_.empty())) --seg;
        lastOp_.bevel.segments = std::max(1, std::min(seg, 32));
    }
    if (in.mousePressed[MOUSE_LEFT] || in.pressed(KEY_ENTER) || in.pressed(KEY_SPACE)) {
        opModal_ = false;
        if (t == OpType::Inset) insetDefaults_.thickness = lastOp_.inset.thickness;
        if (t == OpType::Bevel) bevelDefaults_ = lastOp_.bevel, bevelDefaults_.vertexOnly = false;
        if (t == OpType::Push) pushDefaults_ = lastOp_.push;
        std::string what = t == OpType::Inset  ? strf("width %.3f, depth %.3f", lastOp_.inset.thickness, lastOp_.inset.depth)
                           : t == OpType::Bevel ? strf("width %.3f, %d segment(s)", lastOp_.bevel.width, lastOp_.bevel.segments)
                                                : strf("depth %.3f, border %.3f", lastOp_.push.depth, lastOp_.push.width);
        setStatus(std::string(opName(t)) + " " + what + " - fine-tune it in Model > Last operation");
        return true;
    }
    float typed = 0;
    const bool hasTyped = !opTyped_.empty() && opTyped_ != "-" && opTyped_ != "." && (typed = std::strtof(opTyped_.c_str(), nullptr), true);
    const float k = opWorldPerPixel_;
    switch (t) {
        case OpType::Inset:
            if (in.ctrl()) lastOp_.inset.depth = hasTyped ? typed : -(mouse_.y - opStartMouse_.y) * k;
            else lastOp_.inset.thickness = hasTyped ? std::max(0.0f, typed)
                                                    : std::max(0.0f, length(opStartMouse_ - opCenter_) - length(mouse_ - opCenter_)) * k;
            break;
        case OpType::Bevel:
            lastOp_.bevel.width = hasTyped ? std::max(0.0f, typed) : length(mouse_ - opStartMouse_) * k * 0.5f;
            break;
        case OpType::Push:
            if (in.ctrl()) lastOp_.push.width = hasTyped ? std::max(0.0f, typed) : std::max(0.0f, (mouse_.x - opStartMouse_.x) * k);
            else lastOp_.push.depth = hasTyped ? typed : (mouse_.y - opStartMouse_.y) * k;
            break;
        default:
            break;
    }
    applyLastOp();
    if (hasTyped || !opTyped_.empty()) setStatus(std::string(opName(t)) + ": " + opTyped_ + "   (Enter confirms)");
    return true;
}

// ---------------------------------------------------------------------------
// Last-operation panel (left panel, Model workspace)
// ---------------------------------------------------------------------------
void Editor::lastOpPanel(PanelLayout& L) {
    if (!lastOpAdjustable()) return;
    const float fs = (float)fontScale_, rowH = ui_.rowHeight();
    ui_.header(L.row(11 * fs), std::string("LAST: ") + opName(lastOp_.type));
    bool changed = false;
    auto row = [&](const char* text, const char* key, float& v, float speed, float lo, float hi) {
        Rect r = L.row(rowH);
        float lw = std::floor(r.w * 0.45f);
        ui_.textIn({r.x, r.y, lw, r.h}, text, theme::textDim, false);
        if (ui_.dragFloat(uiHash(key), {r.x + lw, r.y, r.w - lw, r.h}, v, speed, theme::accent, lo, hi)) changed = true;
    };
    auto intRow = [&](const char* text, const char* key, int& v, int lo, int hi) {
        float f = (float)v;
        row(text, key, f, 0.05f, (float)lo, (float)hi);
        v = (int)std::lround(f);
    };
    auto toggle = [&](const char* key, const char* text, bool& v) {
        if (ui_.button(uiHash(key), L.row(rowH), text, v)) v = !v, changed = true;
    };
    switch (lastOp_.type) {
        case OpType::Inset: {
            meshedit::InsetParams& ip = lastOp_.inset;
            row("Width", "op.thick", ip.thickness, 0.002f, 0.0f, 1000.0f);
            row("Depth", "op.depth", ip.depth, 0.002f, -1000.0f, 1000.0f);
            row("Dish", "op.dish", ip.dish, 0.002f, -1000.0f, 1000.0f);
            toggle("op.ind", "Individual faces", ip.individual);
            if (changed) insetDefaults_ = ip;
            break;
        }
        case OpType::Bevel: {
            meshedit::BevelParams& bp = lastOp_.bevel;
            row("Width", "op.bw", bp.width, 0.002f, 0.0f, 1000.0f);
            intRow("Segments", "op.bseg", bp.segments, 1, 32);
            if (changed) bevelDefaults_.width = bp.width, bevelDefaults_.segments = bp.segments;
            break;
        }
        case OpType::LoopCut:
            intRow("Cuts", "op.lcuts", lastOp_.loop.cuts, 1, 64);
            if (lastOp_.loop.cuts == 1) row("Slide", "op.lslide", lastOp_.loop.slide, 0.005f, -1.0f, 1.0f);
            break;
        case OpType::Subdivide:
            intRow("Cuts", "op.scuts", lastOp_.cuts, 1, 64);
            break;
        case OpType::Bridge:
            intRow("Segments", "op.brseg", lastOp_.bridge.segments, 1, 64);
            intRow("Twist", "op.brtw", lastOp_.bridge.twist, -256, 256);
            break;
        case OpType::Push: {
            meshedit::PushParams& pp = lastOp_.push;
            row("Border", "op.pw", pp.width, 0.002f, 0.0f, 1000.0f);
            if (!pp.through) row("Depth", "op.pd", pp.depth, 0.002f, -1000.0f, 1000.0f);
            toggle("op.pt", "Punch through", pp.through);
            if (changed) pushDefaults_ = pp;
            break;
        }
        case OpType::Poke:
            row("Offset", "op.poke", lastOp_.poke, 0.002f, -1000.0f, 1000.0f);
            break;
        default:
            break;
    }
    if (!lastOp_.info.empty()) label(L, lastOp_.info, theme::textDim);
    if (!lastOp_.error.empty()) label(L, lastOp_.error, theme::error);
    if (changed) applyLastOp();
}

void Editor::insetSelected(bool interactive) { startMeshOp(OpType::Inset, interactive); }

// ---------------------------------------------------------------------------
// One-shot commands
// ---------------------------------------------------------------------------
void Editor::connectSelected() {
    Object* o = activeMesh();
    if (mode_ != Mode::Edit || !o) return setStatus("Connect works in Edit mode", true);
    syncEditSelection();
    pushUndo();
    std::vector<meshedit::Edge> edges;
    int n = meshedit::connectVertices(o->mesh, vertSel(), edges);
    if (!n) {
        undo_.pop_back();
        return setStatus("Connect: select two vertices of the same face that aren't already joined by an edge", true);
    }
    vsel_ = meshedit::vertsFromEdges(o->mesh, edges);
    if (selMode_ == SelMode::Face) selMode_ = SelMode::Edge;
    setEditResult(nullptr, &edges);
    markDirty();
    setStatus(strf("Connected: split %d face(s)", n));
}

void Editor::fillSelected() {
    Object* o = activeMesh();
    if (mode_ != Mode::Edit || !o) return setStatus("Fill works in Edit mode", true);
    std::vector<meshedit::Edge> edges = editEdgeList();
    pushUndo();
    std::vector<int> faces;
    std::string err;
    if (!meshedit::fillFace(o->mesh, edges, vertSel(), faces, vsel_, err)) {
        undo_.pop_back();
        return setStatus(err, true);
    }
    setEditResult(&faces, nullptr);
    markDirty();
    setStatus(strf("Filled: new %d-sided face", (int)o->mesh.faces[faces[0]].size()));
}

void Editor::mergeSelected() {
    Object* o = activeMesh();
    if (mode_ != Mode::Edit || !o) return setStatus("Merge works in Edit mode", true);
    syncEditSelection();
    pushUndo();
    int n = meshedit::mergeAtCenter(o->mesh, vertSel());
    if (!n) {
        undo_.pop_back();
        return setStatus("Merge: select two or more vertices", true);
    }
    selectionFromVertices();
    markDirty();
    setStatus(strf("Merged %d vertices at their centre", n + 1));
}

void Editor::selectLoop(bool ring) {
    Object* o = activeMesh();
    if (mode_ != Mode::Edit || !o) return;
    std::vector<meshedit::Edge> seeds = editEdgeList();
    if (seeds.empty()) return setStatus(ring ? "Select ring: select an edge first" : "Select loop: select an edge first", true);
    std::vector<meshedit::Edge> all;
    for (const auto& e : seeds) {
        auto part = ring ? meshedit::edgeRing(o->mesh, e) : meshedit::edgeLoop(o->mesh, e);
        all.insert(all.end(), part.begin(), part.end());
    }
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    selMode_ = SelMode::Edge;
    esel_ = all;
    verticesFromSelection();
    setStatus(strf("%s: %d edges", ring ? "Edge ring" : "Edge loop", (int)all.size()));
}

void Editor::selectLinked() {
    Object* o = activeMesh();
    if (mode_ != Mode::Edit || !o) return;
    syncEditSelection();
    auto& sel = vertSel();
    const Mesh& m = o->mesh;
    // Vertex -> faces, then flood fill through shared vertices.
    std::vector<std::vector<int>> vf(m.verts.size());
    for (int f = 0; f < (int)m.faces.size(); ++f)
        for (int v : m.faces[f]) vf[v].push_back(f);
    std::vector<int> stack;
    for (size_t v = 0; v < sel.size(); ++v)
        if (sel[v]) stack.push_back((int)v);
    if (stack.empty()) return setStatus("Select linked: select part of a piece first", true);
    std::vector<char> faceSeen(m.faces.size(), 0);
    while (!stack.empty()) {
        int v = stack.back();
        stack.pop_back();
        for (int f : vf[v]) {
            if (faceSeen[f]) continue;
            faceSeen[f] = 1;
            for (int w : m.faces[f])
                if (!sel[w]) sel[w] = 1, stack.push_back(w);
        }
    }
    selectionFromVertices();
    if (selMode_ == SelMode::Face) verticesFromSelection();
    setStatus(strf("Selected the connected piece: %d vertices", (int)std::count(sel.begin(), sel.end(), 1)));
}

void Editor::joinSelected() {
    if (mode_ != Mode::Object) return setStatus("Join works in Object mode (Tab)", true);
    Object* target = activeMesh();
    std::vector<int> others;
    for (int i = 0; i < (int)scene_.objects.size(); ++i)
        if (i != scene_.active && scene_.objects[i].selected && scene_.objects[i].isMesh()) others.push_back(i);
    if (!target || others.empty()) return setStatus("Join: select meshes, the one to join into last (active)", true);
    pushUndo();
    makeEditable(*target);
    const Mat4 toLocal = inverse(scene_.world(scene_.active));
    bool droppedSkin = !target->skinBones.empty();
    for (int i : others) {
        Object& o = scene_.objects[i];
        droppedSkin |= !o.skinBones.empty();
        if (o.param.active()) makeEditable(o);
        meshedit::appendMesh(target->mesh, o.mesh, toLocal * scene_.world(i));
    }
    if (droppedSkin) {
        target->mesh.weights.clear();
        target->skinBones.clear();
        target->bindInverse.clear();
    }
    const uint32_t targetId = target->id;
    for (Object& o : scene_.objects) o.selected = false;
    for (int i : others) scene_.objects[i].selected = true;
    for (int i = 0; i < (int)scene_.objects.size(); ++i) {
        if (scene_.objects[i].selected) continue;
        int p = scene_.parentIndex(i), original = p;
        while (p >= 0 && scene_.objects[p].selected) p = scene_.parentIndex(p);
        if (p != original) scene_.setParent(i, p, true);
    }
    scene_.objects.erase(std::remove_if(scene_.objects.begin(), scene_.objects.end(), [](const Object& o) { return o.selected; }),
                         scene_.objects.end());
    int ti = scene_.indexOf(targetId);
    selectOnly(ti);
    markDirty();
    setStatus(strf("Joined %d mesh(es) into %s%s", (int)others.size(), scene_.objects[ti].name.c_str(),
                   droppedSkin ? " (skinning removed - bind again)" : ""));
}
