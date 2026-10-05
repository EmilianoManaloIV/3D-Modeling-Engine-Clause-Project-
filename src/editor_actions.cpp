#include "editor_internal.h"
#include "skin.h"

#include <algorithm>
#include <cmath>
#include <set>

using namespace ed;

// ============================================================================
// Creation
// ============================================================================
int Editor::addObject(Object o, const char* status) {
    if (xf_ != Xform::None) return -1;
    if (mode_ == Mode::Edit) setMode(Mode::Object);
    pushUndo();
    int i = scene_.addObject(std::move(o));
    selectOnly(i);
    markDirty();
    setStatus(std::string(status) + " " + scene_.objects[i].name);
    return i;
}

void Editor::addShape(int shape) {
    Object o;
    o.name = shapeDef(shape).name;
    o.param = defaultSpec(shape);
    o.mesh = generateParametric(o.param);
    o.color = kPalette[colorCursor_];
    colorCursor_ = colorCursor_ % 9 + 1;
    o.position = cam_.target;
    addObject(std::move(o), "Added");
}

void Editor::addLight(LightType type) {
    Object o;
    o.kind = ObjectKind::Light;
    o.light.type = type;
    o.position = cam_.target + Vec3(0, 3, 0);
    if (type == LightType::Point) {
        o.name = "Point Light";
    } else if (type == LightType::Sun) {
        o.name = "Sun";
        o.light.intensity = 2.5f;
        o.light.color = {1.0f, 0.97f, 0.9f};
        o.rotation = {-35, 25, 0};
        o.position = cam_.target + Vec3(0, 6, 0);
    } else {
        o.name = "Spot Light";
        o.light.intensity = 45.0f;
        o.light.range = 30.0f;
        o.position = cam_.target + Vec3(0, 4, 3);
        o.rotation = {35, 0, 0};
    }
    addObject(std::move(o), "Added");
    if (shading_ == SHADE_STUDIO) {
        shading_ = SHADE_LIT;
        setStatus("Added a light - viewport switched to Lit shading (Z cycles shading modes)");
    }
}

void Editor::addEmpty() {
    Object o;
    o.kind = ObjectKind::Empty;
    o.name = "Empty";
    o.position = cam_.target;
    addObject(std::move(o), "Added");
}

void Editor::addBone(bool asChild) {
    Object o;
    o.kind = ObjectKind::Bone;
    o.name = "Bone";
    Object* a = active();
    if (asChild && a && a->kind == ObjectKind::Bone) {
        o.parent = a->id;
        o.position = {0, a->boneLength, 0};
        o.boneLength = a->boneLength;
        addObject(std::move(o), "Extruded");
    } else {
        o.position = cam_.target;
        addObject(std::move(o), "Added");
    }
}

void Editor::addEmitter() {
    Object o;
    o.kind = ObjectKind::Emitter;
    o.name = "Emitter";
    o.position = cam_.target;
    addObject(std::move(o), "Added");
    playing_ = true;
}

// ============================================================================
// Object / mesh tools
// ============================================================================
std::vector<int> Editor::transformRoots() {
    std::vector<int> out;
    for (int i = 0; i < (int)scene_.objects.size(); ++i) {
        if (!scene_.objects[i].selected) continue;
        bool ancestorSelected = false;
        for (int p = scene_.parentIndex(i), guard = 0; p >= 0 && guard < 256; p = scene_.parentIndex(p), ++guard)
            if (scene_.objects[p].selected) ancestorSelected = true;
        if (!ancestorSelected) out.push_back(i);
    }
    return out;
}

std::vector<int> Editor::editFaces() {
    Object* o = activeMesh();
    if (!o || mode_ != Mode::Edit) return {};
    return selectedFaces(o->mesh, vertSel());  // empty = whole mesh
}

void Editor::regenerate(Object& o) {
    if (!o.param.active()) return;
    clampSpec(o.param);
    o.mesh = generateParametric(o.param);
}

bool Editor::makeEditable(Object& o) {
    if (!o.param.active()) return false;
    o.param = ParametricSpec();
    return true;
}

void Editor::duplicateSelected() {
    if (mode_ != Mode::Object) {
        setStatus("Duplicate works in Object mode (Tab to switch)", true);
        return;
    }
    if (scene_.selectedCount() == 0) {
        setStatus("Nothing selected to duplicate", true);
        return;
    }
    pushUndo();
    const int n = (int)scene_.objects.size();
    std::unordered_map<uint32_t, uint32_t> newId;  // old id -> copy id
    for (int i = 0; i < n; ++i)
        if (scene_.objects[i].selected) newId[scene_.objects[i].id] = scene_.nextId++;
    int newActive = -1, count = 0;
    for (int i = 0; i < n; ++i) {
        if (!scene_.objects[i].selected) continue;
        Object copy = scene_.objects[i];
        copy.id = newId[copy.id];
        copy.name = scene_.uniqueName(copy.name);
        // Keep the copy inside the same hierarchy - or inside the copied
        // parent when the parent is duplicated too (whole rigs duplicate).
        if (newId.count(copy.parent)) copy.parent = newId[copy.parent];
        for (uint32_t& b : copy.skinBones)
            if (newId.count(b)) b = newId[b];
        copy.selected = true;
        scene_.objects[i].selected = false;
        bool wasActive = i == scene_.active;
        scene_.objects.push_back(std::move(copy));
        if (wasActive || newActive < 0) newActive = (int)scene_.objects.size() - 1;
        ++count;
    }
    scene_.active = newActive;
    markDirty();
    beginTransform(Xform::Grab, false);
    setStatus(strf("Duplicated %d object(s)", count));
}

void Editor::deleteSelected() {
    if (mode_ == Mode::Edit) {
        Object* o = activeMesh();
        if (!o) return;
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
    int count = scene_.selectedCount();
    if (!count) {
        setStatus("Nothing selected to delete", true);
        return;
    }
    pushUndo();
    // Children of deleted objects move up to the nearest surviving ancestor,
    // keeping their place in the world.
    for (int i = 0; i < (int)scene_.objects.size(); ++i) {
        if (scene_.objects[i].selected) continue;
        int p = scene_.parentIndex(i), original = p;
        while (p >= 0 && scene_.objects[p].selected) p = scene_.parentIndex(p);
        if (p != original) scene_.setParent(i, p, true);
    }
    scene_.objects.erase(std::remove_if(scene_.objects.begin(), scene_.objects.end(),
                                        [](const Object& o) { return o.selected; }),
                         scene_.objects.end());
    scene_.active = -1;
    markDirty();
    setStatus(strf("Deleted %d object(s)", count));
}

void Editor::subdivideSelected() {
    std::vector<int> targets;
    for (int i = 0; i < (int)scene_.objects.size(); ++i) {
        const Object& o = scene_.objects[i];
        if (!o.isMesh()) continue;
        if (mode_ == Mode::Edit ? i == scene_.active : o.selected) targets.push_back(i);
    }
    if (targets.empty()) {
        setStatus("Select a mesh to subdivide", true);
        return;
    }
    for (int i : targets) {
        size_t corners = 0;
        for (const auto& f : scene_.objects[i].mesh.faces) corners += f.size();
        if (corners > kMaxSubdivCorners) {
            setStatus(scene_.objects[i].name + " is already too dense to subdivide", true);
            return;
        }
    }
    pushUndo();
    bool nonDestructive = false;
    for (int i : targets) {
        Object& o = scene_.objects[i];
        if (o.param.active() && o.param.subdivisions < 3) {
            o.param.subdivisions++;  // stays parametric: just one more modifier level
            regenerate(o);
            nonDestructive = true;
        } else {
            makeEditable(o);
            o.mesh = catmullClark(o.mesh);
        }
    }
    if (mode_ == Mode::Edit) vsel_.clear();
    markDirty();
    setStatus(nonDestructive ? "Subdivision level raised (parametric, see Properties)" : "Subdivided (Catmull-Clark)");
}

void Editor::flipSelected() {
    std::vector<int> targets;
    for (int i = 0; i < (int)scene_.objects.size(); ++i)
        if (scene_.objects[i].isMesh() && (mode_ == Mode::Edit ? i == scene_.active : scene_.objects[i].selected))
            targets.push_back(i);
    if (targets.empty()) {
        setStatus("Select a mesh first", true);
        return;
    }
    pushUndo();
    for (int i : targets) {
        makeEditable(scene_.objects[i]);
        flipNormals(scene_.objects[i].mesh);
    }
    markDirty();
    setStatus("Flipped face normals");
}

void Editor::setSmoothSelected(bool smooth) {
    pushUndo();
    for (int i = 0; i < (int)scene_.objects.size(); ++i)
        if (scene_.objects[i].selected || i == scene_.active) scene_.objects[i].smooth = smooth;
    markDirty();
    setStatus(smooth ? "Smooth shading" : "Flat shading");
}

void Editor::selectAll() {
    if (mode_ == Mode::Edit) {
        auto& sel = vertSel();
        bool any = std::find(sel.begin(), sel.end(), 1) != sel.end();
        std::fill(sel.begin(), sel.end(), any ? 0 : 1);
        return;
    }
    bool any = scene_.selectedCount() > 0;
    for (auto& o : scene_.objects) o.selected = !any;
    scene_.active = any || scene_.objects.empty() ? -1 : (int)scene_.objects.size() - 1;
}

void Editor::extrude() {
    if (mode_ != Mode::Edit) {
        setStatus("Extrude works in Edit mode (Tab)", true);
        return;
    }
    Object* o = activeMesh();
    if (!o) return;
    auto& sel = vertSel();
    if (selectedFaces(o->mesh, sel).empty()) {
        setStatus("Select all vertices of at least one face to extrude", true);
        return;
    }
    pushUndo();
    Vec3 n;
    extrudeSelectedFaces(o->mesh, sel, &n);
    markDirty();
    Mat4 normalMatrix = transpose(inverse(scene_.world(scene_.active)));
    xfCustomAxis_ = normalize(transformDir(normalMatrix, n));
    if (beginTransform(Xform::Grab, false) && dot(xfCustomAxis_, xfCustomAxis_) > 0.5f) xfAxis_ = 3;
    setStatus("Extruded - move the mouse, click to confirm");
}

void Editor::setMode(Mode m) {
    if (m == mode_ || xf_ != Xform::None) return;
    if (m == Mode::Edit) {
        Object* o = activeMesh();
        if (!o) {
            setStatus("Select a mesh to edit its vertices", true);
            return;
        }
        std::string note;
        if (o->param.active()) {
            pushUndo();
            makeEditable(*o);
            markDirty();
            note = " (parametric shape converted to an editable mesh - undo restores it)";
        }
        vsel_.assign(o->mesh.verts.size(), 0);
        setStatus("Edit mode: select vertices, G/R/S transform, E extrude, Tab to exit" + note);
    } else {
        setStatus("Object mode");
    }
    mode_ = m;
    lmbInViewport_ = boxSelecting_ = false;
}

void Editor::frameSelected() {
    Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    bool any = false;
    auto addPoint = [&](Vec3 p) {
        lo = vmin(lo, p);
        hi = vmax(hi, p);
        any = true;
    };
    if (mode_ == Mode::Edit && activeMesh()) {
        Object& o = *activeMesh();
        Mat4 m = scene_.world(scene_.active);
        auto& sel = vertSel();
        for (size_t v = 0; v < sel.size(); ++v)
            if (sel[v]) addPoint(transformPoint(m, o.mesh.verts[v]));
        if (!any)
            for (const Vec3& v : o.mesh.verts) addPoint(transformPoint(m, v));
    } else {
        bool onlySelected = scene_.selectedCount() > 0;
        for (int i = 0; i < (int)scene_.objects.size(); ++i) {
            const Object& o = scene_.objects[i];
            if (onlySelected && !o.selected) continue;
            Mat4 m;
            const std::vector<Vec3>& pos = evaluateMesh(scene_, i, false, scratch_, m);
            if (o.isMesh()) {
                for (const Vec3& v : pos) addPoint(transformPoint(m, v));
            } else {
                addPoint(transformPoint(scene_.world(i), Vec3()));
                if (o.kind == ObjectKind::Bone) addPoint(boneTail(scene_, i));
            }
        }
    }
    if (!any) {
        cam_.target = Vec3();
        cam_.distance = 9.0f;
        return;
    }
    float radius = std::max(0.5f, length(hi - lo) * 0.5f);
    cam_.target = (lo + hi) * 0.5f;
    cam_.distance = radius / std::sin(toRadians(cam_.fovY) * 0.5f) * 1.1f;
}

void Editor::setView(float yaw, float pitch) {
    cam_.yaw = yaw;
    cam_.pitch = pitch;
}

// ============================================================================
// Hierarchy
// ============================================================================
void Editor::parentSelected() {
    const int parent = scene_.active;
    if (mode_ != Mode::Object || parent < 0 || scene_.selectedCount() < 2) {
        setStatus("Select the children, then Shift+click the parent last (it becomes active), then Parent", true);
        return;
    }
    pushUndo();
    int count = 0, refused = 0;
    for (int i = 0; i < (int)scene_.objects.size(); ++i) {
        if (i == parent || !scene_.objects[i].selected) continue;
        if (scene_.setParent(i, parent, true)) ++count;
        else ++refused;
    }
    if (!count) {
        undo_.pop_back();
        setStatus("Can't parent an object to its own child", true);
        return;
    }
    markDirty();
    setStatus(strf("Parented %d object(s) to %s%s", count, scene_.objects[parent].name.c_str(),
                   refused ? " (skipped cycles)" : ""));
}

void Editor::unparentSelected() {
    std::vector<int> targets;
    for (int i = 0; i < (int)scene_.objects.size(); ++i)
        if (scene_.objects[i].selected && scene_.objects[i].parent) targets.push_back(i);
    if (targets.empty()) {
        setStatus("No selected object has a parent", true);
        return;
    }
    pushUndo();
    for (int i : targets) scene_.setParent(i, -1, true);
    markDirty();
    setStatus(strf("Cleared parent of %d object(s) (kept their world transform)", (int)targets.size()));
}

// ============================================================================
// Rigging
// ============================================================================
void Editor::bindSelected() {
    std::set<int> bones;
    std::vector<int> meshes;
    for (int i = 0; i < (int)scene_.objects.size(); ++i) {
        const Object& o = scene_.objects[i];
        if (!o.selected) continue;
        if (o.isMesh()) {
            meshes.push_back(i);
            continue;
        }
        // A selected bone (or empty / root) brings its whole bone chain along.
        if (o.kind == ObjectKind::Bone) bones.insert(i);
        for (int d : scene_.descendants(i))
            if (scene_.objects[d].kind == ObjectKind::Bone) bones.insert(d);
    }
    if (meshes.empty() || bones.empty()) {
        setStatus("Select a mesh and its root bone (Shift+click), then Bind", true);
        return;
    }
    pushUndo();
    std::vector<int> boneList(bones.begin(), bones.end());
    for (int m : meshes) {
        makeEditable(scene_.objects[m]);
        std::string err;
        bindSkin(scene_, m, boneList, err);
    }
    weightSlot_ = 0;
    markDirty();
    setStatus(strf("Bound %d mesh(es) to %d bone(s) with automatic weights - rotate bones (R) to pose",
                   (int)meshes.size(), (int)boneList.size()));
}

void Editor::unbindSelected() {
    std::vector<int> targets;
    for (int i = 0; i < (int)scene_.objects.size(); ++i)
        if (scene_.objects[i].selected && !scene_.objects[i].skinBones.empty()) targets.push_back(i);
    if (targets.empty()) {
        setStatus("No selected mesh is bound to bones", true);
        return;
    }
    pushUndo();
    for (int i : targets) unbindSkin(scene_.objects[i]);
    markDirty();
    setStatus("Removed skinning");
}

void Editor::resetPose() {
    bool anySelected = false;
    for (const Object& o : scene_.objects) anySelected |= o.selected && o.kind == ObjectKind::Bone;
    pushUndo();
    int count = 0;
    for (Object& o : scene_.objects) {
        if (o.kind != ObjectKind::Bone || !o.hasRest || (anySelected && !o.selected)) continue;
        o.position = o.restPosition;
        o.rotation = o.restRotation;
        o.scale = o.restScale;
        ++count;
    }
    if (!count) {
        undo_.pop_back();
        setStatus("No bound bones to reset (bind a mesh first)", true);
        return;
    }
    markDirty();
    setStatus(strf("Reset %d bone(s) to their rest pose", count));
}

void Editor::recomputeWeights() {
    Object* o = activeMesh();
    if (!o || o->skinBones.empty()) {
        setStatus("The active mesh is not bound to bones", true);
        return;
    }
    pushUndo();
    autoWeights(scene_, scene_.active);
    markDirty();
    setStatus("Recomputed automatic weights");
}

void Editor::assignWeight(bool remove) {
    Object* o = activeMesh();
    if (mode_ != Mode::Edit || !o || !isSkinned(*o)) {
        setStatus("Weights are edited in Edit mode on a bound mesh", true);
        return;
    }
    int slot = weightSlotFor(*o);
    auto& sel = vertSel();
    int count = (int)std::count(sel.begin(), sel.end(), 1);
    if (slot < 0 || count == 0) {
        setStatus("Pick a bone in the weights list and select vertices first", true);
        return;
    }
    pushUndo();
    for (size_t v = 0; v < sel.size(); ++v) {
        if (!sel[v]) continue;
        if (remove) o->mesh.weights[v].remove(slot);
        else o->mesh.weights[v].set(slot, weightValue_);
    }
    o->mesh.touch();
    markDirty();
    setStatus(strf("%s weight on %d vertices", remove ? "Removed" : "Assigned", count));
}

void Editor::normalizeWeights() {
    Object* o = activeMesh();
    if (!o || !isSkinned(*o)) {
        setStatus("The active mesh is not bound to bones", true);
        return;
    }
    pushUndo();
    auto& sel = vertSel();
    bool any = std::find(sel.begin(), sel.end(), 1) != sel.end();
    for (size_t v = 0; v < o->mesh.weights.size(); ++v)
        if (!any || (mode_ == Mode::Edit && sel[v])) o->mesh.weights[v].normalize();
    o->mesh.touch();
    markDirty();
    setStatus("Normalized weights");
}

// ============================================================================
// UVs
// ============================================================================
void Editor::unwrapActive(uv::Method method) {
    std::vector<int> targets;
    for (int i = 0; i < (int)scene_.objects.size(); ++i)
        if (scene_.objects[i].isMesh() && (mode_ == Mode::Edit ? i == scene_.active : scene_.objects[i].selected))
            targets.push_back(i);
    if (targets.empty()) {
        setStatus("Select a mesh to unwrap", true);
        return;
    }
    pushUndo();
    bool baked = false;
    for (int i : targets) {
        baked |= makeEditable(scene_.objects[i]);
        uv::unwrap(scene_.objects[i].mesh, method, mode_ == Mode::Edit ? editFaces() : std::vector<int>());
    }
    markDirty();
    if (shading_ != SHADE_CHECKER) shading_ = SHADE_CHECKER;
    setStatus(strf("%s unwrap done%s - showing the UV checker (Z cycles shading)", uv::methodName(method),
                   baked ? "; parametric shape baked" : ""));
}

void Editor::uvTool(int tool) {
    Object* o = activeMesh();
    if (!o) {
        setStatus("Select a mesh first", true);
        return;
    }
    pushUndo();
    makeEditable(*o);
    std::vector<int> faces = editFaces();
    static const char* names[] = {"Fit to 0-1", "Pack islands", "Rotate 90", "Flip U", "Flip V"};
    switch (tool) {
        case 0: uv::fit(o->mesh, faces); break;
        case 1: uv::pack(o->mesh, faces); break;
        case 2: uv::rotate90(o->mesh, faces); break;
        case 3: uv::flip(o->mesh, faces, true); break;
        default: uv::flip(o->mesh, faces, false); break;
    }
    markDirty();
    setStatus(std::string("UV: ") + names[std::max(0, std::min(tool, 4))]);
}

// ============================================================================
// Particles
// ============================================================================
void Editor::togglePlay() {
    playing_ = !playing_;
    setStatus(playing_ ? "Particles playing" : "Particles paused");
}

void Editor::restartParticles() {
    particles_.clear();
    playing_ = true;
    setStatus("Particles restarted");
}

// ============================================================================
// Files
// ============================================================================
void Editor::newScene() {
    if (xf_ != Xform::None) return;
    pushUndo();
    scene_.objects.clear();
    scene_.active = -1;
    mode_ = Mode::Object;
    vsel_.clear();
    particles_.clear();
    filePath_ = fileField_ = "untitled.m3d";
    dirty_ = false;
    cam_ = Camera();
    setStatus("New scene (Ctrl+Z brings the old one back)");
}

void Editor::saveFile() {
    std::string path = withExtension(fileField_, ".m3d");
    std::string err;
    if (saveScene(scene_, path, err)) {
        filePath_ = fileField_ = path;
        dirty_ = false;
        setStatus("Saved " + path);
    } else {
        setStatus(err, true);
    }
}

void Editor::loadFile() {
    std::string path = withExtension(fileField_, ".m3d");
    Scene loaded;
    loaded.nextId = scene_.nextId;
    std::string err;
    if (!loadScene(loaded, path, err)) {
        setStatus(err, true);
        return;
    }
    pushUndo();
    scene_.objects = std::move(loaded.objects);
    scene_.nextId = loaded.nextId;
    scene_.ambient = loaded.ambient;
    scene_.active = -1;
    mode_ = Mode::Object;
    vsel_.clear();
    particles_.clear();
    filePath_ = fileField_ = path;
    dirty_ = false;
    frameSelected();
    setStatus(strf("Loaded %s (%d objects)", path.c_str(), (int)scene_.objects.size()));
}

void Editor::openPath(const std::string& path) {
    fileField_ = path;
    if (endsWithNoCase(path, ".obj")) {
        importObj();
        filePath_ = fileField_ = withExtension(path, ".m3d");
        dirty_ = false;
    } else {
        loadFile();
    }
}

void Editor::exportObj() {
    if (scene_.objects.empty()) {
        setStatus("Nothing to export", true);
        return;
    }
    std::string path = withExtension(fileField_, ".obj");
    std::string err;
    int count = 0;
    if (exportOBJ(scene_, path, err, &count))
        setStatus(strf("Exported %d mesh(es) to %s (+ .mtl)", count, path.c_str()));
    else
        setStatus(err, true);
}

void Editor::importObj() {
    std::string path = withExtension(fileField_, ".obj");
    Snapshot before = snapshot();
    std::string err;
    int first = -1;
    if (!importOBJ(scene_, path, err, &first)) {
        setStatus(err, true);
        return;
    }
    undo_.push_back(std::move(before));
    while (undo_.size() > kMaxUndo) undo_.pop_front();
    redo_.clear();
    mode_ = Mode::Object;
    for (int i = 0; i < (int)scene_.objects.size(); ++i) scene_.objects[i].selected = i >= first;
    scene_.active = first;
    frameSelected();
    markDirty();
    setStatus(strf("Imported %d object(s) from %s", (int)scene_.objects.size() - first, path.c_str()));
}

// ============================================================================
// Sample scenes (also used for automated screenshots: --demo N)
// ============================================================================
void Editor::buildDemo(int which) {
    auto shape = [&](int s, const char* name, int color, Vec3 pos, Vec3 rot = Vec3()) {
        Object o;
        o.name = name;
        o.param = defaultSpec(s);
        o.mesh = generateParametric(o.param);
        o.color = kPalette[color];
        o.position = pos;
        o.rotation = rot;
        return scene_.addObject(std::move(o));
    };
    auto setParam = [&](int i, std::initializer_list<float> values) {
        int k = 0;
        for (float v : values) scene_.objects[i].param.p[k++] = v;
        regenerate(scene_.objects[i]);
    };
    auto light = [&](LightType t, const char* name, Vec3 pos, Vec3 rot, Vec3 color, float intensity) {
        Object o;
        o.kind = ObjectKind::Light;
        o.name = name;
        o.position = pos;
        o.rotation = rot;
        o.light.type = t;
        o.light.color = color;
        o.light.intensity = intensity;
        o.light.range = 30;
        return scene_.addObject(std::move(o));
    };
    auto bone = [&](const char* name, int parent, Vec3 pos, float len) {
        Object o;
        o.kind = ObjectKind::Bone;
        o.name = name;
        o.position = pos;
        o.boneLength = len;
        if (parent >= 0) o.parent = scene_.objects[parent].id;
        return scene_.addObject(std::move(o));
    };

    switch (which) {
        case 1: {  // showcase: lights, emission, particles, hierarchy, parametric shapes
            int ground = shape(PS_Plane, "Ground", 8, {0, 0, -1});
            setParam(ground, {14, 10, 1, 1});
            int g = shape(PS_Gear, "Gear", 4, {-3, 1.05f, 0.3f}, {90, 0, 0});
            scene_.objects[g].gloss = 0.85f;
            int st = shape(PS_Stairs, "Stairs", 9, {-4.2f, 0.8f, -3.5f}, {0, 30, 0});
            (void)st;
            int sp = shape(PS_Spring, "Spring", 2, {0.2f, 1.1f, -3.0f});
            scene_.objects[sp].param.twist = 0;
            int t = shape(PS_Torus, "Torus", 6, {3.3f, 1.0f, -2.6f}, {65, 0, 0});
            scene_.objects[t].gloss = 0.7f;
            int brazier = shape(PS_Pipe, "Brazier", 8, {2.6f, 0.45f, 1.0f});
            setParam(brazier, {0.6f, 0.45f, 0.9f, 32});
            Object fire;
            fire.kind = ObjectKind::Emitter;
            fire.name = "Fire";
            fire.parent = scene_.objects[brazier].id;
            fire.position = {0, 0.35f, 0};
            fire.particles.rate = 160;
            fire.particles.spread = 12;
            fire.particles.speed = 1.6f;
            fire.particles.gravity = 1.0f;
            fire.particles.lifetime = 1.3f;
            fire.particles.radius = 0.3f;
            fire.particles.startSize = 0.45f;
            scene_.addObject(fire);
            int fireLight = light(LightType::Point, "Fire Light", {0, 0.9f, 0}, {}, {1.0f, 0.55f, 0.2f}, 6);
            scene_.objects[fireLight].parent = scene_.objects[brazier].id;
            int lamp = shape(PS_Sphere, "Lamp", 9, {0.3f, 1.4f, 0.9f});
            setParam(lamp, {0.45f, 32, 16});
            scene_.objects[lamp].emission = {1.0f, 0.85f, 0.55f};
            scene_.objects[lamp].emissionStrength = 2.5f;
            int lampLight = light(LightType::Point, "Lamp Light", {0, 0, 0}, {}, {1.0f, 0.8f, 0.55f}, 5);
            scene_.objects[lampLight].parent = scene_.objects[lamp].id;
            light(LightType::Sun, "Sun", {-2, 7, 2}, {-40, 35, 0}, {0.75f, 0.82f, 1.0f}, 1.4f);
            light(LightType::Spot, "Spot", {-1.5f, 5.5f, 3.5f}, {38, 15, 0}, {0.55f, 0.75f, 1.0f}, 70);
            scene_.ambient = {0.04f, 0.045f, 0.06f};
            selectOnly(g);
            shading_ = SHADE_LIT;
            cam_.target = {0, 0.8f, -0.9f};
            cam_.distance = 13.0f;
            cam_.yaw = 22;
            cam_.pitch = 24;
            break;
        }
        case 2: {  // edit mode: extruded and tapered cube top
            int i = shape(PS_Cube, "Cube", 4, {0, 1, 0});
            Object& o = scene_.objects[i];
            makeEditable(o);
            selectOnly(i);
            Mesh& m = o.mesh;
            vsel_.assign(m.verts.size(), 0);
            for (size_t v = 0; v < m.verts.size(); ++v) vsel_[v] = m.verts[v].y > 0 ? 1 : 0;
            Vec3 n;
            extrudeSelectedFaces(m, vsel_, &n);
            for (size_t v = 0; v < m.verts.size(); ++v)
                if (vsel_[v]) {
                    m.verts[v] += n * 1.2f;
                    m.verts[v].x *= 0.5f;
                    m.verts[v].z *= 0.5f;
                }
            m.touch();
            mode_ = Mode::Edit;
            cam_.target = {0, 1.6f, 0};
            cam_.distance = 7.5f;
            break;
        }
        case 3: {  // UVs: smart-unwrapped gear on the checker, UV editor open
            int i = shape(PS_Gear, "Gear", 0, {0, 0, 0}, {60, 0, 0});
            Object& o = scene_.objects[i];
            makeEditable(o);
            uv::unwrap(o.mesh, uv::Method::Smart);
            selectOnly(i);
            mode_ = Mode::Edit;
            vsel_.assign(o.mesh.verts.size(), 0);
            for (size_t v = 0; v < o.mesh.verts.size(); ++v) vsel_[v] = 1;
            shading_ = SHADE_CHECKER;
            uvEditor_ = true;
            cam_.distance = 5.5f;
            break;
        }
        case 4: {  // rig: a segmented column bent by a two-bone chain
            int col = shape(PS_Cube, "Column", 3, {0, 2, 0});
            setParam(col, {0.7f, 4, 0.7f, 8});
            int root = bone("Bone.Root", -1, {0, 0, 0}, 2);
            int tip = bone("Bone.Tip", root, {0, 2, 0}, 2);
            makeEditable(scene_.objects[col]);
            std::string err;
            bindSkin(scene_, col, {root, tip}, err);
            scene_.objects[tip].rotation = {0, 0, 55};
            scene_.objects[root].rotation = {0, 0, 10};
            selectOnly(tip);
            weightSlot_ = 1;
            shading_ = SHADE_WEIGHTS;
            scene_.active = col;
            scene_.objects[col].selected = false;
            scene_.objects[tip].selected = true;
            scene_.active = tip;
            weightSlot_ = 1;
            cam_.target = {-0.6f, 2.0f, 0};
            cam_.distance = 9;
            cam_.yaw = 10;
            cam_.pitch = 12;
            break;
        }
        default: {  // default scene: a cube and a light, like most modelers
            int i = shape(PS_Cube, "Cube", 0, {0, 0, 0});
            light(LightType::Point, "Light", {2.4f, 2.6f, 1.6f}, {}, {1.0f, 0.95f, 0.85f}, 25);
            selectOnly(i);
            break;
        }
    }
}
