#include "editor_internal.h"
#include "jobs.h"
#include "profiler.h"
#include "skin.h"
#include "transform.h"

#include <algorithm>
#include <cctype>
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
void Editor::processKeyCapture(const Input& in) {
    if (captureAction_ < 0) return;
    if (!prefsOpen_ || prefsTab_ != 0) {
        captureAction_ = -1;
        return;
    }
    for (int k = 1; k < KEY_COUNT; ++k) {
        if (!in.keyPressed[k] || k == KEY_SHIFT || k == KEY_CONTROL || k == KEY_ALT) continue;
        const input::Action a = (input::Action)captureAction_;
        swallowKeys_ = true;
        if (k == KEY_ESCAPE) {
            setStatus("Key binding unchanged");
        } else if (k == KEY_BACKSPACE) {
            keys_.slots[captureAction_][captureSlot_] = input::Binding();
            setStatus(std::string("Cleared a binding of \"") + input::info(a).label + "\"");
            saveConfig();
        } else {
            input::Binding b;
            b.key = k;
            b.ctrl = in.ctrl();
            b.shift = in.shift();
            b.alt = in.alt();
            if (input::info(a).held) b.ctrl = b.shift = b.alt = false;  // movement keys: Shift = boost
            input::Action lost = keys_.assign(a, captureSlot_, b);
            std::string msg = std::string("\"") + input::info(a).label + "\" = " + input::toString(b);
            if (lost != input::Action::Count) msg += std::string("  (removed from \"") + input::info(lost).label + "\")";
            setStatus(msg);
            saveConfig();
        }
        captureAction_ = -1;
        return;
    }
}

void Editor::buildLeftPanel(const Input& in) {
    processKeyCapture(in);
    const float fs = (float)fontScale_;
    const float pad = 5 * fs, gap = 3 * fs, rowH = ui_.rowHeight();
    ui_.rect(leftPanel_, theme::panel);
    if (ui_.inputEnabled() && leftPanel_.contains(in.mouseX, in.mouseY) && in.wheel != 0.0f)
        leftScroll_ -= in.wheel * rowH * 2;
    leftScroll_ = clampf(leftScroll_, 0.0f, std::max(0.0f, leftContentH_ - leftPanel_.h));
    ui_.pushClip(leftPanel_);
    PanelLayout L{leftPanel_.x + pad, leftPanel_.y + pad - leftScroll_, leftPanel_.w - 2 * pad, gap};
    const float top = L.y;
    if (prefsOpen_) {
        prefsPanel(L, in);
    } else {
        // Stage title, e.g. "2 TEXTURE": where am I in the pipeline.
        Rect title = L.row(rowH);
        std::string t = strf("%d  %s", workspace_ + 1, workspaceName(workspace_));
        for (char& c : t) c = (char)std::toupper((unsigned char)c);
        ui_.text(title.x, title.y + (rowH - ui_.glyphH()) * 0.5f, t, theme::accent);
        switch (workspace_) {
            case WS_MODEL: modelPanel(L); break;
            case WS_TEXTURE: texturePanel(L); break;
            case WS_RIG: rigPanel(L); break;
            case WS_LIGHT: lightPanel(L); break;
            default: renderPanel(L); break;
        }
    }
    leftContentH_ = (L.y - top) + 2 * pad;
    ui_.popClip();
    // Scroll position indicator (the panel scrolls with the wheel / two fingers).
    if (leftContentH_ > leftPanel_.h) {
        float k = leftPanel_.h / leftContentH_;
        float thumb = std::max(10 * fs, leftPanel_.h * k);
        float y = leftPanel_.y + (leftPanel_.h - thumb) * (leftScroll_ / std::max(1.0f, leftContentH_ - leftPanel_.h));
        ui_.rect({leftPanel_.x + leftPanel_.w - 2 * fs - 1, y, 2 * fs, thumb}, withAlpha(theme::textDim, 0.5f));
    }
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
        std::vector<std::vector<int>> children(count);
        std::vector<int> roots;
        for (int j = 0; j < count; ++j) {
            int p = scene_.parentIndex(j);
            if (p >= 0) children[p].push_back(j);
            else roots.push_back(j);
        }
        std::function<void(int, int)> visit = [&](int i, int depth) {
            if (visited[i]) return;
            visited[i] = 1;
            rows.push_back({i, depth});
            for (int j : children[i]) visit(j, depth + 1);
        };
        for (int i : roots) visit(i, 0);
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
        float indent = rows[k].second * 7.0f * fs;
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

    // Sections remember open / closed per workspace, so each pipeline stage
    // shows what matters there (e.g. Texture opens Material, folds Transform).
    auto sec = [&](const char* key, const std::string& title, bool open) {
        std::string k = std::string(key) + char('0' + workspace_);
        return section(L, k.c_str(), title, open);
    };
    const int ws = workspace_;
    if (sec("p.tf", "TRANSFORM", ws != WS_TEXTURE && ws != WS_RENDER)) {
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
    }

    if (o.isMesh()) {
        // --- Parametric recipe ---
        if (o.param.active() && ws == WS_MODEL && sec("p.param", strf("SHAPE: %s", shapeDef(o.param.shape).name), true)) {
            const ShapeDef& def = shapeDef(o.param.shape);
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
        if (ws != WS_RIG && sec("p.mat", "MATERIAL", ws != WS_MODEL)) {
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
        }

        // --- PBR texture maps ---
        if ((ws == WS_TEXTURE || ws == WS_RENDER) && sec("p.tex", "TEXTURE MAPS (PBR)", ws == WS_TEXTURE)) {
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
        if ((ws == WS_MODEL || ws == WS_TEXTURE) && sec("p.mesh", "MESH", true)) {
        Rect row = L.row(rowH);
        if (ui_.button(uiHash("shade.flat"), cell(row, 0, 2, gap), "Flat", !o.smooth) && o.smooth) setSmoothSelected(false);
        if (ui_.button(uiHash("shade.smooth"), cell(row, 1, 2, gap), "Smooth", o.smooth) && !o.smooth)
            setSmoothSelected(true);
        label(L, strf("%d verts, %d faces", (int)o.mesh.verts.size(), (int)o.mesh.faces.size()), theme::textDim);
        label(L, o.mesh.hasUVs() ? "Has UVs" : "No UVs (Texture: unwrap)", theme::textDim);
        }

        // --- Skin ---
        if (!o.skinBones.empty() && ws == WS_RIG && sec("p.skin", strf("SKIN  (%d bones)", (int)o.skinBones.size()), true)) {
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
                Rect row = L.row(rowH);
                if (ui_.button(uiHash("skin.assign"), cell(row, 0, 2, gap), "Assign")) assignWeight(false);
                if (ui_.button(uiHash("skin.remove"), cell(row, 1, 2, gap), "Remove")) assignWeight(true);
            } else {
                label(L, "Edit mode: assign", theme::textDim);
                label(L, "weights to vertices.", theme::textDim);
            }
        }

        // --- Edit-mode selection ---
        if (mode_ == Mode::Edit && &o == activeMesh() && (ws == WS_MODEL || ws == WS_RIG) &&
            sec("p.vsel", "SELECTED VERTICES", true)) {
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
    } else if (opModal_) {
        msg = "Move the mouse or type a value | Enter / click confirms | Esc cancels";
        c = theme::selection;
    } else if (mode_ == Mode::Edit) {
        msg = keymap_ == Keymap::Unity
                  ? "1 2 3 vert/edge/face | Ctrl+E extrude, I inset, Ctrl+B bevel, Ctrl+R loop cut | dbl-click edge: loop | Ctrl+K search"
                  : "1 2 3 vert/edge/face | E extrude, I inset, Ctrl+B bevel, Ctrl+R loop cut, M merge | G/R/S | Ctrl+K search";
        c = theme::textDim;
    } else {
        msg = keymap_ == Keymap::Unity
                  ? "Q W E R tools | Alt+drag orbit (+Shift pan, +Ctrl zoom) | RMB+WASD fly | Ctrl+K search | Shift+/ help"
                  : "Click select | MMB/RMB orbit, Shift pan | Alt+drag works too | G/R/S | Ctrl+K search | Shift+/ help";
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
    if (button("hdr.wire", "Wire", wireframe_)) wireframe_ = !wireframe_;
    if (button("hdr.grid", "Grid", showGrid_)) showGrid_ = !showGrid_;
    if (button("hdr.uv", "UV Map", uvEditor_)) uvEditor_ = !uvEditor_;
    if (button("hdr.play", playing_ ? "Pause" : "Play", playing_)) togglePlay();
    ui_.popClip();
}

void Editor::buildViewportOverlay() {
    const float fs = (float)fontScale_;
    ui_.pushClip(viewport_);
    drawGizmo();
    const float x = toolbarRect_.x + toolbarRect_.w + 6 * fs;
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

    // Scene gizmo (top-right): six axis arms, front-most drawn last.
    {
        Vec2 c;
        float len;
        sceneGizmoLayout(c, len);
        pickSceneGizmo({-1e6f, -1e6f});  // refresh sceneGizmoRect_
        struct Arm {
            int k;
            float x, y, z;
        };
        Arm arms[6];
        for (int k = 0; k < 6; ++k) {
            int i = k / 2;
            float sgn = (k % 2) ? -1.0f : 1.0f;
            arms[k] = {k, view_(0, i) * sgn, -view_(1, i) * sgn, view_(2, i) * sgn};
        }
        std::sort(arms, arms + 6, [](const Arm& a, const Arm& b) { return a.z < b.z; });
        auto disc = [&](Vec2 p, float r, Color col) {
            for (int t = 0; t < 14; ++t) {
                float a0 = 2 * kPi * t / 14, a1 = 2 * kPi * (t + 1) / 14;
                ui_.triangle(p, p + Vec2(std::cos(a0), std::sin(a0)) * r, p + Vec2(std::cos(a1), std::sin(a1)) * r, col);
            }
        };
        disc(c, len + 6 * fs, withAlpha(theme::panelDark, 0.45f));
        const Color hot{1.0f, 0.86f, 0.18f, 1};
        for (const Arm& a : arms) {
            const bool positive = a.k % 2 == 0;
            const int axis = a.k / 2;
            Vec2 e(c.x + a.x * len, c.y + a.y * len);
            Color col = positive ? kAxisColor[axis] : Color{0.62f, 0.64f, 0.70f, 1};
            if (sceneGizmoHover_ == a.k) col = hot;
            if (a.z < -0.3f) col = withAlpha(col, 0.55f);
            ui_.line(c.x, c.y, e.x, e.y, positive ? 2.0f * fs : 1.2f * fs, col);
            disc(e, (positive ? 6.0f : 4.5f) * fs, col);
            if (positive) {
                const char* nm = axis == 0 ? "X" : axis == 1 ? "Y" : "Z";
                ui_.text(e.x - 2.5f * fs, e.y - 3.5f * fs, nm, theme::panelDark);
            }
        }
        ui_.rect({c.x - 3 * fs, c.y - 3 * fs, 6 * fs, 6 * fs}, sceneGizmoHover_ == 6 ? hot : Color{0.9f, 0.9f, 0.92f, 0.9f});
        const std::string label = cam_.ortho ? "Iso" : "Persp";
        ui_.text(c.x - ui_.textWidth(label) * 0.5f, c.y + len + 6 * fs, label,
                 sceneGizmoHover_ == 6 ? hot : withAlpha(theme::text, 0.85f));
    }
    drawNavWidget();
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
    static const char* const kUnity[][2] = {
        {"UNITY KEYMAP", ""},
        {"Q W E R Y", "Hand / Move / Rotate / Scale / All tools"},
        {"Drag a handle", "Arrow: axis, square: plane, ring: rotate"},
        {"  Ctrl while dragging", "Snap (0.25 / 15 deg / 0.1)"},
        {"  Shift + drag (Edit)", "Extrude the selected faces, then move"},
        {"X / Z", "Global-local axes / pivot-center"},
        {"Alt+LMB  MMB  Alt+RMB", "Orbit / pan / zoom"},
        {"RMB + W A S D Q E", "Fly (accelerates; Shift faster)"},
        {"  wheel while flying", "Change fly speed"},
        {"Arrow keys", "Move the camera; wheel zooms"},
        {"Scene gizmo (corner)", "Click an axis: view along it; label: Persp/Iso"},
        {"F / double-click list", "Frame selection (animated)"},
        {"Click / drag", "Select / box (Shift or Ctrl adds)"},
        {"Ctrl+D / Delete", "Duplicate / delete"},
        {"Ctrl+A / Ctrl+E", "Select all / extrude faces"},
        {"Shift+Z", "Cycle shading modes"},
    };
    static const char* const kBlender[][2] = {
        {"BLENDER KEYMAP", ""},
        {"G / R / S", "Move / rotate / scale (X Y Z lock, Ctrl snap)"},
        {"  Left click / Enter", "Confirm     Right click / Esc: cancel"},
        {"Handles", "Toolbar tools work too (drag the gizmo)"},
        {"RMB / MMB drag", "Orbit; Shift: pan; wheel: zoom"},
        {"Click / drag", "Select / box (Shift add, Ctrl remove)"},
        {"A / E / X", "Select all / extrude faces / delete"},
        {"Shift + D", "Duplicate (keeps hierarchy and rigs)"},
        {"Z / W", "Cycle shading / wireframe"},
    };
    static const char* const kCommon[][2] = {
        {"", ""},
        {"BOTH KEYMAPS", ""},
        {"Tab (Edit: 1 2 3 I)", "Edit mode (vertex/edge/face, inset)"},
        {"Ctrl+P / Alt+P", "Parent to active / clear parent"},
        {"U / Space / F", "Smart unwrap / particles / frame"},
        {"1 / 3 / 7  (Ctrl)  5", "Front / right / top (opposite); ortho"},
        {"Ctrl+Z Y / Ctrl+S O", "Undo, redo / save, load (File tab)"},
        {"F3 / F12 / F1 / F5", "Stats / screenshot / help / render"},
        {"0 / Ctrl+Alt+0", "Camera view / camera to this view"},
        {"Drag in the list", "Parent / reorder; Ctrl/Shift multi"},
        {"Keys / Camera tabs", "Rebind keys (defaults shown) / look"},
    };
    std::vector<std::pair<const char*, const char*>> rows;
    if (keymap_ == Keymap::Unity)
        for (const auto& r : kUnity) rows.push_back({r[0], r[1]});
    else
        for (const auto& r : kBlender) rows.push_back({r[0], r[1]});
    for (const auto& r : kCommon) rows.push_back({r[0], r[1]});
    const int n = (int)rows.size();
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
        bool section = rows[i].second[0] == '\0';
        ui_.text(box.x + pad, y, rows[i].first, section ? theme::textDim : theme::white);
        ui_.text(box.x + pad + keyW, y, rows[i].second, theme::text);
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

// Unity-like tool palette on the left edge of the viewport.
void Editor::buildViewportToolbar() {
    const float fs = (float)fontScale_;
    const float gap = 2 * fs, h = ui_.rowHeight();
    const float w = ui_.textWidth("Center") + 10 * fs;
    float x = viewport_.x + 4 * fs, y = headerRect_.y + headerRect_.h + 4 * fs;
    const float top = y;
    struct Entry {
        const char* label;
        Tool tool;
    };
    static const Entry tools[] = {
        {"Hand", Tool::Hand}, {"Move", Tool::Move}, {"Rotate", Tool::Rotate}, {"Scale", Tool::Scale}, {"All", Tool::Universal}};
    for (const Entry& e : tools) {
        if (ui_.button(uiHash("tool", (uint32_t)e.tool), {x, y, w, h}, e.label, tool_ == e.tool)) setTool(e.tool);
        y += h + gap;
    }
    y += 3 * fs;
    if (ui_.button(uiHash("tool.space"), {x, y, w, h}, localSpace_ ? "Local" : "Global", false)) {
        localSpace_ = !localSpace_;
        setStatus(localSpace_ ? "Handle orientation: Local" : "Handle orientation: Global");
    }
    y += h + gap;
    if (ui_.button(uiHash("tool.pivot"), {x, y, w, h}, pivotCenter_ ? "Center" : "Pivot", false)) {
        pivotCenter_ = !pivotCenter_;
        setStatus(pivotCenter_ ? "Handle position: Center" : "Handle position: Pivot");
    }
    y += h + gap + 3 * fs;
    // Path-traced view toggle (F5 by default; settings in the Render tab).
    if (ui_.button(uiHash("tool.render"), {x, y, w, h}, "Render", renderView_)) toggleRenderView();
    y += h;
    toolbarRect_ = {x, top, w, y - top};
}

// F3: rolling per-section timings from the profiler (GEA Vol. I ch. 10).
void Editor::buildStatsOverlay() {
    const float fs = (float)fontScale_;
    std::vector<prof::Stat> st = prof::stats();
    std::sort(st.begin(), st.end(), [](const prof::Stat& a, const prof::Stat& b) { return a.avgMs > b.avgMs; });
    std::vector<std::string> lines;
    for (const auto& x : st)
        if (std::string(x.name) == "frame total")
            lines.push_back(strf("Frame %6.2f ms (%4.0f fps)", x.avgMs, x.avgMs > 0 ? 1000.0 / x.avgMs : 0.0));
    for (const auto& x : st) {
        if (std::string(x.name) == "frame total" || x.avgMs < 0.01) continue;
        lines.push_back(strf("%-22.22s %6.2f", x.name, x.avgMs));
        if (lines.size() > 12) break;
    }
    for (const auto& c : prof::counters())
        if (c.avg > 0) lines.push_back(strf("%-22.22s %6.0f", c.name, c.avg));
    for (const std::string& l : gpuReport()) lines.push_back(l);
    const float lineH = 9 * fs, w = 30 * 6 * fs + 8 * fs, h = lines.size() * lineH + 8 * fs;
    Rect nav[4];
    navWidgetLayout(nav);
    float below = camSet_.navWidget ? nav[3].y + nav[3].h : sceneGizmoRect_.y + sceneGizmoRect_.h;
    Rect box{viewport_.x + viewport_.w - w - 6 * fs, below + 4 * fs, w, h};
    ui_.rect(box, withAlpha(theme::panelDark, 0.88f));
    float y = box.y + 4 * fs;
    for (size_t i = 0; i < lines.size(); ++i, y += lineH)
        ui_.text(box.x + 4 * fs, y, lines[i], i == 0 ? theme::white : theme::text);
}
