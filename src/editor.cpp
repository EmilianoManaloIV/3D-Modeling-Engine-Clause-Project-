#include "editor_internal.h"
#include "image_io.h"
#include "profiler.h"
#include "skin.h"
#include "transform.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <unordered_set>
#include <cstdlib>
#include <cctype>

using namespace ed;

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

namespace {
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
// Setup / frame
// ============================================================================
bool Editor::init(const AppOptions& options, std::string& error) {
    if (!renderer_.init(error)) return false;
    initGpuInfo();
    dpi_ = platform::dpiScale();
    configPath_ = options.configPath;
    loadConfig();
    benchReport_ = options.benchmarkReport;
    benchScenario_ = benchReport_.empty() ? -1 : 0;
    applyUiScale();
    registerCommands();
    buildGrid();

    if (!options.openPath.empty()) {
        openPath(options.openPath);
    } else if (options.demo) {
        buildDemo(options.demo);
        // Sample scenes open in the workspace they show (keeping their own shading).
        if (options.demo == 3) ws_ = Workspace::Texture, propsTab_ = 1;
        if (options.demo == 4) ws_ = Workspace::Rig;
        if (options.demo == 6) ws_ = Workspace::Light, propsTab_ = 2;
        if (options.demo == 5) sectionOpen_[uiHash("section", uiHash("m.boolean"))] = true;
    } else {
        buildDemo(0);
    }
    undo_.clear();
    redo_.clear();
    dirty_ = false;
    showHelp_ = options.showHelp;
    if (!options.renderOut.empty()) {
        renderOut_ = options.renderOut;
        if (options.renderDevice == "cpu") renderSet_.device = DEV_CPU;
        else if (options.renderDevice == "gpu") renderSet_.device = gpuTracerOk_ ? DEV_GPU : DEV_CPU;
        else if (options.renderDevice == "rtx") renderSet_.device = DEV_RTX;
        if (options.allowWarp) renderSet_.allowWarp = true;
        if (options.renderSamples > 0) renderSet_.samples = (float)options.renderSamples;
        if (options.renderPercent > 0) renderSet_.resolution = (float)options.renderPercent;
        renderView_ = true;  // started on the first frame, once the viewport size is known
        renderStamp_ = 0;
    }
    if (!options.stressReport.empty()) {
        stressReport_ = options.stressReport;
        stressOnly_ = options.stressOnly;
        stressInit();
    }
    if (status_.empty())
        setStatus("Welcome to Modeler3D - " + shortcutText((int)input::Action::Palette) +
                  " searches every command, F1 (or ?) shows the controls.");
    updateTitle();
    return true;
}

void Editor::shutdown() {
    shutdownRender();
    renderer_.shutdown();
}

void Editor::frame(const Input& realIn, int width, int height, float dt) {
    if (stressing_ && stressW_ > 0) {  // stress test: pretend the window has this size
        width = stressW_;
        height = stressH_;
    }
    screenW_ = width;
    screenH_ = height;
    if (dt > 0) lastDt_ = std::min(dt, 0.1f);
    clock_ += dt;
    updateCameraAnimation(dt);
    if (benchScenario_ >= 0) benchmarkTick();
    if (stressing_) stressTick();
    // The stress test's fuzzing phase replaces the real input with random input.
    Input fuzzIn;
    if (fuzzInput_) fuzzIn = stressInput(realIn, width, height);
    const Input& in = fuzzInput_ ? fuzzIn : realIn;
    if (dt > 0) fps_ = fps_ * 0.95f + (1.0f / dt) * 0.05f;
    statusTime_ += dt;
    swallowKeys_ = false;
    if (!in.droppedFiles.empty()) handleDroppedFiles(in.droppedFiles);
    if (configDirty_ && !ui_.isActive()) {
        configDirty_ = false;
        saveConfig();
    }

    // Layout: top bar (menus, pipeline workspaces), then tool panel |
    // viewport | properties panel, status bar at the bottom.
    ui_.setTime(clock_);
    ui_.tooltipsEnabled = access_.tooltips;
    const float fs = (float)fontScale_;
    const float lw = std::min(136 * fs, width * 0.3f), rw = std::min(140 * fs, width * 0.3f), sb = 12 * fs,
                tb = 18 * fs;
    topBar_ = {0, 0, (float)width, tb};
    leftPanel_ = {0, tb, lw, std::max(1.0f, height - sb - tb)};
    rightPanel_ = {width - rw, tb, rw, std::max(1.0f, height - sb - tb)};
    statusBar_ = {0, height - sb, (float)width, sb};
    viewport_ = {lw, tb, std::max(1.0f, width - lw - rw), std::max(1.0f, height - sb - tb)};
    layoutDone_ = true;
    headerRect_ = {viewport_.x, viewport_.y, viewport_.w, ui_.rowHeight() + 6 * fs};
    if (uvEditor_) {
        float size = std::floor(std::min(viewport_.w, viewport_.h - headerRect_.h) * 0.46f);
        float m = 6 * fs;
        uvRect_ = {viewport_.x + viewport_.w - size - m, viewport_.y + viewport_.h - size - m, size, size};
    } else {
        uvRect_ = {};
    }
    mouse_ = {in.mouseX - viewport_.x, in.mouseY - viewport_.y};
    updateMatrices();
    layoutNavPads();
    {
        PROF_SCOPE("particle sim");
        updateParticles(dt);
    }

    const bool modal = confirmQuit_ || showHelp_ || paletteOpen_;
    // Menus are drawn after the panels (on top); a press on an open menu must
    // not reach the panel underneath.
    const bool onMenu = openMenu_ >= 0 && menuRect_.contains(in.mouseX, in.mouseY);
    ui_.begin(in, width, height, fontScale_);
    {
        PROF_SCOPE("ui build (panels)");
        ui_.setInputEnabled(xf_ == Xform::None && !modal && !onMenu);
        buildTopBar(in);
        buildLeftPanel(in);
        buildRightPanel(in);
        buildStatusBar();
        buildViewportHeader();
        buildViewportToolbar();
        ui_.setInputEnabled(true);
    }
    {
        PROF_SCOPE("input+tools");
        if (confirmQuit_) {
            if (in.pressed(KEY_ESCAPE)) confirmQuit_ = false;
        } else if (showHelp_) {
            if (anyKeyPressed(in)) showHelp_ = false;
        } else if (!paletteOpen_) {
            if (!ui_.keyboardUsedThisFrame() && !ui_.wantsKeyboard()) handleShortcuts(in);
            if (openMenu_ < 0 && !paletteOpen_) handleViewport(in);
        } else if (keys_.pressed(input::Action::Screenshot, in)) {
            screenshotPending_ = true;  // screenshots work over the palette too
        }
    }
    updateMatrices();
    {
        PROF_SCOPE("ui build (overlays)");
        buildViewportOverlay();
        if (uvEditor_) buildUvEditor();
        if (showStats_) buildStatsOverlay();
        if (!modal) buildMenu(in);
        if (paletteOpen_ && !confirmQuit_) buildPalette(in);
        if (showHelp_) buildHelp();
        if (confirmQuit_) buildQuitDialog();
    }
    ui_.end();

    renderer_.clearWindow(width, height, theme::panel);
    {
        PROF_SCOPE("viewport render");
        viewportTimer_.begin();
        renderViewport();
        viewportTimer_.end();
    }
    {
        PROF_SCOPE("path trace");
        updateRender();  // traces / uploads; the result is shown next frame by renderViewport
    }
    renderer_.drawUI(ui_, width, height);
    renderer_.purge(scene_);
    if (screenshotPending_) saveScreenshot();

    if (editGroup_ && !ui_.isActive() && !ui_.wantsKeyboard()) editGroup_ = 0;
    updateTitle();
}

void Editor::requestQuit() {
    if (stressing_) return;  // the stress test ends itself
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

Object* Editor::activeMesh() {
    Object* o = active();
    return (o && o->isMesh()) ? o : nullptr;
}

std::vector<char>& Editor::vertSel() {
    Object* o = activeMesh();
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

int Editor::weightSlotFor(const Object& o) const {
    return (weightSlot_ >= 0 && weightSlot_ < (int)o.skinBones.size()) ? weightSlot_ : -1;
}

// With a bone active, every skinned mesh shows that bone's influence;
// otherwise the bone picked in Properties > Skin.
int Editor::displayWeightSlot(const Object& o) const {
    int a = scene_.active;
    if (a >= 0 && a < (int)scene_.objects.size() && scene_.objects[a].kind == ObjectKind::Bone) {
        for (int s = 0; s < (int)o.skinBones.size(); ++s)
            if (o.skinBones[s] == scene_.objects[a].id) return s;
        return -1;
    }
    return weightSlotFor(o);
}

// ============================================================================
// Undo / redo: whole-scene snapshots (simple and robust for a small editor;
// a command pattern would store deltas instead).
// ============================================================================
Editor::Snapshot Editor::snapshot() {
    Snapshot s;
    s.objects.reserve(scene_.objects.size());
    s.meshes.reserve(scene_.objects.size());
    for (Object& o : scene_.objects) {
        Mesh m = std::move(o.mesh);  // copy everything but the mesh
        s.objects.push_back(o);
        o.mesh = std::move(m);
        std::shared_ptr<const Mesh> shared;
        if (!o.mesh.verts.empty() || !o.mesh.faces.empty()) {
            auto it = meshShare_.find(o.id);
            if (it != meshShare_.end() && it->second.first == o.mesh.version) {
                shared = it->second.second;
            } else {
                shared = std::make_shared<const Mesh>(o.mesh);
                meshShare_[o.id] = {o.mesh.version, shared};
            }
        }
        s.meshes.push_back(std::move(shared));
    }
    // Forget copies of objects that no longer exist.
    if (meshShare_.size() > scene_.objects.size() * 2 + 16) {
        for (auto it = meshShare_.begin(); it != meshShare_.end();)
            it = scene_.indexOf(it->first) < 0 ? meshShare_.erase(it) : std::next(it);
    }
    s.active = scene_.active;
    s.vsel = vsel_;
    s.ambient = scene_.ambient;
    return s;
}

size_t Editor::undoBytes() const {
    std::unordered_set<const Mesh*> seen;
    size_t bytes = 0;
    auto meshBytes = [](const Mesh& m) {
        size_t b = m.verts.capacity() * sizeof(Vec3) + m.weights.capacity() * sizeof(BoneWeights);
        for (const auto& f : m.faces) b += f.capacity() * sizeof(int) + sizeof(f);
        for (const auto& u : m.uvs) b += u.capacity() * sizeof(Vec2) + sizeof(u);
        return b;
    };
    for (const auto* list : {&undo_, &redo_})
        for (const Snapshot& s : *list) {
            bytes += s.objects.size() * sizeof(Object) + s.vsel.size();
            for (const auto& m : s.meshes)
                if (m && seen.insert(m.get()).second) bytes += meshBytes(*m);
        }
    return bytes;
}

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
    for (size_t i = 0; i < s.objects.size() && i < s.meshes.size(); ++i)
        if (s.meshes[i]) s.objects[i].mesh = *s.meshes[i];
    scene_.objects = std::move(s.objects);
    scene_.invalidateNames();
    scene_.active = s.active < (int)scene_.objects.size() ? s.active : -1;
    scene_.ambient = s.ambient;
    vsel_ = std::move(s.vsel);
    if (mode_ == Mode::Edit && !activeMesh()) mode_ = Mode::Object;
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
// Transform tool. Two front-ends feed it:
//   - Unity-style handles (editor_gizmo.cpp): Move / Rotate / Scale / All
//   - modal keyboard transforms (Blender keymap: G / R / S + X/Y/Z)
// Both produce an XfDelta that applyTransform() applies to the originals
// captured by beginTransform(). Objects are written back through the
// Unity-like Transform API (transform.h), so world-space edits land in each
// object's parent space; edit-mode vertices go back into object space.
// ============================================================================
bool Editor::beginTransform(Xform type, bool pushUndoState, bool fromGizmo) {
    if (xf_ != Xform::None) return false;
    xfItems_.clear();
    xfPos_.clear();
    xfRot_.clear();
    xfScale_.clear();
    xfLocal_.clear();
    xfWorld_.clear();
    xfParentInv_.clear();
    Vec3 sum;
    if (mode_ == Mode::Object) {
        for (int i : transformRoots()) {
            const Object& o = scene_.objects[i];
            Mat4 w = scene_.world(i);
            xfItems_.push_back(i);
            xfPos_.push_back(o.position);
            xfRot_.push_back(o.rotation);
            xfScale_.push_back(o.scale);
            xfWorld_.push_back(w);
            sum += Vec3(w(0, 3), w(1, 3), w(2, 3));
        }
    } else if (Object* o = activeMesh()) {
        auto& sel = vertSel();
        Mat4 m = scene_.world(scene_.active);
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
    xfPivot_ = fromGizmo ? gizmoPivot_ : sum / float(xfItems_.size());
    // Unity's "Pivot" mode rotates / scales every object around its own origin.
    xfPerObjectPivot_ = fromGizmo && mode_ == Mode::Object && !pivotCenter_ && xfItems_.size() > 1;
    if (pushUndoState) pushUndo();
    xfPushedUndo_ = pushUndoState;
    xf_ = type;
    xfFromGizmo_ = fromGizmo;
    xfAxis_ = -1;
    xfStart_ = mouse_;
    Vec2 ps;
    worldToScreen(xfPivot_, ps);
    xfPrevAngle_ = std::atan2(-(mouse_.y - ps.y), mouse_.x - ps.x);
    xfAngle_ = 0;
    modalNumber_.clear();
    lmbInViewport_ = boxSelecting_ = false;
    nav_ = Nav::None;
    if (!fromGizmo) updateTransform(Input());
    return true;
}

void Editor::applyTransform(const XfDelta& d) {
    Object* editObj = mode_ == Mode::Edit ? activeMesh() : nullptr;
    const bool rotating = std::fabs(d.rotDeg) > 1e-6f;
    const Mat4 R = rotating ? rotationAxis(d.rotAxis, toRadians(d.rotDeg)) : Mat4();
    const Vec3 axes[3] = {{d.basis(0, 0), d.basis(1, 0), d.basis(2, 0)},
                          {d.basis(0, 1), d.basis(1, 1), d.basis(2, 1)},
                          {d.basis(0, 2), d.basis(1, 2), d.basis(2, 2)}};
    auto scaled = [&](Vec3 rel) {
        Vec3 out;
        for (int i = 0; i < 3; ++i) out += axes[i] * (dot(rel, axes[i]) * d.scale[i]);
        return out;
    };

    if (editObj) {
        const Mat4 inv = inverse(scene_.world(scene_.active));
        for (size_t k = 0; k < xfItems_.size(); ++k) {
            Vec3 rel = transformDir(R, scaled(xfPos_[k] - xfPivot_));
            editObj->mesh.verts[xfItems_[k]] = transformPoint(inv, xfPivot_ + rel + d.move);
        }
        editObj->mesh.touchPositions();
        return;
    }
    for (size_t k = 0; k < xfItems_.size(); ++k) {
        const int i = xfItems_[k];
        Object& o = scene_.objects[i];
        o.position = xfPos_[k];
        o.rotation = xfRot_[k];
        o.scale = mul(xfScale_[k], d.scale);  // scale handles act on the object's local axes, like Unity
        const Vec3 p0(xfWorld_[k](0, 3), xfWorld_[k](1, 3), xfWorld_[k](2, 3));
        const Vec3 pivot = xfPerObjectPivot_ ? p0 : xfPivot_;
        if (rotating) tf::setRotation(scene_, i, R * tf::orthonormalized(xfWorld_[k]));
        tf::setPosition(scene_, i, pivot + transformDir(R, scaled(p0 - pivot)) + d.move);
    }
}

// Mouse motion projected onto an axis' on-screen direction, in world units.
float Editor::axisDrag(Vec3 pivot, Vec3 axis, Vec2 delta) const {
    float step = std::max(1e-4f, worldPerPixel(pivot) * 50.0f);
    Vec2 s0, s1;
    if (!worldToScreen(pivot, s0) || !worldToScreen(pivot + axis * step, s1)) return 0.0f;
    Vec2 sd = (s1 - s0) * (1.0f / step);
    float len2 = dot(sd, sd);
    return len2 > 1e-8f ? dot(delta, sd) / len2 : 0.0f;
}

void Editor::updateTransform(const Input& in) {
    const bool snap = in.ctrl();
    const Vec2 d = mouse_ - xfStart_;
    const Vec3 axis = xfAxis_ < 0 ? Vec3() : (xfAxis_ < 3 ? axisVector(xfAxis_) : xfCustomAxis_);
    const char* axisLabel = xfAxis_ < 0   ? ""
                            : xfAxis_ == 0 ? "along X"
                            : xfAxis_ == 1 ? "along Y"
                            : xfAxis_ == 2 ? "along Z"
                                           : "along normal";
    XfDelta delta;
    if (xf_ == Xform::Grab) {
        if (xfAxis_ < 0) {
            // Free move in the view plane, scaled so the item tracks the cursor.
            float k = worldPerPixel(xfPivot_);
            delta.move = camRight() * (d.x * k) - camUp() * (d.y * k);
            if (snap)
                for (int i = 0; i < 3; ++i) delta.move[i] = std::round(delta.move[i] * 4.0f) / 4.0f;
        } else {
            float t = axisDrag(xfPivot_, axis, d);
            if (snap) t = std::round(t * 4.0f) / 4.0f;
            delta.move = axis * t;
        }
        Vec3 m = delta.move;
        xfInfo_ = strf("Move %s  (%.3f, %.3f, %.3f)", axisLabel, m.x, m.y, m.z);
    } else if (xf_ == Xform::Rotate) {
        Vec2 ps;
        worldToScreen(xfPivot_, ps);
        float a = std::atan2(-(mouse_.y - ps.y), mouse_.x - ps.x);
        xfAngle_ += wrapRadians(a - xfPrevAngle_);
        xfPrevAngle_ = a;
        float deg = toDegrees(xfAngle_);
        if (snap) deg = std::round(deg / 15.0f) * 15.0f;
        Vec3 towardViewer = -camForward();
        delta.rotAxis = xfAxis_ < 0 ? towardViewer : axis;
        // Keep the rotation following the cursor even when the axis points away.
        delta.rotDeg = (xfAxis_ >= 0 && dot(axis, towardViewer) < 0) ? -deg : deg;
        xfInfo_ = strf("Rotate %s  %.1f deg", axisLabel, deg);
    } else if (xf_ == Xform::Scale) {
        Vec2 ps;
        worldToScreen(xfPivot_, ps);
        // Reference distance from the pivot; floored so starting right on the
        // pivot doesn't make the scale explode.
        float d0 = std::max(length(xfStart_ - ps), 40.0f * dpi_);
        float f = length(mouse_ - ps) / d0;
        if (snap) f = std::round(f * 10.0f) / 10.0f;
        if (xfAxis_ < 0) {
            delta.scale = {f, f, f};
        } else if (xfAxis_ < 3) {
            delta.scale[xfAxis_] = f;
        } else {  // custom axis (extrude normal): basis with the axis first
            Vec3 u = normalize(axis);
            Vec3 v = normalize(cross(u, std::fabs(u.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0)));
            Vec3 w = cross(u, v);
            for (int r = 0; r < 3; ++r) {
                delta.basis(r, 0) = u[r];
                delta.basis(r, 1) = v[r];
                delta.basis(r, 2) = w[r];
            }
            delta.scale = {f, 1, 1};
        }
        xfInfo_ = strf("Scale %s  x%.3f", axisLabel, f);
    }
    // A typed number overrides the mouse (Blender: G X 2 Enter).
    if (!modalNumber_.empty() && modalNumber_ != "-" && modalNumber_ != ".") {
        const float v = std::strtof(modalNumber_.c_str(), nullptr);
        if (std::isfinite(v)) {
            delta = XfDelta();
            if (xf_ == Xform::Grab) {
                Vec3 a = xfAxis_ < 0 ? Vec3(1, 0, 0) : axis;
                delta.move = a * v;
                xfInfo_ = strf("Move %s  %.4g", xfAxis_ < 0 ? "along X" : axisLabel, v);
            } else if (xf_ == Xform::Rotate) {
                Vec3 towardViewer = -camForward();
                delta.rotAxis = xfAxis_ < 0 ? towardViewer : axis;
                delta.rotDeg = v;
                xfInfo_ = strf("Rotate %s  %.4g deg", axisLabel, v);
            } else if (xf_ == Xform::Scale) {
                if (xfAxis_ < 0) {
                    delta.scale = {v, v, v};
                } else if (xfAxis_ < 3) {
                    delta.scale[xfAxis_] = v;
                } else {
                    Vec3 u = normalize(axis);
                    Vec3 vv = normalize(cross(u, std::fabs(u.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0)));
                    Vec3 w = cross(u, vv);
                    for (int r = 0; r < 3; ++r) {
                        delta.basis(r, 0) = u[r];
                        delta.basis(r, 1) = vv[r];
                        delta.basis(r, 2) = w[r];
                    }
                    delta.scale = {v, 1, 1};
                }
                xfInfo_ = strf("Scale %s  x%.4g", axisLabel, v);
            }
        }
    }
    applyTransform(delta);
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
        } else if (Object* o = activeMesh()) {
            for (size_t k = 0; k < xfItems_.size(); ++k) o->mesh.verts[xfItems_[k]] = xfLocal_[k];
            o->mesh.touchPositions();
        }
        if (xfPushedUndo_ && !undo_.empty()) undo_.pop_back();
        setStatus("Transform cancelled");
    } else {
        markDirty();
        setStatus(xfInfo_);
    }
    xf_ = Xform::None;
    xfFromGizmo_ = false;
    gizmoDrag_ = GH_None;
    modalNumber_.clear();
}

// ============================================================================
// Input
// ============================================================================
void Editor::setTool(Tool t) {
    tool_ = t;
    static const char* names[] = {"Hand (view) tool", "Move tool", "Rotate tool", "Scale tool", "Transform tool"};
    setStatus(names[(int)t]);
}

void Editor::handleShortcuts(const Input& in) {
    if (xf_ != Xform::None || insetModal_ || captureAction_ >= 0 || swallowKeys_) return;
    if (keys_.pressed(input::Action::Render, in)) {  // also works while the render view is up
        toggleRenderView();
        return;
    }
    using input::Action;
    if (keys_.pressed(Action::Palette, in)) return openPalette();
    if (flyToggle_) {  // fly mode owns W A S D Q E; only its own key and Esc end it
        if (keys_.pressed(Action::FlyMode, in) || in.pressed(KEY_ESCAPE)) runCommand("view.fly");
        return;
    }
    if (in.pressed(KEY_ESCAPE) && settingsOpen_) {
        settingsOpen_ = false;
        return;
    }
    for (int w = 0; w < kWorkspaceCount; ++w)
        if (keys_.pressed((Action)((int)Action::WorkspaceModel + w), in)) return setWorkspace((Workspace)w);
    // Unity: the fly keys (W A S D Q E) steer the camera while RMB is held,
    // so the tool shortcuts that share those keys pause during flight.
    if (nav_ == Nav::Fly) return;
    auto hit = [&](Action a) { return keys_.pressed(a, in); };
    if (mode_ == Mode::Edit) {  // Edit-mode keys take precedence (1 2 3 = select modes)
        if (hit(Action::SelectVertices)) return setSelMode(SelMode::Vertex);
        if (hit(Action::SelectEdges)) return setSelMode(SelMode::Edge);
        if (hit(Action::SelectFaces)) return setSelMode(SelMode::Face);
        if (hit(Action::Inset)) return insetSelected(true);
        if (hit(Action::Bevel)) return bevelSelected(true);
        if (hit(Action::LoopCut)) return loopCutAt(true);
        if (hit(Action::Connect)) return connectSelected();
        if (hit(Action::Merge)) return mergeSelected(false);
        if (hit(Action::Fill)) return fillSelected();
        if (hit(Action::Bridge)) return bridgeSelected();
        if (hit(Action::PushThrough)) return pushThroughSelected();
        if (hit(Action::SelectMore)) return growSelection(true);
        if (hit(Action::SelectLess)) return growSelection(false);
    }
    if (hit(Action::FlyMode)) return (void)runCommand("view.fly");
    if (hit(Action::NextObject)) return cycleObject(1);
    if (hit(Action::PrevObject)) return cycleObject(-1);
    if (hit(Action::JoinObjects)) return joinSelected();
    if (hit(Action::Help)) showHelp_ = true;
    if (hit(Action::Stats)) showStats_ = !showStats_;
    if (hit(Action::Screenshot)) screenshotPending_ = true;
    if (hit(Action::Undo)) undo();
    if (hit(Action::Redo)) redo();
    if (hit(Action::Save)) saveFile();
    if (hit(Action::Open)) loadFile();
    if (hit(Action::Parent)) parentSelected();
    if (hit(Action::Unparent)) unparentSelected();
    if (hit(Action::ToggleEditMode)) setMode(mode_ == Mode::Object ? Mode::Edit : Mode::Object);
    if (hit(Action::FrameSelected)) frameSelected();
    if (hit(Action::SmartUnwrap)) unwrapActive(uv::Method::Smart);
    if (hit(Action::PlayPause)) togglePlay();
    if (hit(Action::Delete)) deleteSelected();
    if (hit(Action::Duplicate)) duplicateSelected();
    if (hit(Action::SelectAll)) selectAll();
    if (hit(Action::Extrude)) extrude();
    if (hit(Action::ViewFront)) setView(0, 0);
    if (hit(Action::ViewBack)) setView(180, 0);
    if (hit(Action::ViewRight)) setView(90, 0);
    if (hit(Action::ViewLeft)) setView(-90, 0);
    if (hit(Action::ViewTop)) setView(0, 89.9f);
    if (hit(Action::ViewBottom)) setView(0, -89.9f);
    if (hit(Action::ToggleOrtho)) {
        cam_.ortho = !cam_.ortho;
        setStatus(cam_.ortho ? "Orthographic view" : "Perspective view");
    }
    if (hit(Action::ToolHand)) setTool(Tool::Hand);
    if (hit(Action::ToolMove)) setTool(Tool::Move);
    if (hit(Action::ToolRotate)) setTool(Tool::Rotate);
    if (hit(Action::ToolScale)) setTool(Tool::Scale);
    if (hit(Action::ToolUniversal)) setTool(Tool::Universal);
    if (hit(Action::ToggleLocalGlobal)) {
        localSpace_ = !localSpace_;
        setStatus(localSpace_ ? "Handle orientation: Local" : "Handle orientation: Global");
    }
    if (hit(Action::TogglePivotCenter)) {
        pivotCenter_ = !pivotCenter_;
        setStatus(pivotCenter_ ? "Handle position: Center" : "Handle position: Pivot");
    }
    if (hit(Action::CycleShading)) {
        shading_ = (shading_ + 1) % SHADE_COUNT;
        static const char* names[] = {"Studio", "Lit", "UV checker", "Weights"};
        setStatus(std::string("Shading: ") + names[shading_]);
    }
    if (hit(Action::ToggleWireframe)) wireframe_ = !wireframe_;
    if (hit(Action::LookThroughCamera)) lookThroughCamera();
    if (hit(Action::AlignCameraToView)) alignActiveCameraToView();
    if (xf_ == Xform::None && hit(Action::ModalGrab)) beginTransform(Xform::Grab, true);
    if (xf_ == Xform::None && hit(Action::ModalRotate)) beginTransform(Xform::Rotate, true);
    if (xf_ == Xform::None && hit(Action::ModalScale)) beginTransform(Xform::Scale, true);
}

bool Editor::handleUvEditor(const Input& in) {
    if (!uvEditor_) return false;
    const bool inside = uvRect_.contains(in.mouseX, in.mouseY);
    Object* o = activeMesh();
    if (uvDragging_) {
        if (!in.mouseDown[MOUSE_LEFT] || !o) {
            uvDragging_ = false;
            return true;
        }
        Vec2 m{in.mouseX, in.mouseY};
        Vec2 delta = (m - uvDragLast_) / std::max(1.0f, uvRect_.w);
        delta.y = -delta.y;  // UV v grows upward
        uv::translate(o->mesh, selectedFaces(o->mesh, vertSel()), delta);
        uvDragLast_ = m;
        return true;
    }
    if (!inside) return false;
    if (in.mousePressed[MOUSE_LEFT]) {
        std::vector<int> faces = o ? selectedFaces(o->mesh, vertSel()) : std::vector<int>();
        if (mode_ == Mode::Edit && o && o->mesh.hasUVs() && !faces.empty()) {
            pushUndo();
            markDirty();
            uvDragging_ = true;
            uvDragLast_ = {in.mouseX, in.mouseY};
        } else {
            setStatus("UV editor: select faces in Edit mode, then drag here to move their UVs");
        }
    }
    return true;  // swallow everything over the UV editor
}

// Pads under the scene gizmo: Pan and Zoom (drag them). The gizmo itself
// orbits when dragged. Window coordinates.
void Editor::layoutNavPads() {
    Vec2 c;
    float len;
    sceneGizmoLayout(c, len);
    const float fs = (float)fontScale_;
    const float r = len + 6 * fs, h = ui_.rowHeight() * 0.95f;
    navPads_[0] = {c.x - r, c.y - r, 2 * r, 2 * r};
    const float y = c.y + len + 15 * fs;
    navPads_[1] = {c.x - r, y, 2 * r, h};
    navPads_[2] = {c.x - r, y + h + fs, 2 * r, h};
    if (!access_.navButtons) navPads_[1] = navPads_[2] = Rect{c.x, c.y + len + 8 * fs, 0, 0};
}

// Mouse-look around the eye (fly camera).
void Editor::flyLook(float dx, float dy) {
    Vec3 eye = cam_.eye();
    cam_.yaw -= dx * camSet_.lookSensitivity;
    cam_.pitch = clampf(cam_.pitch + dy * camSet_.lookSensitivity, -89.9f, 89.9f);
    float p = toRadians(cam_.pitch), y = toRadians(cam_.yaw);
    Vec3 dir(std::cos(p) * std::sin(y), std::sin(p), std::cos(p) * std::cos(y));
    cam_.target = eye - dir * cam_.distance;
    updateMatrices();
}

// W A S D Q E flight with Unity-style acceleration (Shift = fast).
void Editor::flyMove(const Input& in) {
    using input::Action;
    Vec3 move;
    if (keys_.down(Action::FlyForward, in)) move += camForward();
    if (keys_.down(Action::FlyBack, in)) move -= camForward();
    if (keys_.down(Action::FlyRight, in)) move += camRight();
    if (keys_.down(Action::FlyLeft, in)) move -= camRight();
    if (keys_.down(Action::FlyUp, in)) move += Vec3(0, 1, 0);
    if (keys_.down(Action::FlyDown, in)) move -= Vec3(0, 1, 0);
    if (length(move) > 0) {
        flyHold_ += lastDt_;
        float accel = camSet_.flyAcceleration ? 1.0f + std::min(flyHold_, 2.0f) * 1.5f : 1.0f;
        float speed = std::max(1.0f, cam_.distance) * 1.2f * flySpeed_ * accel * (in.shift() ? camSet_.fastMultiplier : 1.0f);
        cam_.target += normalize(move) * (speed * lastDt_);
    } else {
        flyHold_ = 0;
    }
}

void Editor::handleViewport(const Input& in) {
    const bool unity = keymap_ == Keymap::Unity;
    const bool overUi = headerRect_.contains(in.mouseX, in.mouseY) || toolbarRect_.contains(in.mouseX, in.mouseY) ||
                        (uvEditor_ && uvRect_.contains(in.mouseX, in.mouseY));
    const bool overGizmo = sceneGizmoRect_.contains(in.mouseX, in.mouseY);
    const bool overPads = access_.navButtons && (navPads_[1].contains(in.mouseX, in.mouseY) ||
                                                 navPads_[2].contains(in.mouseX, in.mouseY));
    const bool over = viewport_.contains(in.mouseX, in.mouseY) && !ui_.isActive() && !overUi && !overGizmo && !overPads;

    // --- wheel / trackpad gestures ---
    if (in.fractionalWheel && !access_.trackpad && !access_.trackpadHintShown) {
        access_.trackpadHintShown = true;
        configDirty_ = true;
        setStatus("Using a trackpad? Settings > Input > Trackpad navigation: two fingers orbit, pinch zooms");
    }
    const bool wheelMoved = in.wheel != 0.0f || in.wheelX != 0.0f;
    if ((nav_ == Nav::Fly || flyToggle_) && in.wheel != 0.0f) {
        // Unity: scrolling while flying changes the fly speed instead of zooming.
        flySpeed_ = clampf(flySpeed_ * std::pow(1.25f, in.wheel), 0.01f, 100.0f);
        setStatus(strf("Fly speed x%.2f", flySpeed_));
    } else if ((over || overGizmo || overPads) && wheelMoved && xf_ == Xform::None && !insetModal_) {
        camAnimating_ = false;
        const bool zoom = in.pinch || in.ctrl() || !access_.trackpad;
        // One notch ~ a 40 px drag.
        const float dx = in.wheelX * 40.0f * (camSet_.invertX ? -1.0f : 1.0f);
        const float dy = -in.wheel * 40.0f * (camSet_.invertY ? -1.0f : 1.0f);
        if (access_.trackpad && !zoom) {
            if (in.shift()) {  // two fingers + Shift: pan
                float k = worldPerPixel(cam_.target) * camSet_.panSpeed;
                cam_.target -= camRight() * (dx * k);
                cam_.target += camUp() * (dy * k);
            } else {  // two fingers: orbit
                cam_.yaw -= dx * camSet_.orbitSensitivity;
                cam_.pitch = clampf(cam_.pitch + dy * camSet_.orbitSensitivity, -89.9f, 89.9f);
            }
        } else {
            const float amount = in.wheel * camSet_.zoomSpeed * (in.pinch ? 2.0f : 1.0f);
            cam_.distance = clampf(cam_.distance * std::pow(0.85f, amount), 0.05f, 5000.0f);
            if (in.wheelX != 0.0f) {  // mouse with a tilt wheel / sideways swipe: pan sideways
                float k = worldPerPixel(cam_.target) * camSet_.panSpeed;
                cam_.target -= camRight() * (dx * k);
            }
        }
        updateMatrices();
    }

    // --- interactive inset / bevel owns the mouse ---
    if (updateInsetModal(in)) return;

    // --- an active transform owns the mouse ---
    if (xf_ != Xform::None) {
        if (xfFromGizmo_) {
            if (in.mousePressed[MOUSE_RIGHT] || in.pressed(KEY_ESCAPE)) {
                endTransform(false);
            } else if (!in.mouseDown[MOUSE_LEFT]) {
                endTransform(true);
            } else {
                updateMatrices();
                updateGizmoDrag(in);
            }
            return;
        }
        const input::Action axisKeys[3] = {input::Action::AxisX, input::Action::AxisY, input::Action::AxisZ};
        for (int a = 0; a < 3; ++a)
            if (keys_.pressed(axisKeys[a], in)) xfAxis_ = xfAxis_ == a ? -1 : a;
        // Typed value (digits, '.', '-'; Backspace edits).
        for (char ch : in.text) {
            if (std::isdigit((unsigned char)ch) || ch == '.') {
                if (modalNumber_.size() < 16) modalNumber_ += ch;
            } else if (ch == '-') {
                modalNumber_ = (!modalNumber_.empty() && modalNumber_[0] == '-') ? modalNumber_.substr(1) : "-" + modalNumber_;
            }
        }
        if (in.keyRepeat[KEY_BACKSPACE] && !modalNumber_.empty()) modalNumber_.pop_back();
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

    // --- fly mode (keyboard toggle, no button held): drag or Alt+arrows look ---
    if (flyToggle_) {
        using input::Action;
        if (in.mousePressed[MOUSE_RIGHT]) {
            runCommand("view.fly");
            return;
        }
        if (in.mouseDown[MOUSE_LEFT] && viewport_.contains(in.mouseX, in.mouseY))
            flyLook(in.mouseDX * (camSet_.invertX ? -1.0f : 1.0f), in.mouseDY * (camSet_.invertY ? -1.0f : 1.0f));
        const float rate = 120.0f * lastDt_ / std::max(0.01f, camSet_.lookSensitivity);
        float lx = 0, ly = 0;
        if (keys_.down(Action::OrbitLeft, in)) lx -= rate;
        if (keys_.down(Action::OrbitRight, in)) lx += rate;
        if (keys_.down(Action::OrbitUp, in)) ly -= rate;
        if (keys_.down(Action::OrbitDown, in)) ly += rate;
        if (lx != 0 || ly != 0) flyLook(lx, ly);
        flyMove(in);
        updateMatrices();
        return;
    }

    if (nav_ == Nav::None && !lmbInViewport_ && navPad_ < 0 && handleUvEditor(in)) return;

    // --- scene gizmo + navigation pads: click an axis to look along it, drag
    // the gizmo to orbit, drag Pan / Zoom (no middle or right button needed) ---
    if (navPad_ >= 0) {
        if (!in.mouseDown[MOUSE_LEFT]) {
            if (navPad_ == 0 && !navPadMoved_ && navPadPart_ >= 0) clickSceneGizmo(navPadPart_);
            navPad_ = -1;
        } else {
            if (length(Vec2(in.mouseX, in.mouseY) - navPadStart_) > 3.0f * dpi_) navPadMoved_ = true;
            if (navPadMoved_) {
                camAnimating_ = false;
                const float dx = in.mouseDX * (camSet_.invertX ? -1.0f : 1.0f);
                const float dy = in.mouseDY * (camSet_.invertY ? -1.0f : 1.0f);
                if (navPad_ == 0) {
                    cam_.yaw -= dx * camSet_.orbitSensitivity;
                    cam_.pitch = clampf(cam_.pitch + dy * camSet_.orbitSensitivity, -89.9f, 89.9f);
                } else if (navPad_ == 1) {
                    float k = worldPerPixel(cam_.target) * camSet_.panSpeed;
                    cam_.target -= camRight() * (in.mouseDX * k);
                    cam_.target += camUp() * (in.mouseDY * k);
                } else {
                    cam_.distance = clampf(cam_.distance * std::exp(in.mouseDY * 0.005f * camSet_.zoomSpeed), 0.05f, 5000.0f);
                }
                updateMatrices();
            }
        }
        return;
    }
    sceneGizmoHover_ = (nav_ == Nav::None && !ui_.isActive()) ? pickSceneGizmo({in.mouseX, in.mouseY}) : -1;
    if (in.mousePressed[MOUSE_LEFT] && !lmbInViewport_ && nav_ == Nav::None && !ui_.isActive()) {
        int pad = -1;
        if (overGizmo || sceneGizmoHover_ >= 0) pad = 0;
        else if (access_.navButtons && navPads_[1].contains(in.mouseX, in.mouseY)) pad = 1;
        else if (access_.navButtons && navPads_[2].contains(in.mouseX, in.mouseY)) pad = 2;
        if (pad >= 0) {
            navPad_ = pad;
            navPadPart_ = pad == 0 ? sceneGizmoHover_ : -1;
            navPadMoved_ = false;
            navPadStart_ = {in.mouseX, in.mouseY};
            return;
        }
    }

    // --- keyboard camera: arrows move, Alt+arrows orbit, = / - zoom (Shift = faster) ---
    if (nav_ == Nav::None && !ui_.wantsKeyboard() && !ui_.keyboardUsedThisFrame() && captureAction_ < 0) {
        using input::Action;
        Vec3 flat = camForward();
        flat.y = 0;
        flat = length(flat) > 1e-4f ? normalize(flat) : camUp();
        Vec3 move;
        if (keys_.down(Action::CameraForward, in)) move += flat;
        if (keys_.down(Action::CameraBack, in)) move -= flat;
        if (keys_.down(Action::CameraRight, in)) move += camRight();
        if (keys_.down(Action::CameraLeft, in)) move -= camRight();
        const float boost = in.shift() ? camSet_.fastMultiplier : 1.0f;
        bool moved = false;
        if (length(move) > 0) {
            float speed = std::max(1.0f, cam_.distance) * 0.8f * camSet_.arrowSpeed * boost;
            cam_.target += normalize(move) * (speed * lastDt_);
            moved = true;
        }
        const float orbit = 90.0f * lastDt_ * boost;  // degrees per second
        if (keys_.down(Action::OrbitLeft, in)) cam_.yaw += orbit, moved = true;
        if (keys_.down(Action::OrbitRight, in)) cam_.yaw -= orbit, moved = true;
        if (keys_.down(Action::OrbitUp, in)) cam_.pitch = clampf(cam_.pitch - orbit, -89.9f, 89.9f), moved = true;
        if (keys_.down(Action::OrbitDown, in)) cam_.pitch = clampf(cam_.pitch + orbit, -89.9f, 89.9f), moved = true;
        const float zoom = std::pow(0.25f, lastDt_ * boost);  // x4 closer per second
        if (keys_.down(Action::ZoomIn, in)) cam_.distance = clampf(cam_.distance * zoom, 0.05f, 5000.0f), moved = true;
        if (keys_.down(Action::ZoomOut, in)) cam_.distance = clampf(cam_.distance / zoom, 0.05f, 5000.0f), moved = true;
        if (moved) {
            camAnimating_ = false;
            updateMatrices();
        }
    }

    // --- camera navigation ---
    // Unity:   Alt+LMB orbit, Alt+Ctrl+LMB pan, Alt+Shift+LMB zoom (laptops without
    //          a middle button), MMB pan, Alt+RMB zoom, RMB fly (+WASDQE), Hand tool pans.
    // Blender: RMB / MMB orbit, Shift = pan, Ctrl = zoom, Alt+LMB = emulated middle button.
    if (nav_ == Nav::None && over) {
        int b = -1;
        Nav kind = Nav::Orbit;
        if (unity) {
            if (in.mousePressed[MOUSE_MIDDLE]) b = MOUSE_MIDDLE, kind = Nav::Pan;
            else if (in.mousePressed[MOUSE_RIGHT]) b = MOUSE_RIGHT, kind = in.alt() ? Nav::Zoom : Nav::Fly;
            else if (in.mousePressed[MOUSE_LEFT] && in.alt())
                b = MOUSE_LEFT, kind = in.ctrl() ? Nav::Pan : in.shift() ? Nav::Zoom : Nav::Orbit;
            else if (in.mousePressed[MOUSE_LEFT] && tool_ == Tool::Hand) b = MOUSE_LEFT, kind = Nav::Pan;
        } else {
            if (in.mousePressed[MOUSE_MIDDLE]) b = MOUSE_MIDDLE;
            else if (in.mousePressed[MOUSE_RIGHT]) b = MOUSE_RIGHT;
            else if (in.mousePressed[MOUSE_LEFT] && in.alt()) b = MOUSE_LEFT;
        }
        if (b >= 0) {
            nav_ = kind;
            navButton_ = b;
            camAnimating_ = false;  // user input takes over from any transition
            flyHold_ = 0;
        }
    }
    if (nav_ != Nav::None) {
        if (!in.mouseDown[navButton_]) {
            nav_ = Nav::None;
        } else {
            if (!unity) nav_ = in.shift() ? Nav::Pan : in.ctrl() ? Nav::Zoom : Nav::Orbit;
            // Inversion applies to rotation (orbit / mouse-look).
            const float dx = in.mouseDX * (camSet_.invertX ? -1.0f : 1.0f);
            const float dy = in.mouseDY * (camSet_.invertY ? -1.0f : 1.0f);
            switch (nav_) {
                case Nav::Orbit:
                    cam_.yaw -= dx * camSet_.orbitSensitivity;
                    cam_.pitch = clampf(cam_.pitch + dy * camSet_.orbitSensitivity, -89.9f, 89.9f);
                    break;
                case Nav::Pan: {
                    float k = worldPerPixel(cam_.target) * camSet_.panSpeed;
                    cam_.target -= camRight() * (in.mouseDX * k);
                    cam_.target += camUp() * (in.mouseDY * k);
                    break;
                }
                case Nav::Zoom:
                    cam_.distance = clampf(cam_.distance * std::exp((in.mouseDY - in.mouseDX) * 0.005f * camSet_.zoomSpeed),
                                           0.05f, 5000.0f);
                    break;
                case Nav::Fly:
                    flyLook(dx, dy);
                    flyMove(in);
                    break;
                default:
                    break;
            }
            updateMatrices();
        }
        return;
    }

    // --- transform handles ---
    computeGizmo();
    gizmoHover_ = (over && gizmoVisible_ && !lmbInViewport_) ? pickHandle(mouse_) : GH_None;
    if (over && in.mousePressed[MOUSE_LEFT] && !in.alt() && gizmoHover_ != GH_None) {
        beginGizmoDrag(gizmoHover_, in);
        return;
    }

    // --- selection: click, double-click (edge loop), or drag a box ---
    if (over && in.mousePressed[MOUSE_LEFT]) {
        lmbInViewport_ = true;
        boxSelecting_ = false;
        pressPos_ = mouse_;
    }
    if (lmbInViewport_) {
        if (in.mouseDown[MOUSE_LEFT]) {
            if (length(mouse_ - pressPos_) > 4.0f * dpi_) boxSelecting_ = true;
        } else {
            if (boxSelecting_) {
                boxSelect(pressPos_, mouse_, in.shift(), in.ctrl());
            } else {
                const bool dbl = clock_ - lastViewportClick_ < 0.4 && length(mouse_ - lastViewportClickPos_) < 6.0f * dpi_;
                lastViewportClick_ = dbl ? -1.0 : clock_;
                lastViewportClickPos_ = mouse_;
                if (dbl && mode_ == Mode::Edit && selMode_ == SelMode::Edge) selectLoopRing(in.ctrl(), true, in.shift());
                else clickSelect(mouse_, in.shift() || (unity && in.ctrl()));
            }
            lmbInViewport_ = boxSelecting_ = false;
        }
    }
}

int Editor::pickIcon(Vec2 p) {
    // Icons are lights, bones, empties... With few of them, walking their
    // parent chains is cheaper than computing every object's world matrix.
    int icons = 0;
    for (const Object& o : scene_.objects) icons += o.isMesh() ? 0 : 1;
    if (icons == 0) return -1;
    if (icons > 64) scene_.beginWorldCache();
    struct EndCache {
        const Scene& s;
        bool on;
        ~EndCache() {
            if (on) s.endWorldCache();
        }
    } endCache{scene_, icons > 64};
    int best = -1;
    float bestD = 12.0f * dpi_;
    for (int i = 0; i < (int)scene_.objects.size(); ++i) {
        const Object& o = scene_.objects[i];
        if (o.isMesh()) continue;
        float d;
        if (o.kind == ObjectKind::Bone) {
            Vec2 a, b;
            if (!worldToScreen(boneHead(scene_, i), a) || !worldToScreen(boneTail(scene_, i), b)) continue;
            d = distanceToSegment2D(p, a, b);
        } else {
            Vec2 s;
            if (!worldToScreen(transformPoint(scene_.world(i), Vec3()), s)) continue;
            d = length(s - p);
        }
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

void Editor::clickSelect(Vec2 p, bool extend) {
    if (mode_ == Mode::Edit) {
        clickSelectEdit(p, extend);
        return;
    }
    int hit = pickIcon(p);
    if (hit < 0) {
        Vec3 o, d;
        viewRay(p, o, d);
        // Dense meshes go through their cached BVH; small ones are faster brute force.
        hit = pickObject(scene_, o, d, nullptr, [this](int i, Vec3 lo, Vec3 ld, float& t) {
            const Object& obj = scene_.objects[i];
            if (obj.mesh.triangleCount() < 512) return raycastMesh(obj.mesh, lo, ld, t);
            return meshAccel(obj).raycast(lo, ld, t);
        });
    }
    if (!extend) selectOnly(hit);
    else if (hit >= 0) toggleSelect(hit);
}

int Editor::pickVertex(Vec2 p) {
    Object* o = activeMesh();
    if (!o) return -1;
    const Mat4 m = scene_.world(scene_.active), inv = inverse(m);
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
        bool hidden =
            meshAccel(*o).raycast(transformPoint(inv, origin), transformDir(inv, dir), t) && t < 1.0f - tolerance;
        if (!hidden) return candidates[c].second;
    }
    return -1;
}

void Editor::boxSelect(Vec2 a, Vec2 b, bool extend, bool subtract) {
    WorldCacheScope worldCache(scene_);
    const float x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
    auto inside = [&](Vec3 world) {
        Vec2 s;
        return worldToScreen(world, s) && s.x >= x0 && s.x <= x1 && s.y >= y0 && s.y <= y1;
    };
    if (mode_ == Mode::Edit) {
        boxSelectEdit(a, b, extend, subtract);
        return;
    }
    if (!extend && !subtract) selectOnly(-1);
    for (int i = 0; i < (int)scene_.objects.size(); ++i) {
        Object& o = scene_.objects[i];
        Vec3 center;
        if (o.isMesh()) {
            if (o.mesh.verts.empty()) continue;
            Vec3 lo = o.mesh.verts[0], hi = lo;
            for (const Vec3& v : o.mesh.verts) {
                lo = vmin(lo, v);
                hi = vmax(hi, v);
            }
            center = (lo + hi) * 0.5f;
        }
        if (!inside(transformPoint(scene_.world(i), center))) continue;
        if (subtract) {
            o.selected = false;
            if (scene_.active == i) scene_.active = -1;
        } else {
            o.selected = true;
            scene_.active = i;
        }
    }
}
