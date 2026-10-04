#include "editor.h"

#include "image_io.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

const Color kAxisColor[3] = {{0.93f, 0.33f, 0.33f, 1}, {0.47f, 0.80f, 0.30f, 1}, {0.33f, 0.56f, 0.98f, 1}};
const Color kCustomAxisColor{0.96f, 0.86f, 0.30f, 1};
const Color kViewportBg{0.215f, 0.225f, 0.245f, 1};
const Color kEdgeDark{0.04f, 0.04f, 0.06f, 0.85f};

const Vec3 kPalette[10] = {{0.80f, 0.80f, 0.80f}, {0.90f, 0.42f, 0.33f}, {0.95f, 0.74f, 0.32f},
                           {0.52f, 0.78f, 0.42f}, {0.36f, 0.66f, 0.90f}, {0.62f, 0.50f, 0.90f},
                           {0.90f, 0.52f, 0.72f}, {0.40f, 0.78f, 0.74f}, {0.30f, 0.31f, 0.34f},
                           {0.97f, 0.96f, 0.93f}};
const char* const kPrimitiveNames[6] = {"Cube", "Sphere", "Cylinder", "Cone", "Plane", "Torus"};

constexpr size_t kMaxUndo = 64;
constexpr size_t kMaxSubdivCorners = 400000;  // refuse subdivisions that would explode

struct Layout {
    float x, y, w, gap;
    Rect row(float h) {
        Rect r{x, y, w, h};
        y += h + gap;
        return r;
    }
};

Rect cell(const Rect& r, int i, int n, float gap) {
    float cw = (r.w - gap * (n - 1)) / n;
    return {std::floor(r.x + i * (cw + gap)), r.y, std::floor(cw), r.h};
}

std::string strf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

bool endsWithNoCase(const std::string& s, const char* suffix) {
    size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower((unsigned char)s[s.size() - n + i]) != std::tolower((unsigned char)suffix[i])) return false;
    return true;
}

// "chair" / "chair.m3d" / "chair.obj"  ->  "chair<ext>"
std::string withExtension(const std::string& in, const char* ext) {
    std::string p = trimmed(in);
    if (endsWithNoCase(p, ext)) return p;
    if (endsWithNoCase(p, ".m3d") || endsWithNoCase(p, ".obj")) p.resize(p.size() - 4);
    if (p.empty()) p = "scene";
    return p + ext;
}

std::string fileNameOf(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? path : path.substr(p + 1);
}

float wrapRadians(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}

Vec3 axisVector(int a) {
    Vec3 v;
    v[a] = 1.0f;
    return v;
}

Color toColor(Vec3 v, float a = 1.0f) { return {v.x, v.y, v.z, a}; }

bool anyKeyPressed(const Input& in) {
    for (int k = 1; k < KEY_COUNT; ++k)
        if (in.keyPressed[k] && k != KEY_SHIFT && k != KEY_CONTROL && k != KEY_ALT) return true;
    return in.mousePressed[0] || in.mousePressed[1] || in.mousePressed[2];
}

}  // namespace

// ============================================================================
// Camera
// ============================================================================
Vec3 Camera::eye() const {
    float p = toRadians(pitch), y = toRadians(yaw);
    return target + Vec3(std::cos(p) * std::sin(y), std::sin(p), std::cos(p) * std::cos(y)) * distance;
}

Mat4 Camera::view() const { return lookAt(eye(), target, {0, 1, 0}); }

float Camera::orthoHalfHeight() const { return distance * std::tan(toRadians(fovY) * 0.5f); }

Mat4 Camera::projection(float aspect) const {
    if (ortho) {
        float h = orthoHalfHeight();
        float range = distance * 20.0f + 200.0f;
        return orthographic(-h * aspect, h * aspect, -h, h, -range, range);
    }
    float n = std::max(0.01f, distance * 0.002f);
    float f = std::max(500.0f, distance * 60.0f);
    return perspective(toRadians(fovY), aspect, n, f);
}

// ============================================================================
// Setup
// ============================================================================
bool Editor::init(const AppOptions& options, std::string& error) {
    if (!renderer_.init(error)) return false;
    dpi_ = platform::dpiScale();
    fontScale_ = std::max(1, (int)(dpi_ * 2.0f + 0.25f));
    buildGrid();

    if (!options.openPath.empty()) {
        openPath(options.openPath);
    } else if (options.demo) {
        buildDemo(options.demo);
    } else {
        int i = scene_.add(primitives::cube(), "Cube", kPalette[0]);
        selectOnly(i);
    }
    undo_.clear();
    redo_.clear();
    dirty_ = false;
    showHelp_ = options.showHelp;
    if (status_.empty()) setStatus("Welcome to Modeler3D - press F1 for help.");
    updateTitle();
    return true;
}

void Editor::shutdown() { renderer_.shutdown(); }

void Editor::buildGrid() {
    grid_.clear();
    const int n = 50;
    const Color minor{0.55f, 0.57f, 0.62f, 0.20f}, major{0.62f, 0.64f, 0.70f, 0.40f};
    for (int i = -n; i <= n; ++i) {
        float f = (float)i, e = (float)n;
        Color alongZ = i == 0 ? withAlpha(kAxisColor[2], 0.9f) : (i % 5 == 0 ? major : minor);
        Color alongX = i == 0 ? withAlpha(kAxisColor[0], 0.9f) : (i % 5 == 0 ? major : minor);
        grid_.push_back({{f, 0, -e}, alongZ});
        grid_.push_back({{f, 0, e}, alongZ});
        grid_.push_back({{-e, 0, f}, alongX});
        grid_.push_back({{e, 0, f}, alongX});
    }
}

void Editor::buildDemo(int which) {
    auto add = [&](Mesh m, const char* name, int color, Vec3 pos, Vec3 rot = Vec3(), Vec3 scl = Vec3(1, 1, 1)) {
        int i = scene_.add(std::move(m), name, kPalette[color]);
        Object& o = scene_.objects[i];
        o.position = pos;
        o.rotation = rot;
        o.scale = scl;
        return i;
    };
    if (which == 2) {
        int i = add(primitives::cube(), "Cube", 4, {0, 1, 0});
        selectOnly(i);
        Mesh& m = scene_.objects[i].mesh;
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
        return;
    }
    add(primitives::plane(2.0f, 1), "Ground", 8, {0, 0, -1.5f}, Vec3(), {5.5f, 1, 4});
    add(primitives::cube(), "Cube", 1, {-3, 1, 0}, {0, 25, 0});
    int sphere = add(primitives::uvSphere(), "Sphere", 4, {0, 1, 0});
    int torus = add(primitives::torus(), "Torus", 6, {3, 1.4f, 0}, {60, 0, 20});
    add(primitives::cylinder(0.8f, 2.0f), "Cylinder", 3, {-3, 1, -3});
    add(primitives::cone(), "Cone", 2, {0, 1, -3});
    add(catmullClark(catmullClark(primitives::cube())), "Subdivided", 5, {3, 1, -3});
    selectOnly(sphere);
    scene_.objects[torus].selected = true;
    cam_.target = {0, 0.8f, -1.3f};
    cam_.distance = 12.5f;
    cam_.yaw = 28;
    cam_.pitch = 26;
}

// ============================================================================
// Frame
// ============================================================================
void Editor::frame(const Input& in, int width, int height, float dt) {
    screenW_ = width;
    screenH_ = height;
    if (dt > 0) fps_ = fps_ * 0.95f + (1.0f / dt) * 0.05f;
    statusTime_ += dt;

    // Layout: tool panel | viewport | properties panel, status bar at bottom.
    const float fs = (float)fontScale_;
    const float lw = 136 * fs, rw = 140 * fs, sb = 12 * fs;
    leftPanel_ = {0, 0, lw, height - sb};
    rightPanel_ = {width - rw, 0, rw, height - sb};
    statusBar_ = {0, height - sb, (float)width, sb};
    viewport_ = {lw, 0, std::max(1.0f, width - lw - rw), std::max(1.0f, height - sb)};
    mouse_ = {in.mouseX - viewport_.x, in.mouseY - viewport_.y};
    updateMatrices();

    const bool modal = confirmQuit_ || showHelp_;
    ui_.begin(in, width, height, fontScale_);
    ui_.setInputEnabled(xf_ == Xform::None && !modal);
    buildLeftPanel(in);
    buildRightPanel(in);
    buildStatusBar();
    ui_.setInputEnabled(true);

    if (confirmQuit_) {
        if (in.pressed(KEY_ESCAPE)) confirmQuit_ = false;
    } else if (showHelp_) {
        if (anyKeyPressed(in)) showHelp_ = false;
    } else {
        if (!ui_.keyboardUsedThisFrame() && !ui_.wantsKeyboard()) handleShortcuts(in);
        handleViewport(in);
    }
    updateMatrices();
    buildViewportOverlay();
    if (showHelp_) buildHelp();
    if (confirmQuit_) buildQuitDialog();
    ui_.end();

    renderer_.clearWindow(width, height, theme::panel);
    renderViewport();
    renderer_.drawUI(ui_, width, height);
    renderer_.purge(scene_);
    if (screenshotPending_) saveScreenshot();

    if (editGroup_ && !ui_.isActive() && !ui_.wantsKeyboard()) editGroup_ = 0;
    updateTitle();
}

void Editor::requestQuit() {
    if (dirty_ && !scene_.objects.empty()) {
        confirmQuit_ = true;
        showHelp_ = false;
        if (xf_ != Xform::None) endTransform(false);
    } else {
        exit_ = true;
    }
}

// ============================================================================
// Helpers
// ============================================================================
void Editor::updateMatrices() {
    view_ = cam_.view();
    proj_ = cam_.projection(viewport_.w / std::max(1.0f, viewport_.h));
    viewProj_ = proj_ * view_;
}

bool Editor::worldToScreen(Vec3 p, Vec2& out) const {
    Vec4 c = viewProj_ * Vec4(p, 1.0f);
    if (c.w <= 1e-6f) return false;
    float x = c.x / c.w, y = c.y / c.w;
    out = {(x * 0.5f + 0.5f) * viewport_.w, (0.5f - y * 0.5f) * viewport_.h};
    return true;
}

// Unproject the pixel through the inverse view-projection (FoCG 5e sec. 4.3).
void Editor::viewRay(Vec2 p, Vec3& origin, Vec3& dir) const {
    float x = p.x / viewport_.w * 2.0f - 1.0f;
    float y = 1.0f - p.y / viewport_.h * 2.0f;
    Mat4 inv = inverse(viewProj_);
    Vec4 a = inv * Vec4(x, y, -1.0f, 1.0f);
    Vec4 b = inv * Vec4(x, y, 1.0f, 1.0f);
    Vec3 nearP = a.xyz() / a.w, farP = b.xyz() / b.w;
    origin = nearP;
    dir = normalize(farP - nearP);
}

float Editor::worldPerPixel(Vec3 p) const {
    float halfH;
    if (cam_.ortho) {
        halfH = cam_.orthoHalfHeight();
    } else {
        float depth = std::max(0.01f, dot(p - cam_.eye(), camForward()));
        halfH = depth * std::tan(toRadians(cam_.fovY) * 0.5f);
    }
    return 2.0f * halfH / std::max(1.0f, viewport_.h);
}

std::vector<char>& Editor::vertSel() {
    Object* o = active();
    size_t n = o ? o->mesh.verts.size() : 0;
    if (vsel_.size() != n) vsel_.assign(n, 0);
    return vsel_;
}

void Editor::selectOnly(int index) {
    for (int i = 0; i < (int)scene_.objects.size(); ++i) scene_.objects[i].selected = i == index;
    scene_.active = index;
}

void Editor::toggleSelect(int i) {
    Object& o = scene_.objects[i];
    if (o.selected && scene_.active == i) {
        o.selected = false;
        scene_.active = -1;
        for (int j = (int)scene_.objects.size() - 1; j >= 0; --j)
            if (scene_.objects[j].selected) {
                scene_.active = j;
                break;
            }
    } else {
        o.selected = true;
        scene_.active = i;
    }
}

void Editor::setStatus(const std::string& msg, bool error) {
    status_ = msg;
    statusError_ = error;
    statusTime_ = 0;
}

void Editor::updateTitle() {
    std::string title = "Modeler3D - " + fileNameOf(filePath_) + (dirty_ ? " *" : "");
    if (title != lastTitle_) {
        platform::setTitle(title);
        lastTitle_ = title;
    }
}

// ============================================================================
// Undo / redo: whole-scene snapshots (simple and robust for a small editor;
// a command pattern would store deltas instead).
// ============================================================================
Editor::Snapshot Editor::snapshot() const { return {scene_.objects, scene_.active, vsel_}; }

void Editor::pushUndo() {
    undo_.push_back(snapshot());
    while (undo_.size() > kMaxUndo) undo_.pop_front();
    redo_.clear();
}

// Groups all changes made through one widget interaction into one undo step.
void Editor::beginEdit(uint32_t widgetId) {
    if (editGroup_ != widgetId) {
        pushUndo();
        editGroup_ = widgetId;
    }
    markDirty();
}

void Editor::markDirty() { dirty_ = true; }

void Editor::restore(Snapshot s) {
    scene_.objects = std::move(s.objects);
    scene_.active = s.active < (int)scene_.objects.size() ? s.active : -1;
    vsel_ = std::move(s.vsel);
    if (mode_ == Mode::Edit && !active()) mode_ = Mode::Object;
    ui_.cancelEdit();
    editGroup_ = 0;
    markDirty();
}

void Editor::undo() {
    if (xf_ != Xform::None) return;
    if (undo_.empty()) {
        setStatus("Nothing to undo");
        return;
    }
    redo_.push_back(snapshot());
    Snapshot s = std::move(undo_.back());
    undo_.pop_back();
    restore(std::move(s));
    setStatus(strf("Undo (%d more)", (int)undo_.size()));
}

void Editor::redo() {
    if (xf_ != Xform::None) return;
    if (redo_.empty()) {
        setStatus("Nothing to redo");
        return;
    }
    undo_.push_back(snapshot());
    Snapshot s = std::move(redo_.back());
    redo_.pop_back();
    restore(std::move(s));
    setStatus("Redo");
}

// ============================================================================
// Actions
// ============================================================================
void Editor::addPrimitive(int kind) {
    if (xf_ != Xform::None) return;
    if (mode_ == Mode::Edit) setMode(Mode::Object);
    pushUndo();
    Mesh m;
    switch (kind) {
        case 0: m = primitives::cube(); break;
        case 1: m = primitives::uvSphere(); break;
        case 2: m = primitives::cylinder(); break;
        case 3: m = primitives::cone(); break;
        case 4: m = primitives::plane(); break;
        default: m = primitives::torus(); break;
    }
    int i = scene_.add(std::move(m), kPrimitiveNames[kind], kPalette[colorCursor_]);
    colorCursor_ = colorCursor_ % 9 + 1;
    scene_.objects[i].position = cam_.target;
    selectOnly(i);
    markDirty();
    setStatus(std::string("Added ") + scene_.objects[i].name);
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
    int newActive = -1, count = 0;
    for (int i = 0; i < n; ++i) {
        if (!scene_.objects[i].selected) continue;
        Object copy = scene_.objects[i];
        copy.id = scene_.nextId++;
        copy.name = scene_.uniqueName(copy.name);
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
        Object* o = active();
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
    scene_.objects.erase(std::remove_if(scene_.objects.begin(), scene_.objects.end(),
                                        [](const Object& o) { return o.selected; }),
                         scene_.objects.end());
    scene_.active = -1;
    markDirty();
    setStatus(strf("Deleted %d object(s)", count));
}

void Editor::subdivideSelected() {
    std::vector<int> targets;
    if (mode_ == Mode::Edit) {
        if (scene_.active >= 0) targets.push_back(scene_.active);
    } else {
        for (int i = 0; i < (int)scene_.objects.size(); ++i)
            if (scene_.objects[i].selected) targets.push_back(i);
    }
    if (targets.empty()) {
        setStatus("Select an object to subdivide", true);
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
    for (int i : targets) scene_.objects[i].mesh = catmullClark(scene_.objects[i].mesh);
    if (mode_ == Mode::Edit) vsel_.clear();
    markDirty();
    setStatus("Subdivided (Catmull-Clark)");
}

void Editor::flipSelected() {
    bool any = false;
    for (int i = 0; i < (int)scene_.objects.size(); ++i)
        if (mode_ == Mode::Edit ? i == scene_.active : scene_.objects[i].selected) any = true;
    if (!any) {
        setStatus("Select an object first", true);
        return;
    }
    pushUndo();
    for (int i = 0; i < (int)scene_.objects.size(); ++i)
        if (mode_ == Mode::Edit ? i == scene_.active : scene_.objects[i].selected) flipNormals(scene_.objects[i].mesh);
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
    Object* o = active();
    if (!o) return;
    auto& sel = vertSel();
    bool wholeFace = false;
    for (const auto& f : o->mesh.faces) {
        bool all = f.size() >= 3;
        for (int v : f) all = all && sel[v];
        if (all) {
            wholeFace = true;
            break;
        }
    }
    if (!wholeFace) {
        setStatus("Select all vertices of at least one face to extrude", true);
        return;
    }
    pushUndo();
    Vec3 n;
    extrudeSelectedFaces(o->mesh, sel, &n);
    markDirty();
    Mat4 normalMatrix = transpose(inverse(o->matrix()));
    xfCustomAxis_ = normalize(transformDir(normalMatrix, n));
    if (beginTransform(Xform::Grab, false) && dot(xfCustomAxis_, xfCustomAxis_) > 0.5f) xfAxis_ = 3;
    setStatus("Extruded - move the mouse, click to confirm");
}

void Editor::setMode(Mode m) {
    if (m == mode_ || xf_ != Xform::None) return;
    if (m == Mode::Edit) {
        Object* o = active();
        if (!o) {
            setStatus("Select an object to edit its vertices", true);
            return;
        }
        vsel_.assign(o->mesh.verts.size(), 0);
        setStatus("Edit mode: click/box-select vertices, G/R/S transform, E extrude, Tab to exit");
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
    if (mode_ == Mode::Edit && active()) {
        Object& o = *active();
        Mat4 m = o.matrix();
        auto& sel = vertSel();
        for (size_t v = 0; v < sel.size(); ++v)
            if (sel[v]) addPoint(transformPoint(m, o.mesh.verts[v]));
        if (!any)
            for (const Vec3& v : o.mesh.verts) addPoint(transformPoint(m, v));
    } else {
        bool onlySelected = scene_.selectedCount() > 0;
        for (const auto& o : scene_.objects) {
            if (onlySelected && !o.selected) continue;
            Mat4 m = o.matrix();
            for (const Vec3& v : o.mesh.verts) addPoint(transformPoint(m, v));
        }
    }
    if (!any) {
        cam_.target = Vec3();
        cam_.distance = 9.0f;
        return;
    }
    float radius = std::max(0.25f, length(hi - lo) * 0.5f);
    cam_.target = (lo + hi) * 0.5f;
    cam_.distance = radius / std::sin(toRadians(cam_.fovY) * 0.5f) * 1.1f;
}

void Editor::setView(float yaw, float pitch) {
    cam_.yaw = yaw;
    cam_.pitch = pitch;
}

void Editor::newScene() {
    if (xf_ != Xform::None) return;
    pushUndo();
    scene_.objects.clear();
    scene_.active = -1;
    mode_ = Mode::Object;
    vsel_.clear();
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
    scene_.active = -1;
    mode_ = Mode::Object;
    vsel_.clear();
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
        setStatus(strf("Exported %d object(s) to %s (+ .mtl)", count, path.c_str()));
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
// Transform tool (G / R / S), modelled on Blender's modal transforms.
// All math happens in world space; edit-mode vertices are converted back to
// object space with the inverse model matrix (FoCG 5e sec. 7.5).
// ============================================================================
bool Editor::beginTransform(Xform type, bool pushUndoState) {
    if (xf_ != Xform::None) return false;
    xfItems_.clear();
    xfPos_.clear();
    xfRot_.clear();
    xfScale_.clear();
    xfLocal_.clear();
    Vec3 sum;
    if (mode_ == Mode::Object) {
        for (int i = 0; i < (int)scene_.objects.size(); ++i) {
            const Object& o = scene_.objects[i];
            if (!o.selected) continue;
            xfItems_.push_back(i);
            xfPos_.push_back(o.position);
            xfRot_.push_back(o.rotation);
            xfScale_.push_back(o.scale);
            sum += o.position;
        }
    } else if (Object* o = active()) {
        auto& sel = vertSel();
        Mat4 m = o->matrix();
        for (int v = 0; v < (int)sel.size(); ++v) {
            if (!sel[v]) continue;
            Vec3 w = transformPoint(m, o->mesh.verts[v]);
            xfItems_.push_back(v);
            xfLocal_.push_back(o->mesh.verts[v]);
            xfPos_.push_back(w);
            sum += w;
        }
    }
    if (xfItems_.empty()) {
        setStatus(mode_ == Mode::Object ? "Select an object first" : "Select some vertices first", true);
        return false;
    }
    xfPivot_ = sum / float(xfItems_.size());
    if (pushUndoState) pushUndo();
    xfPushedUndo_ = pushUndoState;
    xf_ = type;
    xfAxis_ = -1;
    xfStart_ = mouse_;
    Vec2 ps;
    worldToScreen(xfPivot_, ps);
    xfPrevAngle_ = std::atan2(-(mouse_.y - ps.y), mouse_.x - ps.x);
    xfAngle_ = 0;
    lmbInViewport_ = boxSelecting_ = false;
    nav_ = Nav::None;
    updateTransform(Input());
    return true;
}

void Editor::updateTransform(const Input& in) {
    const bool snap = in.ctrl();
    const Vec2 d = mouse_ - xfStart_;
    const Vec3 axis = xfAxis_ < 0 ? Vec3() : (xfAxis_ < 3 ? axisVector(xfAxis_) : xfCustomAxis_);
    const char* axisLabel = xfAxis_ < 0 ? "" : xfAxis_ == 0 ? "along X" : xfAxis_ == 1 ? "along Y" : xfAxis_ == 2 ? "along Z" : "along normal";
    Vec2 ps;
    worldToScreen(xfPivot_, ps);

    Object* editObj = mode_ == Mode::Edit ? active() : nullptr;
    const Mat4 inv = editObj ? inverse(editObj->matrix()) : Mat4();
    auto setVert = [&](int k, Vec3 world) { editObj->mesh.verts[xfItems_[k]] = transformPoint(inv, world); };

    if (xf_ == Xform::Grab) {
        Vec3 delta;
        if (xfAxis_ < 0) {
            // Free move in the view plane, scaled so the item tracks the cursor.
            float k = worldPerPixel(xfPivot_);
            delta = camRight() * (d.x * k) - camUp() * (d.y * k);
            if (snap)
                for (int i = 0; i < 3; ++i) delta[i] = std::round(delta[i] * 4.0f) / 4.0f;
        } else {
            // Project the mouse motion onto the axis' on-screen direction.
            float step = std::max(1e-4f, worldPerPixel(xfPivot_) * 50.0f);
            Vec2 s0, s1;
            float t = 0.0f;
            if (worldToScreen(xfPivot_, s0) && worldToScreen(xfPivot_ + axis * step, s1)) {
                Vec2 sd = (s1 - s0) * (1.0f / step);
                float len2 = dot(sd, sd);
                if (len2 > 1e-8f) t = dot(d, sd) / len2;
            }
            if (snap) t = std::round(t * 4.0f) / 4.0f;
            delta = axis * t;
        }
        for (size_t k = 0; k < xfItems_.size(); ++k) {
            if (editObj) setVert((int)k, xfPos_[k] + delta);
            else scene_.objects[xfItems_[k]].position = xfPos_[k] + delta;
        }
        xfInfo_ = strf("Move %s  (%.3f, %.3f, %.3f)", axisLabel, delta.x, delta.y, delta.z);
    } else if (xf_ == Xform::Rotate) {
        float a = std::atan2(-(mouse_.y - ps.y), mouse_.x - ps.x);
        xfAngle_ += wrapRadians(a - xfPrevAngle_);
        xfPrevAngle_ = a;
        float deg = toDegrees(xfAngle_);
        if (snap) deg = std::round(deg / 15.0f) * 15.0f;
        Vec3 towardViewer = -camForward();
        Vec3 rotAxis = xfAxis_ < 0 ? towardViewer : axis;
        // Keep the rotation following the cursor even when the axis points away.
        float signedDeg = (xfAxis_ >= 0 && dot(axis, towardViewer) < 0) ? -deg : deg;
        Mat4 R = rotationAxis(rotAxis, toRadians(signedDeg));
        for (size_t k = 0; k < xfItems_.size(); ++k) {
            Vec3 p = xfPivot_ + transformDir(R, xfPos_[k] - xfPivot_);
            if (editObj) {
                setVert((int)k, p);
            } else {
                Object& o = scene_.objects[xfItems_[k]];
                o.rotation = matrixToEuler(R * eulerToMatrix(xfRot_[k]));
                o.position = p;
            }
        }
        xfInfo_ = strf("Rotate %s  %.1f deg", axisLabel, deg);
    } else if (xf_ == Xform::Scale) {
        // Reference distance from the pivot; floored so starting right on the
        // pivot doesn't make the scale explode.
        float d0 = std::max(length(xfStart_ - ps), 40.0f * dpi_);
        float f = length(mouse_ - ps) / d0;
        if (snap) f = std::round(f * 10.0f) / 10.0f;
        for (size_t k = 0; k < xfItems_.size(); ++k) {
            Vec3 rel = xfPos_[k] - xfPivot_;
            if (xfAxis_ < 0) rel *= f;
            else if (xfAxis_ < 3) rel[xfAxis_] *= f;
            else rel += axis * (dot(rel, axis) * (f - 1.0f));
            if (editObj) {
                setVert((int)k, xfPivot_ + rel);
            } else {
                Object& o = scene_.objects[xfItems_[k]];
                o.position = xfPivot_ + rel;
                o.scale = xfScale_[k];
                if (xfAxis_ < 0) o.scale *= f;
                else if (xfAxis_ < 3) o.scale[xfAxis_] *= f;
            }
        }
        xfInfo_ = strf("Scale %s  x%.3f", axisLabel, f);
    }
    if (editObj) editObj->mesh.touch();
}

void Editor::endTransform(bool confirm) {
    if (xf_ == Xform::None) return;
    if (!confirm) {
        if (mode_ == Mode::Object) {
            for (size_t k = 0; k < xfItems_.size(); ++k) {
                Object& o = scene_.objects[xfItems_[k]];
                o.position = xfPos_[k];
                o.rotation = xfRot_[k];
                o.scale = xfScale_[k];
            }
        } else if (Object* o = active()) {
            for (size_t k = 0; k < xfItems_.size(); ++k) o->mesh.verts[xfItems_[k]] = xfLocal_[k];
            o->mesh.touch();
        }
        if (xfPushedUndo_ && !undo_.empty()) undo_.pop_back();
        setStatus("Transform cancelled");
    } else {
        markDirty();
        setStatus(xfInfo_);
    }
    xf_ = Xform::None;
}

// ============================================================================
// Input
// ============================================================================
void Editor::handleShortcuts(const Input& in) {
    if (xf_ != Xform::None) return;
    if (in.ctrl()) {
        if (in.pressed('Z')) {
            if (in.shift()) redo();
            else undo();
        }
        if (in.pressed('Y')) redo();
        if (in.pressed('S')) saveFile();
        if (in.pressed('O')) loadFile();
        if (in.pressed('1')) setView(180, 0);
        if (in.pressed('3')) setView(-90, 0);
        if (in.pressed('7')) setView(0, -89.9f);
        return;
    }
    if (in.pressed(KEY_F1) || in.pressed('H')) showHelp_ = true;
    if (in.pressed(KEY_TAB)) setMode(mode_ == Mode::Object ? Mode::Edit : Mode::Object);
    if (in.pressed('G')) beginTransform(Xform::Grab, true);
    if (in.pressed('R')) beginTransform(Xform::Rotate, true);
    if (in.pressed('S')) beginTransform(Xform::Scale, true);
    if (in.pressed('D') && in.shift()) duplicateSelected();
    if (in.pressed('X') || in.pressed(KEY_DELETE)) deleteSelected();
    if (in.pressed('A')) selectAll();
    if (in.pressed('E')) extrude();
    if (in.pressed('F')) frameSelected();
    if (in.pressed('1')) setView(0, 0);
    if (in.pressed('3')) setView(90, 0);
    if (in.pressed('7')) setView(0, 89.9f);
    if (in.pressed('5')) {
        cam_.ortho = !cam_.ortho;
        setStatus(cam_.ortho ? "Orthographic view" : "Perspective view");
    }
    if (in.pressed('W')) wireframe_ = !wireframe_;
    if (in.pressed(KEY_F12)) screenshotPending_ = true;
}

void Editor::saveScreenshot() {
    screenshotPending_ = false;
    std::string path;
    for (int i = 1; i < 10000; ++i) {
        path = strf("screenshot_%03d.png", i);
        if (FILE* f = std::fopen(path.c_str(), "rb")) std::fclose(f);
        else break;
    }
    std::vector<uint8_t> pixels;
    renderer_.readPixels(screenW_, screenH_, pixels);
    if (writePNG(path, screenW_, screenH_, pixels)) setStatus("Saved screenshot " + path);
    else setStatus("Could not write " + path, true);
}

void Editor::handleViewport(const Input& in) {
    const bool over = viewport_.contains(in.mouseX, in.mouseY) && !ui_.isActive();
    if (over && in.wheel != 0.0f) cam_.distance = clampf(cam_.distance * std::pow(0.85f, in.wheel), 0.05f, 5000.0f);

    if (xf_ != Xform::None) {
        for (int a = 0; a < 3; ++a)
            if (in.pressed('X' + a)) xfAxis_ = xfAxis_ == a ? -1 : a;
        if (in.mousePressed[MOUSE_LEFT] || in.pressed(KEY_ENTER) || in.pressed(KEY_SPACE)) {
            updateTransform(in);
            endTransform(true);
        } else if (in.mousePressed[MOUSE_RIGHT] || in.pressed(KEY_ESCAPE)) {
            endTransform(false);
        } else {
            updateMatrices();
            updateTransform(in);
        }
        return;
    }

    // Camera navigation: orbit (RMB / MMB / Alt+LMB), pan with Shift.
    if (nav_ == Nav::None && over) {
        int b = -1;
        if (in.mousePressed[MOUSE_MIDDLE]) b = MOUSE_MIDDLE;
        else if (in.mousePressed[MOUSE_RIGHT]) b = MOUSE_RIGHT;
        else if (in.mousePressed[MOUSE_LEFT] && in.alt()) b = MOUSE_LEFT;
        if (b >= 0) {
            nav_ = Nav::Orbit;
            navButton_ = b;
        }
    }
    if (nav_ != Nav::None) {
        if (!in.mouseDown[navButton_]) {
            nav_ = Nav::None;
        } else {
            nav_ = in.shift() ? Nav::Pan : Nav::Orbit;
            if (nav_ == Nav::Orbit) {
                cam_.yaw -= in.mouseDX * 0.4f;
                cam_.pitch = clampf(cam_.pitch + in.mouseDY * 0.4f, -89.9f, 89.9f);
            } else {
                float k = worldPerPixel(cam_.target);
                cam_.target -= camRight() * (in.mouseDX * k);
                cam_.target += camUp() * (in.mouseDY * k);
            }
            updateMatrices();
        }
        return;
    }

    // Selection: click, or drag a box.
    if (over && in.mousePressed[MOUSE_LEFT]) {
        lmbInViewport_ = true;
        boxSelecting_ = false;
        pressPos_ = mouse_;
    }
    if (lmbInViewport_) {
        if (in.mouseDown[MOUSE_LEFT]) {
            if (length(mouse_ - pressPos_) > 4.0f * dpi_) boxSelecting_ = true;
        } else {
            if (boxSelecting_) boxSelect(pressPos_, mouse_, in.shift(), in.ctrl());
            else clickSelect(mouse_, in.shift());
            lmbInViewport_ = boxSelecting_ = false;
        }
    }
}

void Editor::clickSelect(Vec2 p, bool extend) {
    if (mode_ == Mode::Edit) {
        auto& sel = vertSel();
        int v = pickVertex(p);
        if (!extend) std::fill(sel.begin(), sel.end(), 0);
        if (v >= 0) sel[v] = extend ? !sel[v] : 1;
        return;
    }
    Vec3 o, d;
    viewRay(p, o, d);
    int hit = pickObject(scene_, o, d);
    if (!extend) selectOnly(hit);
    else if (hit >= 0) toggleSelect(hit);
}

int Editor::pickVertex(Vec2 p) {
    Object* o = active();
    if (!o) return -1;
    const Mat4 m = o->matrix(), inv = inverse(m);
    const float radius = 12.0f * dpi_;
    std::vector<std::pair<float, int>> candidates;
    for (int v = 0; v < (int)o->mesh.verts.size(); ++v) {
        Vec2 s;
        if (!worldToScreen(transformPoint(m, o->mesh.verts[v]), s)) continue;
        float dist = length(s - p);
        if (dist <= radius) candidates.push_back({dist, v});
    }
    std::sort(candidates.begin(), candidates.end());
    if (wireframe_) return candidates.empty() ? -1 : candidates[0].second;  // x-ray: ignore occlusion

    // Prefer the closest vertex that is not hidden behind the mesh itself:
    // cast a ray from the eye to the vertex and look for an earlier hit.
    const Vec3 eye = cam_.eye();
    for (size_t c = 0; c < candidates.size() && c < 32; ++c) {
        Vec3 w = transformPoint(m, o->mesh.verts[candidates[c].second]);
        Vec3 origin = cam_.ortho ? w - camForward() * (cam_.distance * 4.0f + 100.0f) : eye;
        Vec3 dir = w - origin;  // t = 1 at the vertex
        float tolerance = (0.002f * cam_.distance + 1e-4f) / std::max(1e-6f, length(dir));
        float t;
        bool hidden = raycastMesh(o->mesh, transformPoint(inv, origin), transformDir(inv, dir), t) && t < 1.0f - tolerance;
        if (!hidden) return candidates[c].second;
    }
    return -1;
}

void Editor::boxSelect(Vec2 a, Vec2 b, bool extend, bool subtract) {
    const float x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
    auto inside = [&](Vec3 world) {
        Vec2 s;
        return worldToScreen(world, s) && s.x >= x0 && s.x <= x1 && s.y >= y0 && s.y <= y1;
    };
    if (mode_ == Mode::Edit) {
        Object* o = active();
        if (!o) return;
        auto& sel = vertSel();
        if (!extend && !subtract) std::fill(sel.begin(), sel.end(), 0);
        Mat4 m = o->matrix();
        for (size_t v = 0; v < sel.size(); ++v)
            if (inside(transformPoint(m, o->mesh.verts[v]))) sel[v] = subtract ? 0 : 1;
        return;
    }
    if (!extend && !subtract) selectOnly(-1);
    for (int i = 0; i < (int)scene_.objects.size(); ++i) {
        Object& o = scene_.objects[i];
        if (o.mesh.verts.empty()) continue;
        Vec3 lo = o.mesh.verts[0], hi = lo;
        for (const Vec3& v : o.mesh.verts) {
            lo = vmin(lo, v);
            hi = vmax(hi, v);
        }
        if (!inside(transformPoint(o.matrix(), (lo + hi) * 0.5f))) continue;
        if (subtract) {
            o.selected = false;
            if (scene_.active == i) scene_.active = -1;
        } else {
            o.selected = true;
            scene_.active = i;
        }
    }
}

// ============================================================================
// UI panels
// ============================================================================
bool Editor::vec3Fields(float& y, float x, float w, const char* key, uint32_t salt, Vec3& v, float speed, float lo,
                        float hi, const Color* colors) {
    const float gap = 3.0f * fontScale_, h = ui_.rowHeight();
    Rect row{x, y, w, h};
    y += h + gap;
    bool changed = false;
    for (int i = 0; i < 3; ++i) {
        float value = v[i];
        uint32_t id = uiHash(key, salt * 4u + (uint32_t)i);
        if (ui_.dragFloat(id, cell(row, i, 3, gap), value, speed, colors[i], lo, hi)) {
            beginEdit(id);
            v[i] = value;
            changed = true;
        }
    }
    return changed;
}

void Editor::buildLeftPanel(const Input& in) {
    const float fs = (float)fontScale_;
    const float pad = 5 * fs, gap = 3 * fs, rowH = ui_.rowHeight(), headerH = 11 * fs;
    ui_.rect(leftPanel_, theme::panel);
    if (ui_.inputEnabled() && leftPanel_.contains(in.mouseX, in.mouseY) && in.wheel != 0.0f)
        leftScroll_ -= in.wheel * rowH * 2;
    leftScroll_ = clampf(leftScroll_, 0.0f, std::max(0.0f, leftContentH_ - leftPanel_.h));
    ui_.pushClip(leftPanel_);
    Layout L{leftPanel_.x + pad, leftPanel_.y + pad - leftScroll_, leftPanel_.w - 2 * pad, gap};
    const float top = L.y;

    Rect title = L.row(rowH);
    ui_.text(title.x, title.y + (rowH - ui_.glyphH()) * 0.5f, "MODELER 3D", theme::accent);

    ui_.header(L.row(headerH), "ADD OBJECT");
    for (int r = 0; r < 3; ++r) {
        Rect row = L.row(rowH);
        for (int c = 0; c < 2; ++c) {
            int k = r * 2 + c;
            if (ui_.button(uiHash("add", k), cell(row, c, 2, gap), kPrimitiveNames[k])) addPrimitive(k);
        }
    }

    ui_.header(L.row(headerH), "MODE  (Tab)");
    Rect row = L.row(rowH);
    if (ui_.button(uiHash("mode.object"), cell(row, 0, 2, gap), "Object", mode_ == Mode::Object)) setMode(Mode::Object);
    if (ui_.button(uiHash("mode.edit"), cell(row, 1, 2, gap), "Edit", mode_ == Mode::Edit)) setMode(Mode::Edit);

    ui_.header(L.row(headerH), "TRANSFORM  (G R S)");
    row = L.row(rowH);
    if (ui_.button(uiHash("xf.move"), cell(row, 0, 3, gap), "Move")) beginTransform(Xform::Grab, true);
    if (ui_.button(uiHash("xf.rotate"), cell(row, 1, 3, gap), "Rotate")) beginTransform(Xform::Rotate, true);
    if (ui_.button(uiHash("xf.scale"), cell(row, 2, 3, gap), "Scale")) beginTransform(Xform::Scale, true);

    if (mode_ == Mode::Object) {
        ui_.header(L.row(headerH), "OBJECT");
        row = L.row(rowH);
        if (ui_.button(uiHash("obj.dup"), cell(row, 0, 2, gap), "Duplicate")) duplicateSelected();
        if (ui_.button(uiHash("obj.del"), cell(row, 1, 2, gap), "Delete")) deleteSelected();
    } else {
        ui_.header(L.row(headerH), "MESH");
        row = L.row(rowH);
        if (ui_.button(uiHash("mesh.all"), cell(row, 0, 2, gap), "All/None")) selectAll();
        if (ui_.button(uiHash("mesh.extrude"), cell(row, 1, 2, gap), "Extrude")) extrude();
        row = L.row(rowH);
        if (ui_.button(uiHash("mesh.del"), cell(row, 0, 2, gap), "Delete")) deleteSelected();
        if (ui_.button(uiHash("mesh.frame"), cell(row, 1, 2, gap), "Frame")) frameSelected();
    }
    row = L.row(rowH);
    if (ui_.button(uiHash("obj.subdiv"), cell(row, 0, 2, gap), "Subdivide")) subdivideSelected();
    if (ui_.button(uiHash("obj.flip"), cell(row, 1, 2, gap), "Flip")) flipSelected();

    ui_.header(L.row(headerH), "VIEW");
    row = L.row(rowH);
    if (ui_.button(uiHash("view.front"), cell(row, 0, 3, gap), "Front")) setView(0, 0);
    if (ui_.button(uiHash("view.right"), cell(row, 1, 3, gap), "Right")) setView(90, 0);
    if (ui_.button(uiHash("view.top"), cell(row, 2, 3, gap), "Top")) setView(0, 89.9f);
    row = L.row(rowH);
    if (ui_.button(uiHash("view.ortho"), cell(row, 0, 2, gap), "Ortho", cam_.ortho)) cam_.ortho = !cam_.ortho;
    if (ui_.button(uiHash("view.frame"), cell(row, 1, 2, gap), "Frame")) frameSelected();
    row = L.row(rowH);
    if (ui_.button(uiHash("view.wire"), cell(row, 0, 2, gap), "Wireframe", wireframe_)) wireframe_ = !wireframe_;
    if (ui_.button(uiHash("view.grid"), cell(row, 1, 2, gap), "Grid", showGrid_)) showGrid_ = !showGrid_;

    ui_.header(L.row(headerH), "HISTORY");
    row = L.row(rowH);
    if (ui_.button(uiHash("undo"), cell(row, 0, 2, gap), "Undo")) undo();
    if (ui_.button(uiHash("redo"), cell(row, 1, 2, gap), "Redo")) redo();

    ui_.header(L.row(headerH), "FILE");
    ui_.textField(uiHash("file.name"), L.row(rowH), fileField_);
    row = L.row(rowH);
    if (ui_.button(uiHash("file.new"), cell(row, 0, 3, gap), "New")) newScene();
    if (ui_.button(uiHash("file.save"), cell(row, 1, 3, gap), "Save")) saveFile();
    if (ui_.button(uiHash("file.load"), cell(row, 2, 3, gap), "Load")) loadFile();
    ui_.header(L.row(headerH), "WAVEFRONT .OBJ");
    row = L.row(rowH);
    if (ui_.button(uiHash("obj.export"), cell(row, 0, 2, gap), "Export")) exportObj();
    if (ui_.button(uiHash("obj.import"), cell(row, 1, 2, gap), "Import")) importObj();

    L.y += gap * 2;
    if (ui_.button(uiHash("help"), L.row(rowH), "Help  (F1)", showHelp_)) showHelp_ = true;

    leftContentH_ = (L.y - top) + 2 * pad;
    ui_.popClip();
    ui_.rect({leftPanel_.x + leftPanel_.w - 1, leftPanel_.y, 1, leftPanel_.h}, theme::border);
}

void Editor::buildRightPanel(const Input& in) {
    const float fs = (float)fontScale_;
    const float pad = 5 * fs, gap = 3 * fs, rowH = ui_.rowHeight(), headerH = 11 * fs, labelH = 9 * fs;
    ui_.rect(rightPanel_, theme::panel);
    if (ui_.inputEnabled() && in.wheel != 0.0f) {
        if (outlinerRect_.contains(in.mouseX, in.mouseY)) outlinerScroll_ -= in.wheel * rowH * 2;
        else if (rightPanel_.contains(in.mouseX, in.mouseY)) rightScroll_ -= in.wheel * rowH * 2;
    }
    rightScroll_ = clampf(rightScroll_, 0.0f, std::max(0.0f, rightContentH_ - rightPanel_.h));
    ui_.pushClip(rightPanel_);
    Layout L{rightPanel_.x + pad, rightPanel_.y + pad - rightScroll_, rightPanel_.w - 2 * pad, gap};
    const float top = L.y;
    auto label = [&](const std::string& s) {
        Rect r = L.row(labelH);
        ui_.text(r.x, r.y + fs, s, theme::textDim);
    };

    // --- Outliner ---
    const int count = (int)scene_.objects.size();
    ui_.header(L.row(headerH), strf("SCENE  (%d)", count));
    int visibleRows = std::max(4, std::min(count, 8));
    Rect list = L.row(visibleRows * rowH);
    outlinerRect_ = list;
    ui_.rect(list, theme::field);
    outlinerScroll_ = clampf(outlinerScroll_, 0.0f, std::max(0.0f, count * rowH - list.h));
    ui_.pushClip(list);
    int clicked = -1;
    for (int i = 0; i < count; ++i) {
        Rect r{list.x, list.y + i * rowH - outlinerScroll_, list.w, rowH};
        if (r.y + r.h < list.y || r.y > list.y + list.h) continue;
        const Object& o = scene_.objects[i];
        if (ui_.selectable(uiHash("outliner", o.id), r, o.name, o.selected, i == scene_.active)) clicked = i;
    }
    if (count == 0) ui_.textIn(list, "(empty scene)", theme::textDim, true);
    ui_.popClip();
    if (clicked >= 0) {
        if (mode_ == Mode::Edit) {
            if (clicked != scene_.active) {
                selectOnly(clicked);
                vsel_.assign(scene_.objects[clicked].mesh.verts.size(), 0);
            }
        } else if (in.shift()) {
            toggleSelect(clicked);
        } else {
            selectOnly(clicked);
        }
    }

    // --- Properties of the active object ---
    ui_.header(L.row(headerH), "PROPERTIES");
    Object* o = active();
    if (!o) {
        label("No active object.");
        label("Click one in the viewport");
        label("or in the list above.");
    } else {
        Rect r = L.row(rowH);
        const float labelW = 34 * fs;
        ui_.textIn({r.x, r.y, labelW, r.h}, "Name", theme::textDim, false);
        std::string name = o->name;
        uint32_t nameId = uiHash("name", o->id);
        if (ui_.textField(nameId, {r.x + labelW, r.y, r.w - labelW, r.h}, name)) {
            name = trimmed(name);
            if (!name.empty() && name != o->name) {
                beginEdit(nameId);
                o->name = scene_.uniqueName(name, scene_.active);
            }
        }

        label("Location");
        vec3Fields(L.y, L.x, L.w, "loc", o->id, o->position, 0.02f, -1e6f, 1e6f, kAxisColor);
        label("Rotation (degrees)");
        vec3Fields(L.y, L.x, L.w, "rot", o->id, o->rotation, 0.5f, -1e6f, 1e6f, kAxisColor);
        label("Scale");
        vec3Fields(L.y, L.x, L.w, "scl", o->id, o->scale, 0.01f, -1e4f, 1e4f, kAxisColor);

        label("Color (R G B)");
        const Color rgbColors[3] = {{1, 0.25f, 0.25f, 1}, {0.3f, 0.9f, 0.3f, 1}, {0.3f, 0.5f, 1, 1}};
        vec3Fields(L.y, L.x, L.w, "col", o->id, o->color, 0.004f, 0.0f, 1.0f, rgbColors);
        for (int rr = 0; rr < 2; ++rr) {
            Rect sw = L.row(std::floor(rowH * 0.8f));
            for (int c = 0; c < 5; ++c) {
                int idx = rr * 5 + c;
                bool same = length(o->color - kPalette[idx]) < 1e-3f;
                uint32_t id = uiHash("swatch", (uint32_t)idx);
                if (ui_.swatch(id, cell(sw, c, 5, gap), toColor(kPalette[idx]), same)) {
                    beginEdit(id);
                    for (int i = 0; i < count; ++i)
                        if (scene_.objects[i].selected || i == scene_.active) scene_.objects[i].color = kPalette[idx];
                }
            }
        }

        label("Shading");
        r = L.row(rowH);
        if (ui_.button(uiHash("shade.flat"), cell(r, 0, 2, gap), "Flat", !o->smooth) && o->smooth) setSmoothSelected(false);
        if (ui_.button(uiHash("shade.smooth"), cell(r, 1, 2, gap), "Smooth", o->smooth) && !o->smooth)
            setSmoothSelected(true);

        o = active();  // (pointer stays valid, but re-fetch for clarity after actions)
        if (o) {
            label(strf("%d verts, %d faces", (int)o->mesh.verts.size(), (int)o->mesh.faces.size()));
        }

        if (mode_ == Mode::Edit && o) {
            ui_.header(L.row(headerH), "SELECTED VERTICES");
            auto& sel = vertSel();
            int n = 0;
            Vec3 median;
            for (size_t v = 0; v < sel.size(); ++v)
                if (sel[v]) {
                    median += o->mesh.verts[v];
                    ++n;
                }
            label(strf("%d of %d selected", n, (int)sel.size()));
            if (n > 0) {
                median = median / float(n);
                label("Median (object space)");
                Vec3 edited = median;
                if (vec3Fields(L.y, L.x, L.w, "median", o->id, edited, 0.01f, -1e6f, 1e6f, kAxisColor)) {
                    Vec3 delta = edited - median;
                    for (size_t v = 0; v < sel.size(); ++v)
                        if (sel[v]) o->mesh.verts[v] += delta;
                    o->mesh.touch();
                }
            }
        }
    }

    rightContentH_ = (L.y - top) + 2 * pad;
    ui_.popClip();
    ui_.rect({rightPanel_.x, rightPanel_.y, 1, rightPanel_.h}, theme::border);
}

void Editor::buildStatusBar() {
    const float fs = (float)fontScale_;
    ui_.rect(statusBar_, theme::panelDark);
    ui_.rect({statusBar_.x, statusBar_.y, statusBar_.w, 1}, theme::border);
    std::string right = strf("%s mode | %d objects | %d tris | %.0f fps", mode_ == Mode::Edit ? "Edit" : "Object",
                             (int)scene_.objects.size(), (int)scene_.triangleCount(), fps_);
    float rw = ui_.textWidth(right) + 10 * fs;
    std::string msg;
    Color c = theme::text;
    if (xf_ != Xform::None) {
        msg = xfInfo_ + "   X/Y/Z axis | Ctrl snap | LMB confirm | RMB cancel";
        c = theme::selection;
    } else if (!status_.empty() && statusTime_ < 6.0f) {
        msg = status_;
        c = statusError_ ? theme::error : theme::text;
    } else if (mode_ == Mode::Edit) {
        msg = "Click/drag: select vertices | A all | G/R/S transform | E extrude | X delete | Tab: object mode";
        c = theme::textDim;
    } else {
        msg = "Click: select | RMB drag: orbit | Shift+RMB: pan | Wheel: zoom | G/R/S transform | F1 help";
        c = theme::textDim;
    }
    ui_.textIn({statusBar_.x + 2 * fs, statusBar_.y, statusBar_.w - rw - 4 * fs, statusBar_.h}, msg, c, false);
    ui_.textIn({statusBar_.x + statusBar_.w - rw, statusBar_.y, rw, statusBar_.h}, right, theme::textDim, false);
}

void Editor::buildViewportOverlay() {
    const float fs = (float)fontScale_;
    ui_.pushClip(viewport_);
    const float x = viewport_.x + 6 * fs;
    float y = viewport_.y + 6 * fs;
    std::string info = std::string(cam_.ortho ? "Orthographic" : "Perspective") +
                       (mode_ == Mode::Edit ? "  |  Edit Mode" : "  |  Object Mode");
    ui_.text(x, y, info, withAlpha(theme::text, 0.8f));
    y += 10 * fs;
    if (mode_ == Mode::Edit && active()) {
        ui_.text(x, y, "Editing: " + active()->name, withAlpha(theme::selection, 0.9f));
        y += 10 * fs;
    }
    if (wireframe_) {
        ui_.text(x, y, "Wireframe (x-ray selection)", withAlpha(theme::textDim, 0.9f));
        y += 10 * fs;
    }
    if (xf_ != Xform::None) ui_.text(x, y, xfInfo_, theme::selection);

    if (scene_.objects.empty()) {
        const std::string msg = "Empty scene - add an object with the buttons on the left";
        ui_.text(viewport_.x + (viewport_.w - ui_.textWidth(msg)) * 0.5f, viewport_.y + viewport_.h * 0.5f, msg,
                 theme::textDim);
    }

    if (boxSelecting_) {
        Rect r{viewport_.x + std::min(pressPos_.x, mouse_.x), viewport_.y + std::min(pressPos_.y, mouse_.y),
               std::fabs(mouse_.x - pressPos_.x), std::fabs(mouse_.y - pressPos_.y)};
        ui_.rect(r, {1, 1, 1, 0.06f});
        ui_.border(r, {1, 1, 1, 0.6f}, 1.0f);
    }

    // Orientation gizmo: world axes rotated into view space.
    const float cx = viewport_.x + 28 * fs, cy = viewport_.y + viewport_.h - 28 * fs, len = 18 * fs;
    struct AxisDraw {
        int i;
        float x, y, z;
    };
    AxisDraw axes[3];
    for (int i = 0; i < 3; ++i) axes[i] = {i, view_(0, i), -view_(1, i), view_(2, i)};
    std::sort(axes, axes + 3, [](const AxisDraw& a, const AxisDraw& b) { return a.z < b.z; });
    ui_.rect({cx - 2 * fs, cy - 2 * fs, 4 * fs, 4 * fs}, withAlpha(theme::text, 0.5f));
    for (const AxisDraw& a : axes) {
        float alpha = a.z < -0.2f ? 0.45f : 1.0f;
        float ex = cx + a.x * len, ey = cy + a.y * len;
        Color col = withAlpha(kAxisColor[a.i], alpha);
        ui_.line(cx, cy, ex, ey, 1.5f * fs, col);
        float r = 5 * fs;
        ui_.rect({ex - r, ey - r, 2 * r, 2 * r}, col);
        const char* name = a.i == 0 ? "X" : a.i == 1 ? "Y" : "Z";
        ui_.text(ex - 2.5f * fs, ey - 3.5f * fs, name, withAlpha(theme::panelDark, alpha));
    }
    ui_.popClip();
}

void Editor::buildHelp() {
    static const char* const kRows[][2] = {
        {"MOUSE", ""},
        {"Left click", "Select (Shift: add / toggle)"},
        {"Left drag", "Box select (Shift: add, Ctrl: remove)"},
        {"Right / middle drag", "Orbit the camera (or Alt + left drag)"},
        {"Shift + right drag", "Pan the camera"},
        {"Mouse wheel", "Zoom"},
        {"", ""},
        {"KEYBOARD", ""},
        {"G / R / S", "Move / rotate / scale the selection"},
        {"  then X / Y / Z", "Lock to an axis (press again to unlock)"},
        {"  Ctrl (hold)", "Snap: 0.25 units, 15 degrees, 0.1 scale"},
        {"  Left click / Enter", "Confirm     Right click / Esc: cancel"},
        {"Tab", "Toggle Object / Edit (vertex) mode"},
        {"A", "Select all / nothing"},
        {"E", "Extrude selected faces (Edit mode)"},
        {"Shift + D", "Duplicate selected objects"},
        {"X / Delete", "Delete selection"},
        {"F", "Frame selection"},
        {"1 / 3 / 7", "Front / right / top view (Ctrl: opposite)"},
        {"5", "Perspective / orthographic"},
        {"W", "Wireframe overlay + x-ray vertex picking"},
        {"Ctrl+Z / Ctrl+Y", "Undo / redo"},
        {"Ctrl+S / Ctrl+O", "Save / load the scene named in FILE"},
        {"F12", "Save a screenshot (PNG)"},
        {"F1 or H", "This help"},
    };
    const int n = (int)(sizeof kRows / sizeof kRows[0]);
    const float fs = (float)fontScale_;
    const float pad = 10 * fs, lineH = 10 * fs;
    const float keyW = 23 * 6 * fs, descW = 42 * 6 * fs;
    const float w = keyW + descW + 2 * pad, h = (n + 3) * lineH + 2 * pad;
    ui_.rect({0, 0, (float)screenW_, (float)screenH_}, {0, 0, 0, 0.55f});
    Rect box{std::floor((screenW_ - w) * 0.5f), std::floor(std::max(0.0f, (screenH_ - h) * 0.5f)), w, h};
    ui_.rect(box, theme::panel);
    ui_.border(box, theme::accent, fs);
    float y = box.y + pad;
    ui_.text(box.x + pad, y, "MODELER 3D  -  CONTROLS", theme::accent);
    y += lineH * 1.5f;
    for (int i = 0; i < n; ++i) {
        bool section = kRows[i][1][0] == '\0';
        ui_.text(box.x + pad, y, kRows[i][0], section ? theme::textDim : theme::white);
        ui_.text(box.x + pad + keyW, y, kRows[i][1], theme::text);
        y += lineH;
    }
    ui_.text(box.x + pad, box.y + box.h - pad - ui_.glyphH(), "Click or press any key to close.", theme::textDim);
}

void Editor::buildQuitDialog() {
    const float fs = (float)fontScale_;
    const float pad = 8 * fs, gap = 4 * fs, rowH = ui_.rowHeight();
    const float w = 210 * fs, h = 2 * pad + 3 * rowH;
    ui_.rect({0, 0, (float)screenW_, (float)screenH_}, {0, 0, 0, 0.55f});
    Rect box{std::floor((screenW_ - w) * 0.5f), std::floor((screenH_ - h) * 0.5f), w, h};
    ui_.rect(box, theme::panel);
    ui_.border(box, theme::accent, fs);
    ui_.textIn({box.x + pad, box.y + pad, w - 2 * pad, rowH}, "Save changes before quitting?", theme::white, false);
    Rect row{box.x + pad, box.y + pad + 1.6f * rowH, w - 2 * pad, rowH};
    if (ui_.button(uiHash("quit.save"), cell(row, 0, 3, gap), "Save", true)) {
        saveFile();
        confirmQuit_ = false;
        if (!dirty_) exit_ = true;
    }
    if (ui_.button(uiHash("quit.discard"), cell(row, 1, 3, gap), "Don't Save")) exit_ = true;
    if (ui_.button(uiHash("quit.cancel"), cell(row, 2, 3, gap), "Cancel")) confirmQuit_ = false;
}

// ============================================================================
// Rendering
// ============================================================================
void Editor::renderViewport() {
    const int vx = (int)viewport_.x, vy = (int)(screenH_ - (viewport_.y + viewport_.h));
    renderer_.beginViewport(vx, vy, (int)viewport_.w, (int)viewport_.h, kViewportBg);

    FrameParams f;
    f.viewProj = viewProj_;
    f.cameraPos = cam_.eye();
    const Vec3 r = camRight(), u = camUp(), back = -camForward();
    f.keyLightDir = normalize(r * -0.45f + u * 0.75f + back * 0.6f);   // over the left shoulder
    f.fillLightDir = normalize(r * 0.7f - u * 0.15f + back * 0.4f);

    const int count = (int)scene_.objects.size();
    for (int i = 0; i < count; ++i) {
        const Object& o = scene_.objects[i];
        float highlight = (mode_ == Mode::Object && o.selected) ? (i == scene_.active ? 1.0f : 0.6f) : 0.0f;
        renderer_.drawObject(o, f, highlight);
    }

    if (showGrid_) renderer_.drawLines(grid_, f, Mat4(), true, std::max(20.0f, cam_.distance * 3.0f), cam_.target);

    for (int i = 0; i < count; ++i) {
        const Object& o = scene_.objects[i];
        if (mode_ == Mode::Edit && i == scene_.active) continue;
        if (mode_ == Mode::Object && o.selected)
            renderer_.drawObjectEdges(o, f, withAlpha(theme::selection, i == scene_.active ? 0.9f : 0.55f));
        else if (wireframe_)
            renderer_.drawObjectEdges(o, f, {0.02f, 0.02f, 0.03f, 0.5f});
    }

    if (mode_ == Mode::Edit && active()) {
        const Object& o = *active();
        const auto& sel = vertSel();
        if (editEdgesVersion_ != o.mesh.version) {
            editEdges_ = uniqueEdges(o.mesh);
            editEdgesVersion_ = o.mesh.version;
        }
        std::vector<LineVertex> lines;
        lines.reserve(editEdges_.size() * 2);
        for (auto [a, b] : editEdges_) {
            Color c = (sel[a] && sel[b]) ? theme::selection : kEdgeDark;
            lines.push_back({o.mesh.verts[a], c});
            lines.push_back({o.mesh.verts[b], c});
        }
        Mat4 m = o.matrix();
        renderer_.drawLines(lines, f, m, !wireframe_);
        std::vector<LineVertex> points;
        points.reserve(o.mesh.verts.size());
        for (size_t v = 0; v < o.mesh.verts.size(); ++v)
            points.push_back({o.mesh.verts[v], sel[v] ? theme::selection : Color{0.05f, 0.05f, 0.07f, 1}});
        renderer_.drawPoints(points, f, m, 3.5f * fontScale_, !wireframe_);
    }

    if (xf_ != Xform::None && xfAxis_ >= 0) {
        Vec3 a = xfAxis_ < 3 ? axisVector(xfAxis_) : xfCustomAxis_;
        Color c = xfAxis_ < 3 ? kAxisColor[xfAxis_] : kCustomAxisColor;
        std::vector<LineVertex> guide = {{xfPivot_ - a * 1000.0f, c}, {xfPivot_ + a * 1000.0f, c}};
        renderer_.drawLines(guide, f, Mat4(), false);
    }
}
