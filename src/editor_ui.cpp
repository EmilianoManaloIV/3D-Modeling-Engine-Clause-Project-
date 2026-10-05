#include "editor_internal.h"
#include "skin.h"

#include <algorithm>
#include <cmath>
#include <functional>

using namespace ed;

namespace {
Color kindColor(ObjectKind k) {
    switch (k) {
        case ObjectKind::Mesh: return {0.75f, 0.77f, 0.82f, 1};
        case ObjectKind::Light: return kLightGizmo;
        case ObjectKind::Empty: return {0.9f, 0.9f, 0.9f, 1};
        case ObjectKind::Bone: return kBoneColor;
        default: return kEmitterGizmo;
    }
}
const char* const kShadingNames[SHADE_COUNT] = {"Studio", "Lit", "Checker", "Weights"};
}  // namespace

// ============================================================================
// Widgets helpers
// ============================================================================
void Editor::label(PanelLayout& L, const std::string& text, Color c) {
    const float fs = (float)fontScale_;
    Rect r = L.row(9 * fs);
    ui_.text(r.x, r.y + fs, ui_.fitText(text, r.w), c);
}

bool Editor::vec3Fields(PanelLayout& L, const char* key, uint32_t salt, Vec3& v, float speed, float lo, float hi,
                        const Color* colors) {
    const float gap = 3.0f * fontScale_;
    Rect row = L.row(ui_.rowHeight());
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

bool Editor::floatRow(PanelLayout& L, const char* text, const char* key, uint32_t salt, float& v, float speed,
                      float lo, float hi, bool integer) {
    Rect row = L.row(ui_.rowHeight());
    float lw = std::floor(row.w * 0.52f);
    ui_.textIn({row.x, row.y, lw, row.h}, text, theme::textDim, false);
    float value = v;
    uint32_t id = uiHash(key, salt);
    if (ui_.dragFloat(id, {row.x + lw, row.y, row.w - lw, row.h}, value, speed, theme::accent, lo, hi)) {
        if (integer) value = std::round(value);
        if (value != v) {
            beginEdit(id);
            v = value;
            return true;
        }
    }
    return false;
}

// ============================================================================
// Left panel: tabbed tools
// ============================================================================
void Editor::buildLeftPanel(const Input& in) {
    const float fs = (float)fontScale_;
    const float pad = 5 * fs, gap = 3 * fs, rowH = ui_.rowHeight(), headerH = 11 * fs;
    ui_.rect(leftPanel_, theme::panel);
    if (ui_.inputEnabled() && leftPanel_.contains(in.mouseX, in.mouseY) && in.wheel != 0.0f)
        leftScroll_ -= in.wheel * rowH * 2;
    leftScroll_ = clampf(leftScroll_, 0.0f, std::max(0.0f, leftContentH_ - leftPanel_.h));
    ui_.pushClip(leftPanel_);
    PanelLayout L{leftPanel_.x + pad, leftPanel_.y + pad - leftScroll_, leftPanel_.w - 2 * pad, gap};
    const float top = L.y;
    auto btn = [&](const char* id, const Rect& r, const char* text, bool toggled = false) {
        return ui_.button(uiHash(id), r, text, toggled);
    };
    auto header = [&](const char* text) { ui_.header(L.row(headerH), text); };
    auto note = [&](const char* text) { label(L, text, theme::textDim); };

    Rect title = L.row(rowH);
    ui_.text(title.x, title.y + (rowH - ui_.glyphH()) * 0.5f, "MODELER 3D", theme::accent);

    static const char* const kTabs[6] = {"Create", "Mesh", "UV", "Rig", "FX", "File"};
    for (int r = 0; r < 2; ++r) {
        Rect row = L.row(rowH);
        for (int c = 0; c < 3; ++c) {
            int t = r * 3 + c;
            if (ui_.button(uiHash("tab", (uint32_t)t), cell(row, c, 3, gap), kTabs[t], leftTab_ == t)) {
                leftTab_ = t;
                leftScroll_ = 0;
            }
        }
    }
    L.y += gap;

    auto modeAndTransform = [&]() {
        header("MODE  (Tab)");
        Rect row = L.row(rowH);
        if (btn("mode.object", cell(row, 0, 2, gap), "Object", mode_ == Mode::Object)) setMode(Mode::Object);
        if (btn("mode.edit", cell(row, 1, 2, gap), "Edit", mode_ == Mode::Edit)) setMode(Mode::Edit);
        header("TRANSFORM  (G R S)");
        row = L.row(rowH);
        if (btn("xf.move", cell(row, 0, 3, gap), "Move")) beginTransform(Xform::Grab, true);
        if (btn("xf.rotate", cell(row, 1, 3, gap), "Rotate")) beginTransform(Xform::Rotate, true);
        if (btn("xf.scale", cell(row, 2, 3, gap), "Scale")) beginTransform(Xform::Scale, true);
    };

    switch (leftTab_) {
        case 0: {  // Create
            header("PARAMETRIC SHAPES");
            for (int r = 0; r < 5; ++r) {
                Rect row = L.row(rowH);
                for (int c = 0; c < 2; ++c) {
                    int s = r * 2 + c;
                    if (ui_.button(uiHash("add", (uint32_t)s), cell(row, c, 2, gap), shapeDef(s).name)) addShape(s);
                }
            }
            header("LIGHTS");
            Rect row = L.row(rowH);
            if (btn("add.point", cell(row, 0, 3, gap), "Point")) addLight(LightType::Point);
            if (btn("add.sun", cell(row, 1, 3, gap), "Sun")) addLight(LightType::Sun);
            if (btn("add.spot", cell(row, 2, 3, gap), "Spot")) addLight(LightType::Spot);
            header("OTHER");
            row = L.row(rowH);
            if (btn("add.empty", cell(row, 0, 2, gap), "Empty")) addEmpty();
            if (btn("add.emitter", cell(row, 1, 2, gap), "Emitter")) addEmitter();
            modeAndTransform();
            header("OBJECT");
            row = L.row(rowH);
            if (btn("obj.dup", cell(row, 0, 2, gap), "Duplicate")) duplicateSelected();
            if (btn("obj.del", cell(row, 1, 2, gap), "Delete")) deleteSelected();
            row = L.row(rowH);
            if (btn("obj.parent", cell(row, 0, 2, gap), "Parent")) parentSelected();
            if (btn("obj.unparent", cell(row, 1, 2, gap), "Unparent")) unparentSelected();
            break;
        }
        case 1: {  // Mesh
            modeAndTransform();
            header(mode_ == Mode::Edit ? "MESH (EDIT)" : "MESH");
            Rect row = L.row(rowH);
            if (btn("mesh.all", cell(row, 0, 2, gap), "All/None")) selectAll();
            if (btn("mesh.extrude", cell(row, 1, 2, gap), "Extrude")) extrude();
            row = L.row(rowH);
            if (btn("mesh.del", cell(row, 0, 2, gap), "Delete")) deleteSelected();
            if (btn("mesh.dup", cell(row, 1, 2, gap), "Duplicate")) duplicateSelected();
            row = L.row(rowH);
            if (btn("mesh.subdiv", cell(row, 0, 2, gap), "Subdivide")) subdivideSelected();
            if (btn("mesh.flip", cell(row, 1, 2, gap), "Flip")) flipSelected();
            row = L.row(rowH);
            if (btn("mesh.flat", cell(row, 0, 2, gap), "Flat")) setSmoothSelected(false);
            if (btn("mesh.smooth", cell(row, 1, 2, gap), "Smooth")) setSmoothSelected(true);
            header("VIEW");
            row = L.row(rowH);
            if (btn("view.front", cell(row, 0, 3, gap), "Front")) setView(0, 0);
            if (btn("view.right", cell(row, 1, 3, gap), "Right")) setView(90, 0);
            if (btn("view.top", cell(row, 2, 3, gap), "Top")) setView(0, 89.9f);
            row = L.row(rowH);
            if (btn("view.ortho", cell(row, 0, 2, gap), "Ortho", cam_.ortho)) cam_.ortho = !cam_.ortho;
            if (btn("view.frame", cell(row, 1, 2, gap), "Frame")) frameSelected();
            row = L.row(rowH);
            if (btn("view.wire", cell(row, 0, 2, gap), "Wireframe", wireframe_)) wireframe_ = !wireframe_;
            if (btn("view.grid", cell(row, 1, 2, gap), "Grid", showGrid_)) showGrid_ = !showGrid_;
            break;
        }
        case 2: {  // UV
            header("UNWRAP  (U = Smart)");
            static const uv::Method kMethods[6] = {uv::Method::Smart,  uv::Method::Box,
                                                   uv::Method::Planar, uv::Method::Cylindrical,
                                                   uv::Method::Spherical, uv::Method::PerFace};
            static const char* const kNames[6] = {"Smart", "Box", "Planar", "Cylinder", "Sphere", "Per Face"};
            for (int r = 0; r < 3; ++r) {
                Rect row = L.row(rowH);
                for (int c = 0; c < 2; ++c) {
                    int k = r * 2 + c;
                    if (ui_.button(uiHash("unwrap", (uint32_t)k), cell(row, c, 2, gap), kNames[k]))
                        unwrapActive(kMethods[k]);
                }
            }
            header("UV TOOLS");
            Rect row = L.row(rowH);
            if (btn("uv.fit", cell(row, 0, 2, gap), "Fit 0-1")) uvTool(0);
            if (btn("uv.pack", cell(row, 1, 2, gap), "Pack")) uvTool(1);
            row = L.row(rowH);
            if (btn("uv.rot", cell(row, 0, 2, gap), "Rotate 90")) uvTool(2);
            if (btn("uv.flipu", cell(row, 1, 2, gap), "Flip U")) uvTool(3);
            row = L.row(rowH);
            if (btn("uv.flipv", cell(row, 0, 2, gap), "Flip V")) uvTool(4);
            if (btn("uv.editor", cell(row, 1, 2, gap), "UV Editor", uvEditor_)) uvEditor_ = !uvEditor_;
            row = L.row(rowH);
            if (btn("uv.checker", row, "Show Checker", shading_ == SHADE_CHECKER))
                shading_ = shading_ == SHADE_CHECKER ? SHADE_STUDIO : SHADE_CHECKER;
            note("Object mode: whole");
            note("mesh. Edit mode: the");
            note("selected faces.");
            note("Drag faces in the UV");
            note("editor to move UVs.");
            break;
        }
        case 3: {  // Rig
            header("BONES");
            Rect row = L.row(rowH);
            if (btn("rig.add", cell(row, 0, 2, gap), "Add Bone")) addBone(false);
            if (btn("rig.extrude", cell(row, 1, 2, gap), "Extrude")) addBone(true);
            row = L.row(rowH);
            if (btn("rig.rest", cell(row, 0, 2, gap), "Rest Pose")) resetPose();
            if (btn("rig.frame", cell(row, 1, 2, gap), "Frame")) frameSelected();
            header("SKIN");
            row = L.row(rowH);
            if (btn("rig.bind", cell(row, 0, 2, gap), "Bind")) bindSelected();
            if (btn("rig.unbind", cell(row, 1, 2, gap), "Unbind")) unbindSelected();
            row = L.row(rowH);
            if (btn("rig.auto", cell(row, 0, 2, gap), "Auto Wgt")) recomputeWeights();
            if (btn("rig.norm", cell(row, 1, 2, gap), "Normalize")) normalizeWeights();
            row = L.row(rowH);
            if (btn("rig.weights", row, "Weights View", shading_ == SHADE_WEIGHTS))
                shading_ = shading_ == SHADE_WEIGHTS ? SHADE_STUDIO : SHADE_WEIGHTS;
            header("HIERARCHY");
            row = L.row(rowH);
            if (btn("rig.parent", cell(row, 0, 2, gap), "Parent")) parentSelected();
            if (btn("rig.unparent", cell(row, 1, 2, gap), "Unparent")) unparentSelected();
            note("1 Add Bone; Extrude");
            note("  grows a chain.");
            note("2 Select the mesh,");
            note("  Shift+click the root");
            note("  bone, press Bind.");
            note("3 Pose: rotate (R).");
            note("4 Weights: Edit mode,");
            note("  Properties > Skin.");
            break;
        }
        case 4: {  // FX
            header("PARTICLES  (Space)");
            Rect row = L.row(rowH);
            if (btn("fx.emitter", cell(row, 0, 2, gap), "Emitter")) addEmitter();
            if (btn("fx.play", cell(row, 1, 2, gap), playing_ ? "Pause" : "Play", playing_)) togglePlay();
            row = L.row(rowH);
            if (btn("fx.restart", row, "Restart")) restartParticles();
            header("LIGHTS");
            row = L.row(rowH);
            if (btn("fx.point", cell(row, 0, 3, gap), "Point")) addLight(LightType::Point);
            if (btn("fx.sun", cell(row, 1, 3, gap), "Sun")) addLight(LightType::Sun);
            if (btn("fx.spot", cell(row, 2, 3, gap), "Spot")) addLight(LightType::Spot);
            row = L.row(rowH);
            if (btn("fx.lit", row, "Lit View", shading_ == SHADE_LIT))
                shading_ = shading_ == SHADE_LIT ? SHADE_STUDIO : SHADE_LIT;
            label(L, "Ambient light (R G B)", theme::textDim);
            vec3Fields(L, "ambient", 0, scene_.ambient, 0.003f, 0.0f, 1.0f, kRgbColor);
            note("Emission: raise it in");
            note("a mesh's Properties.");
            break;
        }
        default: {  // File
            header("FILE");
            ui_.textField(uiHash("file.name"), L.row(rowH), fileField_);
            Rect row = L.row(rowH);
            if (btn("file.new", cell(row, 0, 3, gap), "New")) newScene();
            if (btn("file.save", cell(row, 1, 3, gap), "Save")) saveFile();
            if (btn("file.load", cell(row, 2, 3, gap), "Load")) loadFile();
            header("WAVEFRONT .OBJ");
            row = L.row(rowH);
            if (btn("obj.export", cell(row, 0, 2, gap), "Export")) exportObj();
            if (btn("obj.import", cell(row, 1, 2, gap), "Import")) importObj();
            header("HISTORY");
            row = L.row(rowH);
            if (btn("undo", cell(row, 0, 2, gap), "Undo")) undo();
            if (btn("redo", cell(row, 1, 2, gap), "Redo")) redo();
            header("HELP");
            row = L.row(rowH);
            if (btn("help", cell(row, 0, 2, gap), "Keys (F1)", showHelp_)) showHelp_ = true;
            if (btn("shot", cell(row, 1, 2, gap), "Shot F12")) screenshotPending_ = true;
            break;
        }
    }

    leftContentH_ = (L.y - top) + 2 * pad;
    ui_.popClip();
    ui_.rect({leftPanel_.x + leftPanel_.w - 1, leftPanel_.y, 1, leftPanel_.h}, theme::border);
}

// ============================================================================
// Right panel: outliner + properties
// ============================================================================
void Editor::buildRightPanel(const Input& in) {
    const float fs = (float)fontScale_;
    const float pad = 5 * fs, gap = 3 * fs, rowH = ui_.rowHeight(), headerH = 11 * fs;
    ui_.rect(rightPanel_, theme::panel);
    if (ui_.inputEnabled() && in.wheel != 0.0f) {
        if (outlinerRect_.contains(in.mouseX, in.mouseY)) outlinerScroll_ -= in.wheel * rowH * 2;
        else if (rightPanel_.contains(in.mouseX, in.mouseY)) rightScroll_ -= in.wheel * rowH * 2;
    }
    rightScroll_ = clampf(rightScroll_, 0.0f, std::max(0.0f, rightContentH_ - rightPanel_.h));
    ui_.pushClip(rightPanel_);
    PanelLayout L{rightPanel_.x + pad, rightPanel_.y + pad - rightScroll_, rightPanel_.w - 2 * pad, gap};
    const float top = L.y;

    // --- Outliner: the hierarchy as an indented tree (depth-first) ---
    const int count = (int)scene_.objects.size();
    std::vector<std::pair<int, int>> rows;  // (object index, depth)
    {
        std::vector<char> visited(count, 0);
        std::function<void(int, int)> visit = [&](int i, int depth) {
            if (visited[i]) return;
            visited[i] = 1;
            rows.push_back({i, depth});
            for (int j = 0; j < count; ++j)
                if (scene_.parentIndex(j) == i) visit(j, depth + 1);
        };
        for (int i = 0; i < count; ++i)
            if (scene_.parentIndex(i) < 0) visit(i, 0);
        for (int i = 0; i < count; ++i)
            if (!visited[i]) visit(i, 0);  // safety net for broken links
    }
    ui_.header(L.row(headerH), strf("SCENE  (%d)", count));
    int visibleRows = std::max(4, std::min(count, 9));
    Rect list = L.row(visibleRows * rowH);
    outlinerRect_ = list;
    ui_.rect(list, theme::field);
    outlinerScroll_ = clampf(outlinerScroll_, 0.0f, std::max(0.0f, count * rowH - list.h));
    ui_.pushClip(list);
    int clicked = -1;
    for (int k = 0; k < (int)rows.size(); ++k) {
        const int i = rows[k].first;
        Rect r{list.x, list.y + k * rowH - outlinerScroll_, list.w, rowH};
        if (r.y + r.h < list.y || r.y > list.y + list.h) continue;
        const Object& o = scene_.objects[i];
        float indent = rows[k].second * 7.0f * fs;
        Rect textRect{r.x + indent + 6 * fs, r.y, r.w - indent - 6 * fs, r.h};
        bool pressed = ui_.selectable(uiHash("outliner", o.id), r, "", o.selected, i == scene_.active);
        if (rows[k].second > 0)
            ui_.rect({r.x + indent - 4 * fs, r.y + r.h * 0.5f, 3 * fs, std::max(1.0f, fs * 0.5f)}, theme::textDim);
        ui_.rect({r.x + indent + 1.5f * fs, r.y + r.h * 0.5f - 2 * fs, 4 * fs, 4 * fs}, kindColor(o.kind));
        ui_.textIn(textRect, o.name, (o.selected || i == scene_.active) ? theme::white : theme::text, false);
        if (pressed) clicked = i;
    }
    if (count == 0) ui_.textIn(list, "(empty scene)", theme::textDim, true);
    ui_.popClip();
    if (clicked >= 0) {
        if (mode_ == Mode::Edit) {
            if (clicked != scene_.active && scene_.objects[clicked].isMesh()) {
                selectOnly(clicked);
                vsel_.assign(scene_.objects[clicked].mesh.verts.size(), 0);
                if (makeEditable(scene_.objects[clicked])) markDirty();
            }
        } else if (in.shift()) {
            toggleSelect(clicked);
        } else {
            selectOnly(clicked);
        }
    }

    // --- Properties ---
    ui_.header(L.row(headerH), "PROPERTIES");
    if (Object* o = active()) {
        objectProperties(L, *o, in);
    } else {
        label(L, "No active object.", theme::textDim);
        label(L, "Click an object in", theme::textDim);
        label(L, "the viewport or list.", theme::textDim);
        ui_.header(L.row(headerH), "WORLD");
        label(L, "Ambient light (R G B)", theme::textDim);
        vec3Fields(L, "ambient2", 0, scene_.ambient, 0.003f, 0.0f, 1.0f, kRgbColor);
    }

    rightContentH_ = (L.y - top) + 2 * pad;
    ui_.popClip();
    ui_.rect({rightPanel_.x, rightPanel_.y, 1, rightPanel_.h}, theme::border);
}

void Editor::objectProperties(PanelLayout& L, Object& o, const Input& in) {
    (void)in;
    const float fs = (float)fontScale_;
    const float gap = 3 * fs, rowH = ui_.rowHeight(), headerH = 11 * fs;
    const uint32_t id = o.id;

    // Name
    Rect r = L.row(rowH);
    const float labelW = 34 * fs;
    ui_.textIn({r.x, r.y, labelW, r.h}, "Name", theme::textDim, false);
    std::string name = o.name;
    uint32_t nameId = uiHash("name", id);
    if (ui_.textField(nameId, {r.x + labelW, r.y, r.w - labelW, r.h}, name)) {
        name = trimmed(name);
        if (!name.empty() && name != o.name) {
            beginEdit(nameId);
            o.name = scene_.uniqueName(name, scene_.active);
        }
    }
    int p = scene_.indexOf(o.parent);
    label(L, strf("%s%s%s", kindName(o.kind), p >= 0 ? "  -  child of " : "", p >= 0 ? scene_.objects[p].name.c_str() : ""),
          theme::textDim);

    label(L, o.parent ? "Location (in parent)" : "Location", theme::textDim);
    vec3Fields(L, "loc", id, o.position, 0.02f, -1e6f, 1e6f, kAxisColor);
    label(L, "Rotation (degrees)", theme::textDim);
    vec3Fields(L, "rot", id, o.rotation, 0.5f, -1e6f, 1e6f, kAxisColor);
    label(L, "Scale", theme::textDim);
    vec3Fields(L, "scl", id, o.scale, 0.01f, -1e4f, 1e4f, kAxisColor);

    if (o.isMesh()) {
        // --- Parametric recipe ---
        if (o.param.active()) {
            const ShapeDef& def = shapeDef(o.param.shape);
            ui_.header(L.row(headerH), strf("PARAMETRIC: %s", def.name));
            bool changed = false;
            for (int k = 0; k < def.count; ++k) {
                const ParamDef& pd = def.params[k];
                changed |= floatRow(L, pd.name, "param", id * 16u + k, o.param.p[k], pd.speed, pd.lo, pd.hi, pd.integer);
            }
            float sub = (float)o.param.subdivisions;
            if (floatRow(L, "Subdiv", "param.sub", id, sub, 0.05f, 0, 3, true)) {
                o.param.subdivisions = (int)sub;
                changed = true;
            }
            changed |= floatRow(L, "Twist", "param.twist", id, o.param.twist, 0.5f, -3600, 3600);
            changed |= floatRow(L, "Taper", "param.taper", id, o.param.taper, 0.005f, 0, 10);
            if (changed) regenerate(o);
            Rect row = L.row(rowH);
            if (ui_.button(uiHash("param.bake", id), row, "Bake to editable mesh")) {
                pushUndo();
                makeEditable(o);
                markDirty();
                setStatus("Baked - the shape is now a plain mesh (Edit mode works on it)");
            }
        }

        // --- Material ---
        ui_.header(L.row(headerH), "MATERIAL");
        label(L, "Base color (R G B)", theme::textDim);
        vec3Fields(L, "col", id, o.color, 0.004f, 0.0f, 1.0f, kRgbColor);
        for (int rr = 0; rr < 2; ++rr) {
            Rect sw = L.row(std::floor(rowH * 0.8f));
            for (int c = 0; c < 5; ++c) {
                int idx = rr * 5 + c;
                bool same = length(o.color - kPalette[idx]) < 1e-3f;
                uint32_t sid = uiHash("swatch", (uint32_t)idx);
                if (ui_.swatch(sid, cell(sw, c, 5, gap), toColor(kPalette[idx]), same)) {
                    beginEdit(sid);
                    for (Object& other : scene_.objects)
                        if (other.isMesh() && (other.selected || &other == &o)) other.color = kPalette[idx];
                }
            }
        }
        label(L, "Emission color (RGB)", theme::textDim);
        vec3Fields(L, "emi", id, o.emission, 0.004f, 0.0f, 1.0f, kRgbColor);
        floatRow(L, "Emission", "emi.str", id, o.emissionStrength, 0.02f, 0.0f, 100.0f);
        floatRow(L, "Gloss", "gloss", id, o.gloss, 0.005f, 0.0f, 1.0f);
        Rect row = L.row(rowH);
        if (ui_.button(uiHash("shade.flat"), cell(row, 0, 2, gap), "Flat", !o.smooth) && o.smooth) setSmoothSelected(false);
        if (ui_.button(uiHash("shade.smooth"), cell(row, 1, 2, gap), "Smooth", o.smooth) && !o.smooth)
            setSmoothSelected(true);
        label(L, strf("%d verts, %d faces", (int)o.mesh.verts.size(), (int)o.mesh.faces.size()), theme::textDim);
        label(L, o.mesh.hasUVs() ? "Has UVs" : "No UVs (UV tab)", theme::textDim);

        // --- Skin ---
        if (!o.skinBones.empty()) {
            ui_.header(L.row(headerH), strf("SKIN  (%d bones)", (int)o.skinBones.size()));
            label(L, "Click a bone to view:", theme::textDim);
            for (int s = 0; s < (int)o.skinBones.size(); ++s) {
                int b = scene_.indexOf(o.skinBones[s]);
                Rect br = L.row(rowH);
                std::string bname = b >= 0 ? scene_.objects[b].name : std::string("(deleted bone)");
                if (ui_.selectable(uiHash("skin.bone", id * 64u + s), br, "  " + bname, false, weightSlot_ == s)) {
                    weightSlot_ = s;
                    shading_ = SHADE_WEIGHTS;
                }
            }
            if (mode_ == Mode::Edit && &o == activeMesh()) {
                floatRow(L, "Weight", "skin.w", 0, weightValue_, 0.005f, 0.0f, 1.0f);
                row = L.row(rowH);
                if (ui_.button(uiHash("skin.assign"), cell(row, 0, 2, gap), "Assign")) assignWeight(false);
                if (ui_.button(uiHash("skin.remove"), cell(row, 1, 2, gap), "Remove")) assignWeight(true);
            } else {
                label(L, "Edit mode: assign", theme::textDim);
                label(L, "weights to vertices.", theme::textDim);
            }
        }

        // --- Edit-mode selection ---
        if (mode_ == Mode::Edit && &o == activeMesh()) {
            ui_.header(L.row(headerH), "SELECTED VERTICES");
            auto& sel = vertSel();
            int n = 0;
            Vec3 median;
            for (size_t v = 0; v < sel.size(); ++v)
                if (sel[v]) {
                    median += o.mesh.verts[v];
                    ++n;
                }
            label(L, strf("%d of %d selected", n, (int)sel.size()), theme::textDim);
            if (n > 0) {
                median = median / float(n);
                label(L, "Median (object space)", theme::textDim);
                Vec3 edited = median;
                if (vec3Fields(L, "median", id, edited, 0.01f, -1e6f, 1e6f, kAxisColor)) {
                    Vec3 delta = edited - median;
                    for (size_t v = 0; v < sel.size(); ++v)
                        if (sel[v]) o.mesh.verts[v] += delta;
                    o.mesh.touch();
                }
            }
        }
    } else if (o.kind == ObjectKind::Light) {
        LightSettings& ls = o.light;
        ui_.header(L.row(headerH), "LIGHT");
        Rect row = L.row(rowH);
        static const char* const kTypes[3] = {"Point", "Sun", "Spot"};
        for (int t = 0; t < 3; ++t)
            if (ui_.button(uiHash("light.type", (uint32_t)t), cell(row, t, 3, gap), kTypes[t], (int)ls.type == t)) {
                beginEdit(uiHash("light.type", id));
                ls.type = (LightType)t;
            }
        label(L, "Color (R G B)", theme::textDim);
        vec3Fields(L, "light.col", id, ls.color, 0.004f, 0.0f, 1.0f, kRgbColor);
        floatRow(L, "Intensity", "light.int", id, ls.intensity, 0.1f, 0.0f, 10000.0f);
        if (ls.type != LightType::Sun) floatRow(L, "Range", "light.range", id, ls.range, 0.1f, 0.1f, 10000.0f);
        if (ls.type == LightType::Spot) {
            floatRow(L, "Cone angle", "light.angle", id, ls.spotAngle, 0.3f, 1.0f, 179.0f);
            floatRow(L, "Edge blend", "light.blend", id, ls.spotBlend, 0.005f, 0.0f, 1.0f);
        }
        label(L, ls.type == LightType::Point ? "Shines all around." : "Shines along local -Y",
              theme::textDim);
        if (shading_ != SHADE_LIT) label(L, "Lit view shows it.", theme::selection);
    } else if (o.kind == ObjectKind::Bone) {
        ui_.header(L.row(headerH), "BONE");
        floatRow(L, "Length", "bone.len", id, o.boneLength, 0.01f, 0.01f, 1000.0f);
        label(L, o.hasRest ? "Bound (has rest pose)" : "Not bound yet.", theme::textDim);
        Rect row = L.row(rowH);
        if (ui_.button(uiHash("bone.child", id), cell(row, 0, 2, gap), "Extrude")) addBone(true);
        if (ui_.button(uiHash("bone.rest", id), cell(row, 1, 2, gap), "Rest Pose")) resetPose();
    } else if (o.kind == ObjectKind::Empty) {
        ui_.header(L.row(headerH), "EMPTY");
        floatRow(L, "Display size", "empty.size", id, o.boneLength, 0.01f, 0.01f, 1000.0f);
        label(L, "Parent objects to it", theme::textDim);
        label(L, "to move them together", theme::textDim);
    } else if (o.kind == ObjectKind::Emitter) {
        ParticleSettings& ps = o.particles;
        ui_.header(L.row(headerH), "PARTICLES");
        floatRow(L, "Rate /s", "ps.rate", id, ps.rate, 0.5f, 0.0f, 5000.0f);
        floatRow(L, "Lifetime", "ps.life", id, ps.lifetime, 0.01f, 0.05f, 60.0f);
        floatRow(L, "Speed", "ps.speed", id, ps.speed, 0.02f, 0.0f, 1000.0f);
        floatRow(L, "Spread (deg)", "ps.spread", id, ps.spread, 0.3f, 0.0f, 180.0f);
        floatRow(L, "Gravity", "ps.grav", id, ps.gravity, 0.02f, -1000.0f, 1000.0f);
        floatRow(L, "Drag", "ps.drag", id, ps.drag, 0.005f, 0.0f, 20.0f);
        floatRow(L, "Start size", "ps.s0", id, ps.startSize, 0.005f, 0.0f, 100.0f);
        floatRow(L, "End size", "ps.s1", id, ps.endSize, 0.005f, 0.0f, 100.0f);
        floatRow(L, "Spawn radius", "ps.rad", id, ps.radius, 0.005f, 0.0f, 100.0f);
        label(L, "Start color (R G B)", theme::textDim);
        vec3Fields(L, "ps.c0", id, ps.startColor, 0.004f, 0.0f, 1.0f, kRgbColor);
        label(L, "End color (R G B)", theme::textDim);
        vec3Fields(L, "ps.c1", id, ps.endColor, 0.004f, 0.0f, 1.0f, kRgbColor);
        Rect row = L.row(rowH);
        if (ui_.button(uiHash("ps.glow"), cell(row, 0, 2, gap), "Glow", ps.additive) && !ps.additive) {
            beginEdit(uiHash("ps.blend", id));
            ps.additive = true;
        }
        if (ui_.button(uiHash("ps.smoke"), cell(row, 1, 2, gap), "Smoke", !ps.additive) && ps.additive) {
            beginEdit(uiHash("ps.blend", id));
            ps.additive = false;
        }
        auto it = particles_.find(id);
        label(L, strf("%d alive (%s)", it == particles_.end() ? 0 : (int)it->second.particles.size(),
                      playing_ ? "playing" : "paused"),
              theme::textDim);
        label(L, "Emits along local +Y.", theme::textDim);
    }
}

// ============================================================================
// Status bar, viewport header and overlays
// ============================================================================
void Editor::buildStatusBar() {
    const float fs = (float)fontScale_;
    ui_.rect(statusBar_, theme::panelDark);
    ui_.rect({statusBar_.x, statusBar_.y, statusBar_.w, 1}, theme::border);
    std::string right = strf("%s | %d objects | %d tris | %.0f fps", mode_ == Mode::Edit ? "Edit" : "Object",
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
        msg = "Click/drag: select vertices | A all | G/R/S | E extrude | U unwrap | X delete | Tab: object mode";
        c = theme::textDim;
    } else {
        msg = "Click: select | RMB drag: orbit | Shift+RMB: pan | G/R/S | Ctrl+P parent | Z shading | F1 help";
        c = theme::textDim;
    }
    ui_.textIn({statusBar_.x + 2 * fs, statusBar_.y, statusBar_.w - rw - 4 * fs, statusBar_.h}, msg, c, false);
    ui_.textIn({statusBar_.x + statusBar_.w - rw, statusBar_.y, rw, statusBar_.h}, right, theme::textDim, false);
}

void Editor::buildViewportHeader() {
    const float fs = (float)fontScale_;
    const float gap = 3 * fs, pad = 3 * fs;
    ui_.rect(headerRect_, withAlpha(theme::panelDark, 0.85f));
    ui_.pushClip(headerRect_);
    float x = headerRect_.x + pad;
    const float y = headerRect_.y + pad, h = ui_.rowHeight();
    auto button = [&](const char* id, const char* text, bool toggled) {
        float w = ui_.textWidth(text) + 8 * fs;
        bool clicked = ui_.button(uiHash(id), {x, y, w, h}, text, toggled);
        x += w + gap;
        return clicked;
    };
    for (int s = 0; s < SHADE_COUNT; ++s) {
        float w = ui_.textWidth(kShadingNames[s]) + 8 * fs;
        if (ui_.button(uiHash("hdr.shade", (uint32_t)s), {x, y, w, h}, kShadingNames[s], shading_ == s)) shading_ = s;
        x += w + gap;
    }
    x += 3 * gap;
    if (button("hdr.uv", "UV Map", uvEditor_)) uvEditor_ = !uvEditor_;
    if (button("hdr.play", playing_ ? "Pause" : "Play", playing_)) togglePlay();
    ui_.popClip();
}

void Editor::buildViewportOverlay() {
    const float fs = (float)fontScale_;
    ui_.pushClip(viewport_);
    const float x = viewport_.x + 6 * fs;
    float y = headerRect_.y + headerRect_.h + 4 * fs;
    std::string info = std::string(cam_.ortho ? "Orthographic" : "Perspective") +
                       (mode_ == Mode::Edit ? "  |  Edit Mode" : "  |  Object Mode");
    ui_.text(x, y, info, withAlpha(theme::text, 0.8f));
    y += 10 * fs;
    if (mode_ == Mode::Edit && active()) {
        ui_.text(x, y, "Editing: " + active()->name, withAlpha(theme::selection, 0.9f));
        y += 10 * fs;
    }
    if (shading_ == SHADE_LIT) {
        int lights = 0;
        for (const Object& o : scene_.objects) lights += o.kind == ObjectKind::Light;
        ui_.text(x, y,
                 lights ? strf("Lit: %d light%s%s", lights, lights == 1 ? "" : "s", lights > kMaxLights ? " (8 used)" : "")
                        : std::string("Lit: no lights - add one (Create tab)"),
                 lights ? withAlpha(theme::textDim, 0.9f) : theme::selection);
        y += 10 * fs;
    } else if (shading_ == SHADE_WEIGHTS) {
        Object* o = active();
        std::string boneName;
        if (o && o->kind == ObjectKind::Bone) {
            boneName = o->name;
        } else if (o && o->isMesh() && weightSlotFor(*o) >= 0) {
            int b = scene_.indexOf(o->skinBones[weightSlotFor(*o)]);
            if (b >= 0) boneName = scene_.objects[b].name;
        }
        ui_.text(x, y, boneName.empty() ? "Weights: select a bone (or pick one in Properties > Skin)"
                                        : "Weights of " + boneName + "  (blue 0 .. red 1)",
                 withAlpha(theme::textDim, 0.9f));
        y += 10 * fs;
    }
    if (wireframe_) {
        ui_.text(x, y, "Wireframe (x-ray selection)", withAlpha(theme::textDim, 0.9f));
        y += 10 * fs;
    }
    if (xf_ != Xform::None) ui_.text(x, y, xfInfo_, theme::selection);

    if (scene_.objects.empty()) {
        const std::string msg = "Empty scene - add something from the Create tab";
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
        const char* nm = a.i == 0 ? "X" : a.i == 1 ? "Y" : "Z";
        ui_.text(ex - 2.5f * fs, ey - 3.5f * fs, nm, withAlpha(theme::panelDark, alpha));
    }
    ui_.popClip();
}

// UV editor overlay: the 0-1 UV square with a checker backdrop and the UV
// layout of the active mesh (selected faces highlighted).
void Editor::buildUvEditor() {
    const float fs = (float)fontScale_;
    const Rect r = uvRect_;
    if (r.w <= 0) return;
    ui_.rect({r.x - fs, r.y - 10 * fs, r.w + 2 * fs, r.h + 11 * fs}, withAlpha(theme::panelDark, 0.95f));
    ui_.text(r.x, r.y - 9 * fs, "UV EDITOR", theme::textDim);
    const int n = 8;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            float c = ((i + j) & 1) ? 0.30f : 0.22f;
            ui_.rect({r.x + r.w * i / n, r.y + r.h * j / n, r.w / n + 0.5f, r.h / n + 0.5f}, {c, c, c + 0.02f, 1});
        }
    Object* o = activeMesh();
    if (!o || !o->mesh.hasUVs()) {
        ui_.textIn({r.x, r.y + r.h * 0.45f, r.w, 10 * fs}, o ? "No UVs - unwrap (UV tab)" : "Select a mesh",
                   theme::text, true);
        ui_.border(r, theme::border, fs);
        return;
    }
    ui_.pushClip(r);
    std::vector<char> faceSel(o->mesh.faces.size(), 0);
    if (mode_ == Mode::Edit)
        for (int f : selectedFaces(o->mesh, vertSel())) faceSel[f] = 1;
    auto toScreen = [&](Vec2 t) { return Vec2(r.x + t.x * r.w, r.y + (1.0f - t.y) * r.h); };
    const float thick = std::max(1.0f, fs * 0.5f);
    const size_t limit = std::min<size_t>(o->mesh.faces.size(), 20000);
    for (int pass = 0; pass < 2; ++pass)
        for (size_t f = 0; f < limit; ++f) {
            if ((faceSel[f] != 0) != (pass == 1)) continue;
            const auto& uvs = o->mesh.uvs[f];
            Color c = pass ? withAlpha(theme::selection, 0.95f) : Color{0.85f, 0.88f, 0.95f, 0.55f};
            for (size_t i = 0; i < uvs.size(); ++i) {
                Vec2 a = toScreen(uvs[i]), b = toScreen(uvs[(i + 1) % uvs.size()]);
                ui_.line(a.x, a.y, b.x, b.y, thick, c);
            }
        }
    ui_.popClip();
    ui_.border(r, theme::textDim, std::max(1.0f, fs * 0.5f));
}

void Editor::buildHelp() {
    static const char* const kRows[][2] = {
        {"MOUSE", ""},
        {"Left click / drag", "Select / box select (Shift add, Ctrl remove)"},
        {"Right / middle drag", "Orbit (also Alt + left); Shift: pan"},
        {"Mouse wheel", "Zoom"},
        {"", ""},
        {"KEYBOARD", ""},
        {"G / R / S", "Move / rotate / scale (X Y Z lock, Ctrl snap)"},
        {"  Left click / Enter", "Confirm     Right click / Esc: cancel"},
        {"Tab", "Object / Edit (vertex) mode"},
        {"A / E / X", "Select all / extrude faces / delete"},
        {"Shift + D", "Duplicate (keeps hierarchy and rigs)"},
        {"Ctrl+P / Alt+P", "Parent to active / clear parent"},
        {"U", "Smart UV unwrap"},
        {"Z", "Shading: studio / lit / UV checker / weights"},
        {"Space", "Play / pause particles"},
        {"F", "Frame selection"},
        {"1 / 3 / 7  (Ctrl)", "Front / right / top view (opposite)"},
        {"5 / W", "Perspective-ortho / wireframe + x-ray"},
        {"Ctrl+Z / Ctrl+Y", "Undo / redo"},
        {"Ctrl+S / Ctrl+O", "Save / load the scene named in File"},
        {"F12", "Save a screenshot (PNG)"},
        {"F1 or H", "This help"},
    };
    const int n = (int)(sizeof kRows / sizeof kRows[0]);
    const float fs = (float)fontScale_;
    const float pad = 10 * fs, lineH = 10 * fs;
    const float keyW = 22 * 6 * fs, descW = 46 * 6 * fs;
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
