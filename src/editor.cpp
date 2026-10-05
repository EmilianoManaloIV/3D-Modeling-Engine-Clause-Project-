#include "editor_internal.h"
#include "image_io.h"
#include "profiler.h"
#include "skin.h"
#include "transform.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

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
    fontScale_ = std::max(1, (int)(dpi_ * 2.0f + 0.25f));
    buildGrid();

    if (!options.openPath.empty()) {
        openPath(options.openPath);
    } else if (options.demo) {
        buildDemo(options.demo);
    } else {
        buildDemo(0);
    }
    undo_.clear();
    redo_.clear();
    dirty_ = false;
    showHelp_ = options.showHelp;
    if (!options.renderOut.empty()) {
        renderOut_ = options.renderOut;
        if (options.renderDevice == "cpu") renderSet_.gpu = false;
        else if (options.renderDevice == "gpu") renderSet_.gpu = gpuTracerOk_;
        if (options.renderSamples > 0) renderSet_.samples = (float)options.renderSamples;
        if (options.renderPercent > 0) renderSet_.resolution = (float)options.renderPercent;
        renderView_ = true;  // started on the first frame, once the viewport size is known
        renderStamp_ = 0;
    }
    if (status_.empty()) setStatus("Welcome to Modeler3D - press F1 for help.");
    updateTitle();
    return true;
}

void Editor::shutdown() {
    shutdownRender();
    renderer_.shutdown();
}

void Editor::frame(const Input& in, int width, int height, float dt) {
    screenW_ = width;
    screenH_ = height;
    if (dt > 0) lastDt_ = std::min(dt, 0.1f);
    clock_ += dt;
    updateCameraAnimation(dt);
    if (benchScenario_ >= 0) benchmarkTick();
    if (dt > 0) fps_ = fps_ * 0.95f + (1.0f / dt) * 0.05f;
    statusTime_ += dt;
    swallowKeys_ = false;
    if (configDirty_ && !ui_.isActive()) {
        configDirty_ = false;
        saveConfig();
    }

    // Layout: tool panel | viewport | properties panel, status bar at bottom.
    const float fs = (float)fontScale_;
    const float lw = 136 * fs, rw = 140 * fs, sb = 12 * fs;
    leftPanel_ = {0, 0, lw, height - sb};
    rightPanel_ = {width - rw, 0, rw, height - sb};
    statusBar_ = {0, height - sb, (float)width, sb};
    viewport_ = {lw, 0, std::max(1.0f, width - lw - rw), std::max(1.0f, height - sb)};
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
    {
        PROF_SCOPE("particle sim");
        updateParticles(dt);
    }

    const bool modal = confirmQuit_ || showHelp_;
    ui_.begin(in, width, height, fontScale_);
    {
        PROF_SCOPE("ui build (panels)");
        ui_.setInputEnabled(xf_ == Xform::None && !modal);
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
        } else {
            if (!ui_.keyboardUsedThisFrame() && !ui_.wantsKeyboard()) handleShortcuts(in);
            handleViewport(in);
        }
    }
    updateMatrices();
    {
        PROF_SCOPE("ui build (overlays)");
        buildViewportOverlay();
        if (uvEditor_) buildUvEditor();
        if (showStats_) buildStatsOverlay();
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
Editor::Snapshot Editor::snapshot() const { return {scene_.objects, scene_.active, vsel_, scene_.ambient}; }

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
    if (xf_ != Xform::None || captureAction_ >= 0 || swallowKeys_) return;
    if (keys_.pressed(input::Action::Render, in)) {  // also works while the render view is up
        toggleRenderView();
        return;
    }
    using input::Action;
    // Unity: the fly keys (W A S D Q E) steer the camera while RMB is held,
    // so the tool shortcuts that share those keys pause during flight.
    if (nav_ == Nav::Fly) return;
    auto hit = [&](Action a) { return keys_.pressed(a, in); };
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

void Editor::handleViewport(const Input& in) {
    const bool unity = keymap_ == Keymap::Unity;
    const bool overUi = headerRect_.contains(in.mouseX, in.mouseY) || toolbarRect_.contains(in.mouseX, in.mouseY) ||
                        (uvEditor_ && uvRect_.contains(in.mouseX, in.mouseY));
    const bool overGizmo = sceneGizmoRect_.contains(in.mouseX, in.mouseY);
    const bool over = viewport_.contains(in.mouseX, in.mouseY) && !ui_.isActive() && !overUi && !overGizmo;
    if (nav_ == Nav::Fly && in.wheel != 0.0f) {
        // Unity: scrolling while flying changes the fly speed instead of zooming.
        flySpeed_ = clampf(flySpeed_ * std::pow(1.25f, in.wheel), 0.01f, 100.0f);
        setStatus(strf("Fly speed x%.2f", flySpeed_));
    } else if ((over || overGizmo) && in.wheel != 0.0f) {
        camAnimating_ = false;
        cam_.distance = clampf(cam_.distance * std::pow(0.85f, in.wheel * camSet_.zoomSpeed), 0.05f, 5000.0f);
    }

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

    if (nav_ == Nav::None && !lmbInViewport_ && handleUvEditor(in)) return;

    // --- scene gizmo (top-right): click an axis to look along it, the centre / label to toggle Persp-Iso ---
    sceneGizmoHover_ = (nav_ == Nav::None && !ui_.isActive()) ? pickSceneGizmo({in.mouseX, in.mouseY}) : -1;
    if (sceneGizmoHover_ >= 0 && in.mousePressed[MOUSE_LEFT] && !lmbInViewport_) {
        clickSceneGizmo(sceneGizmoHover_);
        return;
    }

    // --- arrow keys (rebindable) move the scene camera (Shift = faster) ---
    if (nav_ == Nav::None && !ui_.wantsKeyboard() && !ui_.keyboardUsedThisFrame() && captureAction_ < 0) {
        Vec3 flat = camForward();
        flat.y = 0;
        flat = length(flat) > 1e-4f ? normalize(flat) : camUp();
        Vec3 move;
        if (keys_.down(input::Action::CameraForward, in)) move += flat;
        if (keys_.down(input::Action::CameraBack, in)) move -= flat;
        if (keys_.down(input::Action::CameraRight, in)) move += camRight();
        if (keys_.down(input::Action::CameraLeft, in)) move -= camRight();
        if (length(move) > 0) {
            camAnimating_ = false;
            float speed = std::max(1.0f, cam_.distance) * 0.8f * camSet_.arrowSpeed *
                          (in.shift() ? camSet_.fastMultiplier : 1.0f);
            cam_.target += normalize(move) * (speed * lastDt_);
            updateMatrices();
        }
    }

    // --- camera navigation ---
    // Unity:   Alt+LMB orbit, MMB pan, Alt+RMB zoom, RMB fly (+WASDQE), Hand tool LMB pan.
    // Blender: RMB / MMB orbit, Shift = pan, Alt+LMB orbit.
    if (nav_ == Nav::None && over) {
        int b = -1;
        Nav kind = Nav::Orbit;
        if (unity) {
            if (in.mousePressed[MOUSE_MIDDLE]) b = MOUSE_MIDDLE, kind = Nav::Pan;
            else if (in.mousePressed[MOUSE_RIGHT]) b = MOUSE_RIGHT, kind = in.alt() ? Nav::Zoom : Nav::Fly;
            else if (in.mousePressed[MOUSE_LEFT] && in.alt()) b = MOUSE_LEFT, kind = Nav::Orbit;
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
            if (!unity) nav_ = in.shift() ? Nav::Pan : Nav::Orbit;
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
                case Nav::Fly: {
                    // Mouse-look around the eye, then WASD / QE flight (Shift = fast).
                    Vec3 eye = cam_.eye();
                    cam_.yaw -= dx * camSet_.lookSensitivity;
                    cam_.pitch = clampf(cam_.pitch + dy * camSet_.lookSensitivity, -89.9f, 89.9f);
                    float p = toRadians(cam_.pitch), y = toRadians(cam_.yaw);
                    Vec3 dir(std::cos(p) * std::sin(y), std::sin(p), std::cos(p) * std::cos(y));
                    cam_.target = eye - dir * cam_.distance;
                    updateMatrices();
                    Vec3 move;
                    using input::Action;
                    if (keys_.down(Action::FlyForward, in)) move += camForward();
                    if (keys_.down(Action::FlyBack, in)) move -= camForward();
                    if (keys_.down(Action::FlyRight, in)) move += camRight();
                    if (keys_.down(Action::FlyLeft, in)) move -= camRight();
                    if (keys_.down(Action::FlyUp, in)) move += Vec3(0, 1, 0);
                    if (keys_.down(Action::FlyDown, in)) move -= Vec3(0, 1, 0);
                    if (length(move) > 0) {
                        // Unity-style acceleration: speed ramps up to 4x while the keys stay held.
                        flyHold_ += lastDt_;
                        float accel = camSet_.flyAcceleration ? 1.0f + std::min(flyHold_, 2.0f) * 1.5f : 1.0f;
                        float speed = std::max(1.0f, cam_.distance) * 1.2f * flySpeed_ * accel *
                                      (in.shift() ? camSet_.fastMultiplier : 1.0f);
                        cam_.target += normalize(move) * (speed * lastDt_);
                    } else {
                        flyHold_ = 0;
                    }
                    break;
                }
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

    // --- selection: click, or drag a box ---
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
            else clickSelect(mouse_, in.shift() || (unity && in.ctrl()));
            lmbInViewport_ = boxSelecting_ = false;
        }
    }
}

int Editor::pickIcon(Vec2 p) {
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
        auto& sel = vertSel();
        int v = pickVertex(p);
        if (!extend) std::fill(sel.begin(), sel.end(), 0);
        if (v >= 0) sel[v] = extend ? !sel[v] : 1;
        return;
    }
    int hit = pickIcon(p);
    if (hit < 0) {
        Vec3 o, d;
        viewRay(p, o, d);
        hit = pickObject(scene_, o, d);
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
            raycastMesh(o->mesh, transformPoint(inv, origin), transformDir(inv, dir), t) && t < 1.0f - tolerance;
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
        Object* o = activeMesh();
        if (!o) return;
        auto& sel = vertSel();
        if (!extend && !subtract) std::fill(sel.begin(), sel.end(), 0);
        Mat4 m = scene_.world(scene_.active);
        for (size_t v = 0; v < sel.size(); ++v)
            if (inside(transformPoint(m, o->mesh.verts[v]))) sel[v] = subtract ? 0 : 1;
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
