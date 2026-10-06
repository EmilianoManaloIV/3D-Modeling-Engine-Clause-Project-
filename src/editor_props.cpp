// Right panel: the scene outliner (hierarchy with drag & drop) and the
// Properties of the active object in tabs.
#include "editor_internal.h"
#include "image_load.h"
#include "skin.h"
#include "transform.h"

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
        case ObjectKind::Camera: return {0.55f, 0.85f, 1.0f, 1};
        default: return kEmitterGizmo;
    }
}
}  // namespace

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
        std::vector<std::vector<int>> children(count);
        std::vector<int> roots;
        for (int j = 0; j < count; ++j) {
            int p = scene_.parentIndex(j);
            if (p >= 0) children[p].push_back(j);
            else roots.push_back(j);
        }
        // Depth-first with an explicit stack: a 100k-deep chain must not
        // overflow the call stack.
        std::vector<std::pair<int, int>> stack;
        auto visit = [&](int start) {
            stack.push_back({start, 0});
            while (!stack.empty()) {
                auto [i, depth] = stack.back();
                stack.pop_back();
                if (visited[i]) continue;
                visited[i] = 1;
                rows.push_back({i, depth});
                for (int k = (int)children[i].size() - 1; k >= 0; --k) stack.push_back({children[i][k], depth + 1});
            }
        };
        for (int i : roots) visit(i);
        for (int i = 0; i < count; ++i)
            if (!visited[i]) visit(i);  // safety net for broken links / cycles
    }
    const bool listOpen = section(L, "outliner", strf("SCENE  (%d)", count).c_str());
    int visibleRows = listOpen ? std::max(4, std::min(count, 9)) : 0;
    Rect list = listOpen ? L.row(visibleRows * rowH) : Rect{L.x, L.y, L.w, 0};
    outlinerRect_ = list;
    ui_.rect(list, theme::field);
    outlinerScroll_ = clampf(outlinerScroll_, 0.0f, std::max(0.0f, count * rowH - list.h));
    ui_.pushClip(list);
    int clicked = -1;
    // Drag & drop: where would a drop land?
    const bool mouseInList = list.contains(in.mouseX, in.mouseY);
    int dropRow = -1, dropZone = DropNone;
    if (dragActive_ && mouseInList) {
        int k = (int)std::floor((in.mouseY - list.y + outlinerScroll_) / rowH);
        if (k >= 0 && k < (int)rows.size()) {
            float fy = (in.mouseY - (list.y + k * rowH - outlinerScroll_)) / rowH;
            dropRow = k;
            dropZone = fy < 0.25f ? DropBefore : fy > 0.75f ? DropAfter : DropOnto;
        } else {
            dropZone = DropRoot;
        }
    }
    for (int k = 0; k < (int)rows.size(); ++k) {
        const int i = rows[k].first;
        Rect r{list.x, list.y + k * rowH - outlinerScroll_, list.w, rowH};
        if (r.y + r.h < list.y || r.y > list.y + list.h) continue;
        const Object& o = scene_.objects[i];
        float indent = std::min(rows[k].second, 12) * 7.0f * fs;  // deep trees: indent stops at 12 levels
        Rect textRect{r.x + indent + 6 * fs, r.y, r.w - indent - 6 * fs, r.h};
        bool pressed = ui_.selectable(uiHash("outliner", o.id), r, "", o.selected, i == scene_.active);
        if (rows[k].second > 0)
            ui_.rect({r.x + indent - 4 * fs, r.y + r.h * 0.5f, 3 * fs, std::max(1.0f, fs * 0.5f)}, theme::textDim);
        ui_.rect({r.x + indent + 1.5f * fs, r.y + r.h * 0.5f - 2 * fs, 4 * fs, 4 * fs}, kindColor(o.kind));
        ui_.textIn(textRect, o.name, (o.selected || i == scene_.active) ? theme::white : theme::text, false);
        if (pressed) clicked = i;
        if (k == dropRow) {
            if (dropZone == DropOnto) ui_.border(r, theme::selection, (float)fs);
            else ui_.rect({r.x, dropZone == DropBefore ? r.y : r.y + r.h - fs, r.w, 2.0f * fs}, theme::selection);
        }
    }
    if (dropZone == DropRoot && !rows.empty()) {
        float y = std::min(list.y + list.h - 2 * fs, list.y + rows.size() * rowH - outlinerScroll_);
        ui_.rect({list.x, y, list.w, 2.0f * fs}, theme::selection);
    }
    if (count == 0) ui_.textIn(list, "(empty scene)", theme::textDim, true);
    ui_.popClip();

    // Start a drag after the pressed row moved a few pixels; drop on release.
    if (dragRowId_ && in.mouseDown[MOUSE_LEFT] && !dragActive_ &&
        length(Vec2(in.mouseX, in.mouseY) - dragPress_) > 6.0f * dpi_)
        dragActive_ = true;
    if (dragActive_) {
        int di = scene_.indexOf(dragRowId_);
        const bool many = di >= 0 && scene_.objects[di].selected && scene_.selectedCount() > 1;
        std::string label = many ? strf("%d objects", scene_.selectedCount()) : di >= 0 ? scene_.objects[di].name : "";
        ui_.rect({in.mouseX + 10 * fs, in.mouseY + 4 * fs, ui_.textWidth(label) + 8 * fs, rowH}, withAlpha(theme::panelDark, 0.9f));
        ui_.text(in.mouseX + 14 * fs, in.mouseY + 4 * fs + (rowH - ui_.glyphH()) * 0.5f, label, theme::white);
    }
    if (dragRowId_ && !in.mouseDown[MOUSE_LEFT]) {
        if (dragActive_) {
            int di = scene_.indexOf(dragRowId_);
            std::vector<uint32_t> ids;
            if (di >= 0 && scene_.objects[di].selected) {
                for (const Object& o : scene_.objects)
                    if (o.selected) ids.push_back(o.id);
            } else if (di >= 0) {
                ids.push_back(dragRowId_);
            }
            if (!ids.empty() && dropZone != DropNone) {
                uint32_t target = dropRow >= 0 ? scene_.objects[rows[dropRow].first].id : 0;
                pushUndo();
                if (moveInHierarchy(ids, target, dropZone)) {
                    markDirty();
                    static const char* what[] = {"", "Moved before", "Parented to", "Moved after", "Unparented"};
                    setStatus(dropZone == DropRoot ? strf("Unparented %d object(s)", (int)ids.size())
                                                   : strf("%s %s", what[dropZone], scene_.objects[scene_.indexOf(target)].name.c_str()));
                } else {
                    undo_.pop_back();
                    setStatus("Can't parent an object to itself or to one of its children", true);
                }
            }
        } else if (dragDeferSelect_) {
            int di = scene_.indexOf(dragRowId_);
            if (di >= 0) selectOnly(di);
        }
        dragRowId_ = 0;
        dragActive_ = dragDeferSelect_ = false;
    }
    if (clicked >= 0 && mode_ == Mode::Object) {
        dragRowId_ = scene_.objects[clicked].id;
        dragPress_ = {in.mouseX, in.mouseY};
        dragActive_ = dragDeferSelect_ = false;
    }
    if (clicked >= 0) {
        const uint32_t cid = scene_.objects[clicked].id;
        const bool doubleClick = cid == lastOutlinerClickId_ && clock_ - lastOutlinerClickTime_ < 0.4 &&
                                 !in.ctrl() && !in.shift();
        lastOutlinerClickId_ = cid;
        lastOutlinerClickTime_ = doubleClick ? -1.0 : clock_;
        if (doubleClick && mode_ == Mode::Object) {
            selectOnly(clicked);
            frameSelected();  // Unity: double-click in the Hierarchy frames the object
            clicked = -1;
        }
    }
    if (clicked >= 0) {
        if (mode_ == Mode::Edit) {
            if (clicked != scene_.active && scene_.objects[clicked].isMesh()) {
                selectOnly(clicked);
                vsel_.assign(scene_.objects[clicked].mesh.verts.size(), 0);
                if (makeEditable(scene_.objects[clicked])) markDirty();
            }
        } else if (!in.ctrl() && !in.shift() && scene_.objects[clicked].selected && scene_.selectedCount() > 1) {
            // A plain press on one of several selected rows may start a drag of
            // all of them: only select it alone if the mouse is released in place.
            dragDeferSelect_ = true;
            scene_.active = clicked;
            outlinerAnchorId_ = scene_.objects[clicked].id;
        } else if (in.shift()) {
            // Shift+click: select the range of rows from the anchor (the last
            // plain / Ctrl click) to here; Ctrl+Shift adds the range.
            int from = -1, to = -1;
            for (int k = 0; k < (int)rows.size(); ++k) {
                if (rows[k].first == clicked) to = k;
                if (scene_.objects[rows[k].first].id == outlinerAnchorId_) from = k;
            }
            if (from < 0) from = to;
            if (!in.ctrl())
                for (Object& o : scene_.objects) o.selected = false;
            for (int k = std::min(from, to); k <= std::max(from, to); ++k) scene_.objects[rows[k].first].selected = true;
            scene_.active = clicked;
            setStatus(strf("%d objects selected", scene_.selectedCount()));
        } else if (in.ctrl()) {
            toggleSelect(clicked);  // Ctrl+click: add / remove one object
            outlinerAnchorId_ = scene_.objects[clicked].id;
            setStatus(strf("%d objects selected", scene_.selectedCount()));
        } else {
            selectOnly(clicked);
            outlinerAnchorId_ = scene_.objects[clicked].id;
        }
    }

    // --- Properties ---
    ui_.header(L.row(headerH), "PROPERTIES");
    (void)gap;
    if (scene_.selectedCount() > 1)
        label(L, strf("%d selected (Ctrl/Shift+click)", scene_.selectedCount()), theme::accent);
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

// Properties of the active object, in tabs: Object (transform, shape,
// mesh info, skin), Material (meshes) or the kind's own settings (light,
// camera, particles, bone, empty).
void Editor::objectProperties(PanelLayout& L, Object& o, const Input& in) {
    (void)in;
    const float fs = (float)fontScale_, gap = 3 * fs, rowH = ui_.rowHeight();
    const char* second = o.isMesh() ? "Material"
                         : o.kind == ObjectKind::Light ? "Light"
                         : o.kind == ObjectKind::Camera ? "Camera"
                         : o.kind == ObjectKind::Emitter ? "Particles"
                         : o.kind == ObjectKind::Bone ? "Bone" : "Empty";
    Rect row = L.row(rowH);
    int tab = propsTab_ == 0 ? 0 : 1;
    if (ui_.button(uiHash("props.tab0"), cell(row, 0, 2, gap), "Object", tab == 0)) propsTab_ = 0, tab = 0;
    ui_.tooltip("Name, transform, shape settings, mesh info");
    if (ui_.button(uiHash("props.tab1"), cell(row, 1, 2, gap), second, tab == 1)) propsTab_ = o.isMesh() ? 1 : 2, tab = 1;
    ui_.tooltip(o.isMesh() ? "Colour, metal, roughness, glass, PBR texture maps" : "Settings of this object type");
    L.y += gap;
    if (tab == 0) transformProperties(L, o);
    else if (o.isMesh()) materialProperties(L, o);
    else dataProperties(L, o);
}

void Editor::transformProperties(PanelLayout& L, Object& o) {
    const float fs = (float)fontScale_;
    const float gap = 3 * fs, rowH = ui_.rowHeight(), headerH = 11 * fs;
    const uint32_t id = o.id;
    (void)gap;
    (void)headerH;
    (void)fs;
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

    label(L, o.parent ? "Position (in parent)" : "Position", theme::textDim);
    vec3Fields(L, "loc", id, o.position, 0.02f, -1e6f, 1e6f, kAxisColor);
    label(L, "Rotation (degrees)", theme::textDim);
    vec3Fields(L, "rot", id, o.rotation, 0.5f, -1e6f, 1e6f, kAxisColor);
    label(L, "Scale", theme::textDim);
    vec3Fields(L, "scl", id, o.scale, 0.01f, -1e4f, 1e4f, kAxisColor);
    {
        // Unity's Transform context menu: Reset / Copy / Paste (paste goes to every selected object).
        Rect row = L.row(rowH);
        if (ui_.button(uiHash("tf.reset"), cell(row, 0, 3, gap), "Reset")) {
            beginEdit(uiHash("tf.reset", id));
            for (Object& other : scene_.objects)
                if (other.selected || &other == &o) tf::reset(other);
            setStatus("Transform reset");
        }
        if (ui_.button(uiHash("tf.copy"), cell(row, 1, 3, gap), "Copy")) {
            hasClipboard_ = true;
            clipPosition_ = o.position;
            clipRotation_ = o.rotation;
            clipScale_ = o.scale;
            setStatus("Copied transform of " + o.name);
        }
        if (ui_.button(uiHash("tf.paste"), cell(row, 2, 3, gap), "Paste") && hasClipboard_) {
            beginEdit(uiHash("tf.paste", id));
            for (Object& other : scene_.objects)
                if (other.selected || &other == &o) {
                    other.position = clipPosition_;
                    other.rotation = clipRotation_;
                    other.scale = clipScale_;
                }
            setStatus("Pasted transform");
        }
    }

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
                    o.mesh.touchPositions();
                }
            }
        }
    }
}

void Editor::materialProperties(PanelLayout& L, Object& o) {
    const float fs = (float)fontScale_;
    const float gap = 3 * fs, rowH = ui_.rowHeight(), headerH = 11 * fs;
    const uint32_t id = o.id;
    (void)gap;
    (void)headerH;
    (void)fs;
    if (!o.isMesh()) return;
    {
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
        floatRow(L, "Metallic", "mat.metal", id, o.metallic, 0.005f, 0.0f, 1.0f);
        floatRow(L, "Roughness", "mat.rough", id, o.roughness, 0.005f, 0.0f, 1.0f);
        floatRow(L, "Opacity", "mat.opac", id, o.opacity, 0.005f, 0.0f, 1.0f);
        floatRow(L, "Transmit", "mat.trans", id, o.transmission, 0.005f, 0.0f, 1.0f);
        floatRow(L, "IOR", "mat.ior", id, o.ior, 0.002f, 1.0f, 3.0f);
        {
            Rect row = L.row(rowH);
            if (ui_.button(uiHash("mat.glass", id), cell(row, 0, 3, gap), "Glass")) {
                beginEdit(uiHash("mat.preset", id));
                o.transmission = 1.0f, o.roughness = 0.0f, o.metallic = 0.0f, o.ior = 1.5f, o.opacity = 1.0f;
            }
            if (ui_.button(uiHash("mat.metalp", id), cell(row, 1, 3, gap), "Metal")) {
                beginEdit(uiHash("mat.preset", id));
                o.metallic = 1.0f, o.roughness = 0.25f, o.transmission = 0.0f;
            }
            if (ui_.button(uiHash("mat.plastic", id), cell(row, 2, 3, gap), "Matte")) {
                beginEdit(uiHash("mat.preset", id));
                o.metallic = 0.0f, o.roughness = 0.6f, o.transmission = 0.0f, o.ior = 1.45f;
            }
        }
        label(L, "Emission color (RGB)", theme::textDim);
        vec3Fields(L, "emi", id, o.emission, 0.004f, 0.0f, 1.0f, kRgbColor);
        floatRow(L, "Emission", "emi.str", id, o.emissionStrength, 0.02f, 0.0f, 100.0f);

        // --- PBR texture maps ---
        ui_.header(L.row(headerH), "TEXTURES (PBR)");
        for (int t = 0; t < TEX_COUNT; ++t) {
            Rect r = L.row(rowH);
            float lw = std::floor(r.w * 0.3f), bw = 10.0f * fs;
            ui_.textIn({r.x, r.y, lw, r.h}, texSlotLabel(t), theme::textDim, false);
            std::string path = o.textures[t];
            Rect field{r.x + lw, r.y, r.w - lw - 2 * (bw + gap), r.h};
            if (ui_.textField(uiHash("tex.path", id * 16u + t), field, path) && path != o.textures[t]) {
                pushUndo();
                o.textures[t] = path;
                markDirty();
                reportTexture(o.textures[t]);
            }
            if (ui_.button(uiHash("tex.browse", id * 16u + t), {field.x + field.w + gap, r.y, bw, r.h}, "+")) {
                std::string picked = platform::openFileDialog("Choose a texture", true);
                if (!picked.empty()) {
                    pushUndo();
                    o.textures[t] = picked;
                    markDirty();
                    reportTexture(picked);
                }
            }
            if (ui_.button(uiHash("tex.clear", id * 16u + t), {r.x + r.w - bw, r.y, bw, r.h}, "x") &&
                !o.textures[t].empty()) {
                pushUndo();
                o.textures[t].clear();
                markDirty();
            }
            if (!o.textures[t].empty() && !textureCache().error(o.textures[t]).empty())
                label(L, "  (can't load)", theme::error);
        }
        floatRow(L, "Bumpiness", "mat.nstr", id, o.normalStrength, 0.01f, 0.0f, 10.0f);
        {
            label(L, "UV tiling (U V)", theme::textDim);
            Rect row = L.row(rowH);
            float uvx = o.uvScale.x, uvy = o.uvScale.y;
            if (ui_.dragFloat(uiHash("mat.uvx", id), cell(row, 0, 2, gap), uvx, 0.01f, kAxisColor[0], 0.001f, 1000.0f)) {
                beginEdit(uiHash("mat.uvx", id));
                o.uvScale.x = uvx;
            }
            if (ui_.dragFloat(uiHash("mat.uvy", id), cell(row, 1, 2, gap), uvy, 0.01f, kAxisColor[1], 0.001f, 1000.0f)) {
                beginEdit(uiHash("mat.uvy", id));
                o.uvScale.y = uvy;
            }
        }
        {
            Rect row = L.row(rowH);
            float lw = std::floor(row.w * 0.36f);
            ui_.textIn({row.x, row.y, lw, row.h}, "Folder", theme::textDim, false);
            if (ui_.textField(uiHash("tex.folder", id), {row.x + lw, row.y, row.w - lw, row.h}, pbrFolder_))
                loadPbrFolder(o, pbrFolder_);
            row = L.row(rowH);
            if (ui_.button(uiHash("tex.folderbrowse", id), cell(row, 0, 2, gap), "Folder...")) {
                std::string picked = platform::openFileDialog("Choose any image in the texture set", true);
                if (!picked.empty()) {
                    size_t slash = picked.find_last_of("/\\");
                    pbrFolder_ = slash == std::string::npos ? picked : picked.substr(0, slash);
                    loadPbrFolder(o, pbrFolder_);
                }
            }
            if (ui_.button(uiHash("tex.reload", id), cell(row, 1, 2, gap), "Reload")) {
                textureCache().clear();
                setStatus("Textures will be reloaded from disk");
            }
        }
        label(L, "Drop images on the window:", theme::textDim);
        label(L, "slots picked by file name.", theme::textDim);
    }
}

void Editor::dataProperties(PanelLayout& L, Object& o) {
    const float fs = (float)fontScale_;
    const float gap = 3 * fs, rowH = ui_.rowHeight(), headerH = 11 * fs;
    const uint32_t id = o.id;
    (void)gap;
    (void)headerH;
    (void)fs;
    if (o.kind == ObjectKind::Mesh) {
        return;
    } else if (o.kind == ObjectKind::Light) {
        LightSettings& ls = o.light;
        ui_.header(L.row(headerH), "LIGHT");
        Rect row = L.row(rowH);
        static const char* const kTypes[kLightTypeCount] = {"Point", "Sun", "Spot", "Area"};
        for (int t = 0; t < kLightTypeCount; ++t)
            if (ui_.button(uiHash("light.type", (uint32_t)t), cell(row, t, kLightTypeCount, gap), kTypes[t],
                           (int)ls.type == t)) {
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
        if (ls.type == LightType::Area) {
            floatRow(L, "Width", "light.w", id, ls.width, 0.01f, 0.001f, 1000.0f);
            floatRow(L, "Height", "light.h", id, ls.height, 0.01f, 0.001f, 1000.0f);
        }
        {
            Rect r = L.row(rowH);
            if (ui_.button(uiHash("light.usek", id), cell(r, 0, 2, gap), "Temperature", ls.useTemperature)) {
                beginEdit(uiHash("light.usek", id));
                ls.useTemperature = !ls.useTemperature;
            }
            Vec3 k = kelvinToRGB(ls.temperature);
            ui_.rect(cell(r, 1, 2, gap), Color{std::pow(k.x, 1 / 2.2f), std::pow(k.y, 1 / 2.2f), std::pow(k.z, 1 / 2.2f), 1});
        }
        if (ls.useTemperature) {
            floatRow(L, "Kelvin", "light.kelvin", id, ls.temperature, 10.0f, 1000.0f, 40000.0f, true);
            Rect r = L.row(rowH);
            static const float kPresets[4] = {1900, 3200, 5600, 6500};
            static const char* const kNames[4] = {"Candle", "Tungst.", "Day", "D65"};
            for (int p = 0; p < 4; ++p)
                if (ui_.button(uiHash("light.kp", (uint32_t)p), cell(r, p, 4, gap), kNames[p], ls.temperature == kPresets[p])) {
                    beginEdit(uiHash("light.kelvin", id));
                    ls.temperature = kPresets[p];
                }
            label(L, "Tints the color above.", theme::textDim);
        }
        label(L, ls.type == LightType::Point ? "Shines all around." : "Shines along local -Y",
              theme::textDim);
        if (shading_ != SHADE_LIT) label(L, "Lit view shows it.", theme::selection);
    } else if (o.kind == ObjectKind::Camera) {
        PhysicalCamera& c = o.camera;
        ui_.header(L.row(headerH), "CAMERA (PHYSICAL)");
        floatRow(L, "Focal mm", "cam.focal", id, c.focalLength, 0.2f, 4.0f, 2000.0f);
        floatRow(L, "Sensor mm", "cam.sensor", id, c.sensorWidth, 0.1f, 1.0f, 100.0f);
        const float aspect = viewport_.w / std::max(1.0f, viewport_.h);
        label(L, strf("FOV %.1f deg vertical", c.verticalFovDeg(aspect)), theme::textDim);
        ui_.header(L.row(headerH), "EXPOSURE");
        floatRow(L, "Aperture f/", "cam.fstop", id, c.fStop, 0.02f, 0.7f, 64.0f);
        float denom = 1.0f / std::max(1e-6f, c.shutter);
        if (floatRow(L, "Shutter 1/", "cam.shutter", id, denom, 0.5f, 0.001f, 64000.0f)) c.shutter = 1.0f / std::max(1e-6f, denom);
        floatRow(L, "ISO", "cam.iso", id, c.iso, 1.0f, 25.0f, 409600.0f, true);
        floatRow(L, "Comp. EV", "cam.ev", id, c.exposureComp, 0.01f, -10.0f, 10.0f);
        label(L, strf("Exposure x%.2f (EV %+.1f)", c.exposure(), std::log2(std::max(1e-6f, c.exposure()))),
              theme::textDim);
        ui_.header(L.row(headerH), "DEPTH OF FIELD");
        Rect row = L.row(rowH);
        if (ui_.button(uiHash("cam.dof", id), row, "Depth of field", c.depthOfField)) {
            beginEdit(uiHash("cam.dof", id));
            c.depthOfField = !c.depthOfField;
        }
        floatRow(L, "Focus dist.", "cam.focus", id, c.focusDistance, 0.01f, 0.01f, 100000.0f);
        float blades = (float)c.blades;
        if (floatRow(L, "Blades", "cam.blades", id, blades, 0.05f, 0.0f, 12.0f, true)) c.blades = (int)blades;
        label(L, strf("Aperture %.1f mm wide", c.focalLength / std::max(0.5f, c.fStop)), theme::textDim);
        ui_.header(L.row(headerH), "VIEW");
        row = L.row(rowH);
        bool isRender = scene_.renderCameraIndex() == scene_.indexOf(id);
        if (ui_.button(uiHash("cam.render", id), row, isRender ? "Render camera" : "Make render camera", isRender) &&
            !isRender) {
            beginEdit(uiHash("cam.render", id));
            scene_.renderCamera = id;
        }
        row = L.row(rowH);
        if (ui_.button(uiHash("cam.look", id), cell(row, 0, 2, gap), "Look (0)")) lookThroughCamera();
        if (ui_.button(uiHash("cam.align", id), cell(row, 1, 2, gap), "To view")) alignActiveCameraToView();
        label(L, "Looks along local -Z.", theme::textDim);
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
