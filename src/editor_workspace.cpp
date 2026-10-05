// Workspaces and the chrome around the viewport.
//
// The left panel follows the (simplified) 3D production pipeline, one
// workspace per stage, so each stage only shows its own tools:
//   1 Model    create shapes, edit topology (extrude, inset, bevel, loop cut,
//              bridge, push in ...), booleans, clean-up
//   2 Texture  UV unwrapping, UV editor, materials and texture maps
//   3 Rig      bones, binding, skin weights, hierarchy
//   4 Light    lights, world ambient, cameras, particle effects
//   5 Render   the path tracer (GPU / CPU / RTX) and its settings
// Settings that are not part of the pipeline (keys, navigation, interface
// scale) live in Preferences; file commands in the File menu. Every command
// is also reachable by name from the command search (Ctrl+K), so nothing
// depends on knowing a shortcut, having F-keys or a numpad.
#include "editor_internal.h"
#include "jobs.h"
#include "profiler.h"

#include <algorithm>
#include <cctype>
#include <cmath>

using namespace ed;

// Shared locals for panel builders: sizes, and button / header / note helpers.
#define PANEL_HELPERS                                                                                \
    const float fs = (float)fontScale_;                                                              \
    const float gap = 3 * fs, rowH = ui_.rowHeight(), headerH = 11 * fs;                             \
    (void)gap, (void)rowH, (void)headerH;                                                            \
    auto btn = [&](const char* id, const Rect& r, const char* text, bool toggled = false) {          \
        return ui_.button(uiHash(id), r, text, toggled);                                             \
    };                                                                                               \
    auto header = [&](const char* text) { ui_.header(L.row(headerH), text); };                       \
    auto note = [&](const char* text) { label(L, text, theme::textDim); };                           \
    (void)btn, (void)header, (void)note

const char* Editor::workspaceName(int ws) {
    static const char* const names[WS_COUNT] = {"Model", "Texture", "Rig", "Light", "Render"};
    return ws >= 0 && ws < WS_COUNT ? names[ws] : "";
}

void Editor::setWorkspace(int ws) {
    if (ws < 0 || ws >= WS_COUNT) return;
    // Each workspace remembers its own viewport shading and UV editor state.
    static int shadingOf[WS_COUNT] = {SHADE_STUDIO, SHADE_STUDIO, SHADE_STUDIO, SHADE_LIT, SHADE_LIT};
    static bool uvOf[WS_COUNT] = {false, true, false, false, false};
    if (ws != workspace_) {
        shadingOf[workspace_] = shading_;
        uvOf[workspace_] = uvEditor_;
        shading_ = shadingOf[ws];
        uvEditor_ = uvOf[ws];
    }
    if ((ws == WS_LIGHT || ws == WS_RENDER) && mode_ == Mode::Edit) setMode(Mode::Object);
    workspace_ = ws;
    prefsOpen_ = false;
    leftScroll_ = 0;
    static const char* const hints[WS_COUNT] = {
        "Model: create shapes, Tab for Edit mode (extrude, inset, bevel, loop cut, bridge, push in)",
        "Texture: unwrap UVs, then set the material and texture maps in Properties",
        "Rig: add bones, select the mesh then the root bone, Bind; pose with Rotate",
        "Light: add lights and cameras; the viewport shows Lit shading here",
        "Render: path-trace the scene (Render / F5 / Ctrl+Shift+R)"};
    setStatus(hints[ws]);
}

bool Editor::section(PanelLayout& L, const char* key, const std::string& title, bool defaultOpen) {
    const uint32_t id = uiHash("section", uiHash(key));
    auto it = sectionOpen_.find(id);
    bool open = it == sectionOpen_.end() ? defaultOpen : it->second;
    if (ui_.foldHeader(id, L.row(11.0f * fontScale_), title, open)) {
        open = !open;
        sectionOpen_[id] = open;
    }
    return open;
}

// ============================================================================
// Top bar: pipeline stages on the left, search / file / history / prefs / help
// ============================================================================
void Editor::buildTopBar(const Input& in) {
    (void)in;
    const float fs = (float)fontScale_;
    const float pad = 3 * fs, gap = 3 * fs, h = ui_.rowHeight();
    ui_.rect(topBar_, theme::panelDark);
    ui_.rect({topBar_.x, topBar_.y + topBar_.h - 1, topBar_.w, 1}, theme::border);
    // Labels shrink when the window is narrow so nothing overlaps.
    static const char* const full[WS_COUNT] = {"1 Model", "2 Texture", "3 Rig", "4 Light", "5 Render"};
    static const char* const brief[WS_COUNT] = {"Model", "Tex", "Rig", "Light", "Render"};
    static const char* const tiny[WS_COUNT] = {"1", "2", "3", "4", "5"};
    struct Right {
        const char* id;
        const char* full;
        const char* brief;
    };
    const Right right[] = {{"tb.search", "Search  Ctrl+K", "Search"}, {"tb.file", "File", "File"},
                           {"tb.undo", "Undo", "Undo"},              {"tb.redo", "Redo", "Redo"},
                           {"tb.prefs", "Preferences", "Prefs"},     {"tb.help", "Help", "?"}};
    const int nRight = (int)(sizeof right / sizeof right[0]);
    auto width = [&](const char* t) { return ui_.textWidth(t) + 8 * fs; };
    auto total = [&](const char* const* ws, bool briefRight, bool arrows) {
        float w = 2 * pad;
        for (int i = 0; i < WS_COUNT; ++i) w += width(ws[i]) + gap + (arrows ? ui_.textWidth(">") + gap : 0);
        for (int i = 0; i < nRight; ++i) w += width(briefRight ? right[i].brief : right[i].full) + gap;
        return w;
    };
    const char* const* labels = full;
    bool briefRight = false, arrows = true;
    if (total(full, false, true) > topBar_.w) labels = full, briefRight = true;
    if (total(labels, briefRight, true) > topBar_.w) labels = brief, arrows = false;
    if (total(labels, briefRight, arrows) > topBar_.w) labels = tiny;

    ui_.pushClip(topBar_);
    float x = topBar_.x + pad;
    const float y = topBar_.y + pad;
    for (int i = 0; i < WS_COUNT; ++i) {
        float w = width(labels[i]);
        if (ui_.button(uiHash("tb.ws", (uint32_t)i), {x, y, w, h}, labels[i], workspace_ == i && !prefsOpen_)) setWorkspace(i);
        x += w + gap;
        if (arrows && i + 1 < WS_COUNT) {
            ui_.text(x, y + (h - ui_.glyphH()) * 0.5f, ">", theme::textDim);
            x += ui_.textWidth(">") + gap;
        }
    }
    float rx = topBar_.x + topBar_.w - pad;
    for (int i = nRight - 1; i >= 0; --i) {
        const char* text = briefRight ? right[i].brief : right[i].full;
        float w = width(text);
        rx -= w;
        Rect r{rx, y, w, h};
        rx -= gap;
        if (rx < x) break;  // no room left: the stage buttons win
        const std::string id = right[i].id;
        bool toggled = (id == "tb.file" && fileMenu_) || (id == "tb.prefs" && prefsOpen_) ||
                       (id == "tb.search" && paletteOpen_) || (id == "tb.help" && showHelp_);
        if (id == "tb.file") fileButtonRect_ = r;
        if (!ui_.button(uiHash(right[i].id), r, text, toggled)) continue;
        if (id == "tb.search") openPalette();
        else if (id == "tb.file") fileMenu_ = !fileMenu_;
        else if (id == "tb.undo") undo();
        else if (id == "tb.redo") redo();
        else if (id == "tb.prefs") prefsOpen_ = !prefsOpen_, leftScroll_ = 0;
        else if (id == "tb.help") showHelp_ = true;
    }
    ui_.popClip();
}

// File menu: a drop-down under the File button.
void Editor::buildFileMenu(const Input& in) {
    if (!fileMenu_) {
        fileMenuRect_ = {};
        return;
    }
    const float fs = (float)fontScale_;
    const float pad = 4 * fs, gap = 3 * fs, rowH = ui_.rowHeight();
    const float w = std::min(150.0f * fs, (float)screenW_ - 2 * pad);
    const int rows = 9;
    const float h = 2 * pad + rows * (rowH + gap) + 10 * fs;
    float x = std::min(fileButtonRect_.x, (float)screenW_ - w - pad);
    Rect box{std::max(0.0f, x), topBar_.y + topBar_.h, w, h};
    fileMenuRect_ = box;
    ui_.rect({box.x + 2 * fs, box.y + 2 * fs, box.w, box.h}, {0, 0, 0, 0.35f});
    ui_.rect(box, theme::panel);
    ui_.border(box, theme::border, std::max(1.0f, fs * 0.5f));
    PanelLayout L{box.x + pad, box.y + pad, box.w - 2 * pad, gap};
    label(L, "File name (.m3d / .obj)", theme::textDim);
    ui_.textField(uiHash("file.name"), L.row(rowH), fileField_);
    bool close = false;
    auto item = [&](const char* id, const char* text, const char* key) {
        Rect r = L.row(rowH);
        bool hit = ui_.button(uiHash(id), r, "", false);
        ui_.text(r.x + 4 * fs, r.y + (rowH - ui_.glyphH()) * 0.5f, text, theme::text);
        if (key && *key)
            ui_.text(r.x + r.w - ui_.textWidth(key) - 4 * fs, r.y + (rowH - ui_.glyphH()) * 0.5f, key, theme::textDim);
        if (hit) close = true;
        return hit;
    };
    auto keyOf = [&](input::Action a) { return input::toString(keys_.slots[(int)a][0]); };
    if (item("fm.new", "New scene", "")) newScene();
    if (item("fm.save", "Save", keyOf(input::Action::Save).c_str())) saveFile();
    if (item("fm.open", "Open (the name above)", keyOf(input::Action::Open).c_str())) loadFile();
    if (item("fm.browse", "Open file...", "")) {
        std::string picked = platform::openFileDialog("Open a scene or OBJ model", false);
        if (!picked.empty()) {
            fileField_ = picked;
            openPath(picked);
        }
    }
    if (item("fm.import", "Import OBJ (adds)", "")) importObj();
    if (item("fm.export", "Export OBJ", "")) exportObj();
    if (item("fm.shot", "Screenshot (PNG)", keyOf(input::Action::Screenshot).c_str())) screenshotPending_ = true;
    if (close) fileMenu_ = false;
    // Click anywhere else (or Esc) closes the menu.
    if ((in.mousePressed[MOUSE_LEFT] || in.mousePressed[MOUSE_RIGHT]) && !box.contains(in.mouseX, in.mouseY) &&
        !fileButtonRect_.contains(in.mouseX, in.mouseY))
        fileMenu_ = false;
    if (in.pressed(KEY_ESCAPE)) fileMenu_ = false;
}

// ============================================================================
// Workspace panels
// ============================================================================
void Editor::modelPanel(PanelLayout& L) {
    PANEL_HELPERS;
    Rect row = L.row(rowH);
    if (btn("mode.object", cell(row, 0, 2, gap), "Object", mode_ == Mode::Object)) setMode(Mode::Object);
    if (btn("mode.edit", cell(row, 1, 2, gap), "Edit", mode_ == Mode::Edit)) setMode(Mode::Edit);

    if (mode_ == Mode::Edit) {
        if (section(L, "m.select", "SELECT  (1 2 3)")) {
            row = L.row(rowH);
            if (btn("sel.vert", cell(row, 0, 3, gap), "Vertex", selMode_ == SelMode::Vertex)) setSelMode(SelMode::Vertex);
            if (btn("sel.edge", cell(row, 1, 3, gap), "Edge", selMode_ == SelMode::Edge)) setSelMode(SelMode::Edge);
            if (btn("sel.face", cell(row, 2, 3, gap), "Face", selMode_ == SelMode::Face)) setSelMode(SelMode::Face);
            row = L.row(rowH);
            if (btn("sel.all", cell(row, 0, 2, gap), "All")) selectAll();
            if (btn("sel.linked", cell(row, 1, 2, gap), "Linked")) selectLinked();
            row = L.row(rowH);
            if (btn("sel.loop", cell(row, 0, 2, gap), "Edge loop")) selectLoop(false);
            if (btn("sel.ring", cell(row, 1, 2, gap), "Edge ring")) selectLoop(true);
        }
        if (section(L, "m.faces", "ADD FACES")) {
            row = L.row(rowH);
            if (btn("t.extrude", cell(row, 0, 2, gap), "Extrude")) extrude();
            if (btn("t.inset", cell(row, 1, 2, gap), "Inset")) insetSelected(true);
            row = L.row(rowH);
            if (btn("t.bevel", cell(row, 0, 2, gap), "Bevel")) startMeshOp(OpType::Bevel, true);
            if (btn("t.bridge", cell(row, 1, 2, gap), "Bridge")) startMeshOp(OpType::Bridge, false);
            row = L.row(rowH);
            if (btn("t.push", cell(row, 0, 2, gap), "Push in")) {
                pushDefaults_.through = false;
                startMeshOp(OpType::Push, true);
            }
            if (btn("t.punch", cell(row, 1, 2, gap), "Punch")) {
                pushDefaults_.through = true;
                startMeshOp(OpType::Push, false);
                pushDefaults_.through = false;
            }
            row = L.row(rowH);
            if (btn("t.fill", cell(row, 0, 2, gap), "Fill")) fillSelected();
            if (btn("t.poke", cell(row, 1, 2, gap), "Poke")) startMeshOp(OpType::Poke, false);
        }
        if (section(L, "m.verts", "ADD VERTICES")) {
            row = L.row(rowH);
            if (btn("t.loopcut", cell(row, 0, 2, gap), "Loop cut")) startMeshOp(OpType::LoopCut, false);
            if (btn("t.split", cell(row, 1, 2, gap), "Split")) startMeshOp(OpType::Subdivide, false);
            row = L.row(rowH);
            if (btn("t.connect", cell(row, 0, 2, gap), "Connect")) connectSelected();
            if (btn("t.merge", cell(row, 1, 2, gap), "Merge")) mergeSelected();
            row = L.row(rowH);
            if (btn("t.delete", cell(row, 0, 2, gap), "Delete")) deleteSelected();
            if (btn("t.subdiv", cell(row, 1, 2, gap), "Subdiv")) subdivideSelected();
        }
        lastOpPanel(L);
        if (section(L, "m.editshade", "SHADING", false)) {
            row = L.row(rowH);
            if (btn("mesh.flat", cell(row, 0, 3, gap), "Flat")) setSmoothSelected(false);
            if (btn("mesh.smooth", cell(row, 1, 3, gap), "Smooth")) setSmoothSelected(true);
            if (btn("mesh.flip", cell(row, 2, 3, gap), "Flip")) flipSelected();
        }
        return;
    }

    if (section(L, "m.create", "CREATE")) {
        for (int r = 0; r < 5; ++r) {
            row = L.row(rowH);
            for (int c = 0; c < 2; ++c) {
                int s = r * 2 + c;
                if (ui_.button(uiHash("add", (uint32_t)s), cell(row, c, 2, gap), shapeDef(s).name)) addShape(s);
            }
        }
        row = L.row(rowH);
        if (btn("add.empty", cell(row, 0, 2, gap), "Empty")) addEmpty();
        if (btn("add.camera2", cell(row, 1, 2, gap), "Camera")) addCamera();
    }
    if (section(L, "m.object", "OBJECT")) {
        row = L.row(rowH);
        if (btn("obj.dup", cell(row, 0, 3, gap), "Dupl.")) duplicateSelected();
        if (btn("obj.del", cell(row, 1, 3, gap), "Delete")) deleteSelected();
        if (btn("obj.join", cell(row, 2, 3, gap), "Join")) joinSelected();
        row = L.row(rowH);
        if (btn("obj.parent", cell(row, 0, 2, gap), "Parent")) parentSelected();
        if (btn("obj.unparent", cell(row, 1, 2, gap), "Unparent")) unparentSelected();
        row = L.row(rowH);
        if (btn("mesh.subdiv", cell(row, 0, 2, gap), "Subdivide")) subdivideSelected();
        if (btn("mesh.flip", cell(row, 1, 2, gap), "Flip")) flipSelected();
        row = L.row(rowH);
        if (btn("mesh.flat", cell(row, 0, 2, gap), "Flat")) setSmoothSelected(false);
        if (btn("mesh.smooth", cell(row, 1, 2, gap), "Smooth")) setSmoothSelected(true);
    }
    if (section(L, "m.bool", "BOOLEAN", false)) {
        row = L.row(rowH);
        if (btn("csg.union", cell(row, 0, 3, gap), "Union")) booleanSelected(csg::Op::Union);
        if (btn("csg.diff", cell(row, 1, 3, gap), "Diff")) booleanSelected(csg::Op::Difference);
        if (btn("csg.inter", cell(row, 2, 3, gap), "Inter")) booleanSelected(csg::Op::Intersection);
        row = L.row(rowH);
        if (btn("csg.tris", cell(row, 0, 2, gap), "Tris out", csgTriangulate_)) csgTriangulate_ = !csgTriangulate_;
        if (btn("csg.keep", cell(row, 1, 2, gap), "Keep cut", csgKeepCutters_)) csgKeepCutters_ = !csgKeepCutters_;
        note("Select cutters, then");
        note("the target last.");
    }
    if (section(L, "m.clean", "CLEAN UP", false)) {
        row = L.row(rowH);
        if (btn("ngon.tri", cell(row, 0, 2, gap), "Ngon>Tri")) meshCleanupSelected(0);
        if (btn("ngon.all", cell(row, 1, 2, gap), "All>Tris")) meshCleanupSelected(1);
        row = L.row(rowH);
        if (btn("ngon.clean", cell(row, 0, 2, gap), "Weld/fix")) meshCleanupSelected(2);
        if (btn("ngon.quads", cell(row, 1, 2, gap), "Tri>Quad")) meshCleanupSelected(3);
    }
}

void Editor::texturePanel(PanelLayout& L) {
    PANEL_HELPERS;
    Rect row;
    if (section(L, "t.unwrap", "UNWRAP")) {
        static const uv::Method kMethods[6] = {uv::Method::Smart,  uv::Method::Box,       uv::Method::Planar,
                                               uv::Method::Cylindrical, uv::Method::Spherical, uv::Method::PerFace};
        static const char* const kNames[6] = {"Smart (U)", "Box", "Planar", "Cylinder", "Sphere", "Per face"};
        for (int r = 0; r < 3; ++r) {
            row = L.row(rowH);
            for (int c = 0; c < 2; ++c) {
                int k = r * 2 + c;
                if (ui_.button(uiHash("unwrap", (uint32_t)k), cell(row, c, 2, gap), kNames[k])) unwrapActive(kMethods[k]);
            }
        }
        note("Object mode: whole mesh.");
        note("Edit mode: selected faces.");
    }
    if (section(L, "t.uvtools", "UV LAYOUT")) {
        row = L.row(rowH);
        if (btn("uv.fit", cell(row, 0, 2, gap), "Fit 0-1")) uvTool(0);
        if (btn("uv.pack", cell(row, 1, 2, gap), "Pack")) uvTool(1);
        row = L.row(rowH);
        if (btn("uv.rot", cell(row, 0, 3, gap), "Rot 90")) uvTool(2);
        if (btn("uv.flipu", cell(row, 1, 3, gap), "Flip U")) uvTool(3);
        if (btn("uv.flipv", cell(row, 2, 3, gap), "Flip V")) uvTool(4);
        row = L.row(rowH);
        if (btn("uv.editor", cell(row, 0, 2, gap), "UV editor", uvEditor_)) uvEditor_ = !uvEditor_;
        if (btn("uv.checker", cell(row, 1, 2, gap), "Checker", shading_ == SHADE_CHECKER))
            shading_ = shading_ == SHADE_CHECKER ? SHADE_STUDIO : SHADE_CHECKER;
        note("Drag selected faces in");
        note("the UV editor to move UVs.");
    }
    if (section(L, "t.mat", "MATERIAL PRESETS")) {
        Object* o = activeMesh();
        struct Preset {
            const char* name;
            float metal, rough, trans, ior;
        };
        static const Preset kPresets[6] = {{"Matte", 0, 0.6f, 0, 1.45f}, {"Plastic", 0, 0.3f, 0, 1.45f},
                                           {"Metal", 1, 0.25f, 0, 1.5f},  {"Chrome", 1, 0.03f, 0, 1.5f},
                                           {"Glass", 0, 0, 1, 1.5f},      {"Water", 0, 0, 1, 1.33f}};
        for (int r = 0; r < 3; ++r) {
            row = L.row(rowH);
            for (int c = 0; c < 2; ++c) {
                const Preset& p = kPresets[r * 2 + c];
                if (!ui_.button(uiHash("matp", (uint32_t)(r * 2 + c)), cell(row, c, 2, gap), p.name) || !o) continue;
                pushUndo();
                for (Object& t : scene_.objects)
                    if (t.isMesh() && (t.selected || &t == o)) {
                        t.metallic = p.metal, t.roughness = p.rough, t.transmission = p.trans, t.ior = p.ior;
                        t.opacity = 1.0f;
                    }
                markDirty();
                setStatus(std::string("Material: ") + p.name);
            }
        }
        note(o ? "Colour, maps: Properties ->" : "Select a mesh first.");
    }
}

void Editor::rigPanel(PanelLayout& L) {
    PANEL_HELPERS;
    Rect row;
    if (section(L, "r.bones", "BONES")) {
        row = L.row(rowH);
        if (btn("rig.add", cell(row, 0, 2, gap), "Add bone")) addBone(false);
        if (btn("rig.extrude", cell(row, 1, 2, gap), "Extrude")) addBone(true);
        row = L.row(rowH);
        if (btn("rig.rest", cell(row, 0, 2, gap), "Rest pose")) resetPose();
        if (btn("rig.frame", cell(row, 1, 2, gap), "Frame")) frameSelected();
    }
    if (section(L, "r.skin", "SKIN")) {
        row = L.row(rowH);
        if (btn("rig.bind", cell(row, 0, 2, gap), "Bind")) bindSelected();
        if (btn("rig.unbind", cell(row, 1, 2, gap), "Unbind")) unbindSelected();
        row = L.row(rowH);
        if (btn("rig.auto", cell(row, 0, 2, gap), "Auto wgt")) recomputeWeights();
        if (btn("rig.norm", cell(row, 1, 2, gap), "Normalize")) normalizeWeights();
        row = L.row(rowH);
        if (btn("rig.weights", row, "Weights view", shading_ == SHADE_WEIGHTS))
            shading_ = shading_ == SHADE_WEIGHTS ? SHADE_STUDIO : SHADE_WEIGHTS;
    }
    if (section(L, "r.hier", "HIERARCHY")) {
        row = L.row(rowH);
        if (btn("rig.parent", cell(row, 0, 2, gap), "Parent")) parentSelected();
        if (btn("rig.unparent", cell(row, 1, 2, gap), "Unparent")) unparentSelected();
        note("Or drag rows in the list.");
    }
    if (section(L, "r.howto", "HOW TO RIG", false)) {
        note("1 Add bone; Extrude grows");
        note("  a chain.");
        note("2 Select the mesh, Shift+");
        note("  click the root bone, Bind.");
        note("3 Pose: Rotate tool (E).");
        note("4 Weights: Edit mode,");
        note("  Properties > Skin.");
    }
}

void Editor::lightPanel(PanelLayout& L) {
    PANEL_HELPERS;
    Rect row;
    if (section(L, "l.add", "ADD LIGHT")) {
        row = L.row(rowH);
        if (btn("add.point", cell(row, 0, 2, gap), "Point")) addLight(LightType::Point);
        if (btn("add.sun", cell(row, 1, 2, gap), "Sun")) addLight(LightType::Sun);
        row = L.row(rowH);
        if (btn("add.spot", cell(row, 0, 2, gap), "Spot")) addLight(LightType::Spot);
        if (btn("add.area", cell(row, 1, 2, gap), "Area")) addLight(LightType::Area);
        row = L.row(rowH);
        if (btn("fx.lit", row, "Lit view", shading_ == SHADE_LIT)) shading_ = shading_ == SHADE_LIT ? SHADE_STUDIO : SHADE_LIT;
    }
    if (section(L, "l.world", "WORLD")) {
        label(L, "Ambient light (R G B)", theme::textDim);
        vec3Fields(L, "ambient", 0, scene_.ambient, 0.003f, 0.0f, 1.0f, kRgbColor);
        note("Emission: a mesh's");
        note("material (Properties).");
    }
    if (section(L, "l.camera", "CAMERA")) {
        row = L.row(rowH);
        if (btn("add.camera", cell(row, 0, 2, gap), "Add")) addCamera();
        if (btn("cam.look0", cell(row, 1, 2, gap), "Look thru")) lookThroughCamera();
        row = L.row(rowH);
        if (btn("cam.align0", row, "Camera to this view")) alignActiveCameraToView();
    }
    if (section(L, "l.fx", "EFFECTS (PARTICLES)", false)) {
        row = L.row(rowH);
        if (btn("fx.emitter", cell(row, 0, 2, gap), "Emitter")) addEmitter();
        if (btn("fx.play", cell(row, 1, 2, gap), playing_ ? "Pause" : "Play", playing_)) togglePlay();
        row = L.row(rowH);
        if (btn("fx.restart", row, "Restart")) restartParticles();
    }
}

void Editor::renderPanel(PanelLayout& L) {
    PANEL_HELPERS;
    header("PATH TRACER  (F5)");
    Rect row = L.row(rowH);
    if (btn("rt.gpu", cell(row, 0, 3, gap), "GPU", renderSet_.device == DEV_GPU)) {
        if (gpuTracerOk_) renderSet_.device = DEV_GPU;
        else setStatus("GPU tracer unavailable: " + gpuTracerError_, true);
    }
    if (btn("rt.cpu", cell(row, 1, 3, gap), "CPU", renderSet_.device == DEV_CPU)) renderSet_.device = DEV_CPU;
    if (btn("rt.rtx", cell(row, 2, 3, gap), "RTX", renderSet_.device == DEV_RTX)) {
        renderSet_.device = DEV_RTX;
        if (!hwrtInfo_.available && !renderSet_.allowWarp)
            setStatus("No ray-tracing GPU (DXR 1.1) found. Enable 'Software DXR' to test the RTX path on WARP.", true);
    }
    label(L, hwrtInfo_.available ? "RTX: " + hwrtInfo_.adapter : std::string("RTX: no DXR 1.1 GPU"),
          hwrtInfo_.available && !hwrtInfo_.software ? theme::accent : theme::textDim);
    row = L.row(rowH);
    if (btn("rt.render", cell(row, 0, 2, gap), renderView_ ? "Close" : "Render", renderView_)) toggleRenderView();
    if (btn("rt.save", cell(row, 1, 2, gap), "Save PNG")) {
        std::string base = fileField_;
        size_t dot = base.find_last_of('.');
        size_t slash = base.find_last_of("/\\");
        if (dot != std::string::npos && (slash == std::string::npos || slash < dot)) base = base.substr(0, dot);
        saveRender(base + "_render.png");
    }
    if (renderView_) {
        const int spp = renderSamples();
        const double secs = renderSeconds();
        const double rate = renderRate();
        Rect bar = L.row(rowH * 0.6f);
        ui_.rect(bar, theme::field);
        float k = clampf((float)spp / std::max(1.0f, renderSet_.samples), 0.0f, 1.0f);
        ui_.rect({bar.x, bar.y, bar.w * k, bar.h}, theme::accent);
        label(L, strf("%d / %d spp  %.1fs", spp, (int)renderSet_.samples, secs),
              renderRunning() ? theme::text : theme::white);
        label(L, strf("%.2f Msamples/s", rate * 1e-6), theme::textDim);
        if (rtScene_)
            label(L, strf("%d tris, BVH %.0f ms", (int)rtScene_->triangleCount(), rtScene_->buildMs),
                  theme::textDim);
    }
    row = L.row(rowH);
    const bool hasCam = scene_.renderCameraIndex() >= 0;
    if (btn("rt.fromcam", cell(row, 0, 2, gap), "Camera", renderSet_.useCamera && hasCam)) {
        if (hasCam) renderSet_.useCamera = true;
        else setStatus("No camera object yet: Create > Camera adds one at the current view", true);
    }
    if (btn("rt.fromview", cell(row, 1, 2, gap), "Viewport", !renderSet_.useCamera || !hasCam))
        renderSet_.useCamera = false;
    if (renderSet_.useCamera && hasCam) {
        const Object& c = scene_.objects[scene_.renderCameraIndex()];
        label(L, strf("%s: f/%.1g %.0fmm", c.name.c_str(), c.camera.fStop, c.camera.focalLength), theme::textDim);
        label(L, strf("ISO %.0f 1/%.0fs x%.2f", c.camera.iso, 1.0f / std::max(1e-6f, c.camera.shutter),
                      c.camera.exposure()),
              theme::textDim);
    }
    header("SAMPLING");
    auto setRow = [&](const char* text, const char* key, float& v, float speed, float lo, float hi,
                      bool integer) {
        Rect r = L.row(rowH);
        float lw = std::floor(r.w * 0.52f);
        ui_.textIn({r.x, r.y, lw, r.h}, text, theme::textDim, false);
        float value = v;
        if (ui_.dragFloat(uiHash(key), {r.x + lw, r.y, r.w - lw, r.h}, value, speed, theme::accent, lo, hi)) {
            v = integer ? std::round(value) : value;
            return true;
        }
        return false;
    };
    setRow("Samples", "rt.spp", renderSet_.samples, 1.0f, 1.0f, 65536.0f, true);
    row = L.row(rowH);
    static const int kSpp[4] = {16, 64, 256, 1024};
    for (int c = 0; c < 4; ++c)
        if (ui_.button(uiHash("rt.sppq", (uint32_t)c), cell(row, c, 4, gap), std::to_string(kSpp[c]),
                       (int)renderSet_.samples == kSpp[c]))
            renderSet_.samples = (float)kSpp[c];
    setRow("Bounces", "rt.bounce", renderSet_.bounces, 0.05f, 0.0f, 32.0f, true);
    setRow("Res. %", "rt.res", renderSet_.resolution, 0.5f, 5.0f, 200.0f, true);
    setRow("Clamp", "rt.clamp", renderSet_.clamp, 0.05f, 0.0f, 1000.0f, false);
    setRow("Sky", "rt.env", renderSet_.envStrength, 0.01f, 0.0f, 50.0f, false);
    setRow("Soft shad.", "rt.lsize", renderSet_.lightSize, 0.002f, 0.0f, 2.0f, false);
    row = L.row(rowH);
    if (btn("rt.studio", cell(row, 0, 2, gap), "Studio", renderSet_.studioLights))
        renderSet_.studioLights = !renderSet_.studioLights;
    if (btn("rt.auto", cell(row, 1, 2, gap), "Live", renderAutoUpdate_)) renderAutoUpdate_ = !renderAutoUpdate_;
    header("DEVICES");
    setRow("GPU ms", "rt.budget", renderSet_.gpuBudgetMs, 0.1f, 1.0f, 200.0f, false);
    setRow("Threads", "rt.threads", renderSet_.cpuThreads, 0.05f, 0.0f, 256.0f, true);
    note(renderSet_.cpuThreads < 1 ? strf("(0 = all %d threads)", jobs::hardwareThreads()).c_str()
                                   : strf("of %d hw threads", jobs::hardwareThreads()).c_str());
    row = L.row(rowH);
    if (btn("rt.warp", row, "Software DXR", renderSet_.allowWarp)) {
        renderSet_.allowWarp = !renderSet_.allowWarp;
        hwrtInfo_ = HwRayTracer::probe(renderSet_.allowWarp);
    }
    for (const std::string& ad : hwrtInfo_.adapters) label(L, ad, theme::textDim);
    for (const std::string& line : gpuReport()) label(L, line, softwareGl_ ? theme::error : theme::textDim);
    note("F3 = live GPU/CPU stats.");
}

// ============================================================================
// Preferences (keys, navigation, interface)
// ============================================================================
void Editor::prefsPanel(PanelLayout& L, const Input& in) {
    (void)in;
    PANEL_HELPERS;
    Rect row = L.row(rowH);
    ui_.text(row.x, row.y + (rowH - ui_.glyphH()) * 0.5f, "PREFERENCES", theme::accent);
    float dw = ui_.textWidth("Done") + 10 * fs;
    if (ui_.button(uiHash("prefs.done"), {row.x + row.w - dw, row.y, dw, rowH}, "Done", true)) {
        prefsOpen_ = false;
        leftScroll_ = 0;
        return;
    }
    row = L.row(rowH);
    static const char* const tabs[3] = {"Keys", "Navigate", "Interface"};
    for (int t = 0; t < 3; ++t)
        if (ui_.button(uiHash("prefs.tab", (uint32_t)t), cell(row, t, 3, gap), tabs[t], prefsTab_ == t)) {
            prefsTab_ = t;
            leftScroll_ = 0;
        }
    L.y += gap;
    if (prefsTab_ == 0) {
        header("PRESET");
        Rect row = L.row(rowH);
        if (btn("keys.unity", cell(row, 0, 2, gap), "Unity", keymap_ == Keymap::Unity)) setKeymap(Keymap::Unity);
        if (btn("keys.blender", cell(row, 1, 2, gap), "Blender", keymap_ == Keymap::Blender))
            setKeymap(Keymap::Blender);
        note("Click a slot, press keys.");
        note("Esc cancel, Bksp clear.");
        {
            Rect fr = L.row(rowH);
            float lw = std::floor(fr.w * 0.3f);
            ui_.textIn({fr.x, fr.y, lw, fr.h}, "Find", theme::textDim, false);
            ui_.textField(uiHash("keys.filter"), {fr.x + lw, fr.y, fr.w - lw, fr.h}, keysFilter_);
        }
        const char* const kContextNames[4] = {"SHORTCUTS", "FLY (RMB HELD)", "TRANSFORM (MODAL)", "EDIT MODE"};
        int lastContext = -1;
        std::string filter = keysFilter_;
        for (char& ch : filter) ch = (char)std::tolower((unsigned char)ch);
        for (int a = 0; a < input::kActionCount; ++a) {
            const input::ActionInfo& info = input::info((input::Action)a);
            if (!filter.empty()) {
                std::string lbl = info.label;
                for (char& ch : lbl) ch = (char)std::tolower((unsigned char)ch);
                if (lbl.find(filter) == std::string::npos) continue;
            }
            if ((int)info.context != lastContext) {
                lastContext = (int)info.context;
                header(kContextNames[lastContext]);
            }
            Rect r = L.row(rowH);
            float lw = std::floor(r.w * 0.47f), sw = std::floor((r.w - lw - gap) * 0.5f);
            ui_.textIn({r.x, r.y, lw, r.h}, ui_.fitText(info.label, lw), theme::textDim, false);
            for (int slot = 0; slot < 2; ++slot) {
                Rect sr{r.x + lw + slot * (sw + gap), r.y, slot == 0 ? sw : r.w - lw - sw - gap, r.h};
                const bool capturing = captureAction_ == a && captureSlot_ == slot;
                std::string text = capturing ? "press..." : input::toString(keys_.slots[a][slot]);
                if (ui_.button(uiHash("keys.slot", (uint32_t)(a * 2 + slot)), sr, ui_.fitText(text, sr.w - 2),
                               capturing)) {
                    if (capturing) {
                        captureAction_ = -1;
                    } else {
                        captureAction_ = a;
                        captureSlot_ = slot;
                        setStatus(std::string("Press a key (with Ctrl/Shift/Alt) for \"") + info.label +
                                  "\" - Esc cancels, Backspace clears");
                    }
                }
            }
        }
        header("RESET");
        row = L.row(rowH);
        if (btn("keys.reset", row, "Reset to preset")) setKeymap(keymap_);
        return;
    }
    if (prefsTab_ == 1) {
        auto setRow = [&](const char* text, const char* key, float& v, float speed, float lo, float hi) {
            Rect r = L.row(rowH);
            float lw = std::floor(r.w * 0.52f);
            ui_.textIn({r.x, r.y, lw, r.h}, text, theme::textDim, false);
            float value = v;
            if (ui_.dragFloat(uiHash(key), {r.x + lw, r.y, r.w - lw, r.h}, value, speed, theme::accent, lo, hi)) {
                v = value;
                configDirty_ = true;
            }
        };
        CameraSettings& c = camSet_;
        header("LOOK AROUND");
        setRow("Orbit", "camset.orbit", c.orbitSensitivity, 0.002f, 0.01f, 5.0f);
        setRow("Mouse look", "camset.look", c.lookSensitivity, 0.002f, 0.01f, 5.0f);
        Rect row = L.row(rowH);
        if (btn("camset.invx", cell(row, 0, 2, gap), "Invert X", c.invertX)) c.invertX = !c.invertX, configDirty_ = true;
        if (btn("camset.invy", cell(row, 1, 2, gap), "Invert Y", c.invertY)) c.invertY = !c.invertY, configDirty_ = true;
        header("MOVE");
        setRow("Pan speed", "camset.pan", c.panSpeed, 0.01f, 0.05f, 20.0f);
        setRow("Zoom speed", "camset.zoom", c.zoomSpeed, 0.01f, 0.05f, 20.0f);
        setRow("Fly speed", "camset.fly", flySpeed_, 0.01f, 0.01f, 100.0f);
        setRow("Arrow keys", "camset.arrow", c.arrowSpeed, 0.01f, 0.05f, 20.0f);
        setRow("Shift x", "camset.fast", c.fastMultiplier, 0.02f, 1.0f, 20.0f);
        row = L.row(rowH);
        if (btn("camset.accel", row, "Fly acceleration", c.flyAcceleration))
            c.flyAcceleration = !c.flyAcceleration, configDirty_ = true;
        header("VIEW");
        setRow("FOV", "camset.fov", cam_.fovY, 0.2f, 10.0f, 120.0f);
        setRow("Transition", "camset.trans", c.transition, 0.005f, 0.0f, 3.0f);
        row = L.row(rowH);
        if (btn("camset.reset", row, "Reset camera")) {
            c = CameraSettings();
            flySpeed_ = 1.0f;
            cam_.fovY = 50.0f;
            configDirty_ = true;
            setStatus("Camera settings reset");
        }
        note("Saved to Modeler3D.cfg");
        header("TRACKPAD / NO MOUSE");
        row = L.row(rowH);
        if (btn("camset.trackpad", row, camSet_.trackpad ? "Trackpad mode: on" : "Trackpad mode: off", camSet_.trackpad)) {
            camSet_.trackpad = !camSet_.trackpad;
            configDirty_ = true;
        }
        note("On: 2-finger scroll orbits,");
        note("Shift+scroll pans, pinch or");
        note("Ctrl+scroll zooms.");
        row = L.row(rowH);
        if (btn("camset.navw", row, "On-screen nav buttons", camSet_.navWidget)) {
            camSet_.navWidget = !camSet_.navWidget;
            configDirty_ = true;
        }
        note("Alt+drag orbit, Alt+Shift+");
        note("drag pan, Alt+Ctrl+drag zoom");
        note("(no middle button needed).");
        note("Keys: arrows move, Alt+");
        note("arrows orbit, = / - zoom.");
        return;
    }
    header("UI SCALE");
    row = L.row(rowH);
    static const char* const scales[5] = {"Auto", "1x", "2x", "3x", "4x"};
    for (int s = 0; s < 5; ++s)
        if (ui_.button(uiHash("prefs.scale", (uint32_t)s), cell(row, s, 5, gap), scales[s], uiScale_ == s)) {
            uiScale_ = s;
            configDirty_ = true;
        }
    note("Smaller = more room on");
    note("small screens.");
    header("SHORTCUT-FREE USE");
    note("Every command is in the");
    note("search (top bar or Ctrl+K)");
    note("and the workspace panels.");
    note("F-keys and numpad keys");
    note("all have alternatives:");
    note("Help Shift+/, Stats Ctrl+");
    note("Shift+I, Render Ctrl+Shift");
    note("+R, Shot Ctrl+Shift+P.");
}

// ============================================================================
// On-screen navigation buttons (for trackpads / pens: drag to orbit, pan, zoom)
// ============================================================================
void Editor::navWidgetLayout(Rect out[4]) const {
    const float fs = (float)fontScale_;
    Vec2 c;
    float len;
    sceneGizmoLayout(c, len);
    const float w = ui_.textWidth("Orbit") + 8 * fs, h = ui_.rowHeight(), gap = 2 * fs;
    float x = c.x - w * 0.5f, y = c.y + len + 18 * fs;
    for (int i = 0; i < 4; ++i) out[i] = {x, y + i * (h + gap), w, h};
}

int Editor::navWidgetPick(Vec2 p) const {
    if (!camSet_.navWidget) return -1;
    Rect r[4];
    navWidgetLayout(r);
    for (int i = 0; i < 4; ++i)
        if (r[i].contains(p.x, p.y)) return i;
    return -1;
}

void Editor::drawNavWidget() {
    if (!camSet_.navWidget) return;
    Rect r[4];
    navWidgetLayout(r);
    static const char* const names[4] = {"Orbit", "Pan", "Zoom", "Fit"};
    static const Nav kinds[3] = {Nav::Orbit, Nav::Pan, Nav::Zoom};
    for (int i = 0; i < 4; ++i) {
        bool active = i < 3 && nav_ == kinds[i] && navButton_ == MOUSE_LEFT && navFromWidget_;
        Color bg = active ? theme::accent : navHover_ == i ? theme::buttonHover : withAlpha(theme::panelDark, 0.75f);
        ui_.rect(r[i], bg);
        ui_.textIn(r[i], names[i], active ? theme::white : theme::text, true);
    }
}

// ============================================================================
// Command search ("palette"): type part of a name, Enter runs it
// ============================================================================
void Editor::buildCommands() {
    using A = input::Action;
    const A none = A::Count;
    auto add = [&](const char* name, int ws, A key, std::function<void()> fn) {
        commands_.push_back({name, ws, key, std::move(fn)});
    };
    // Pipeline stages and chrome
    for (int w = 0; w < WS_COUNT; ++w)
        add((std::string("Workspace: ") + workspaceName(w)).c_str(), -1, (A)((int)A::WorkspaceModel + w), [this, w] { setWorkspace(w); });
    add("Preferences: keys", -1, none, [this] { prefsOpen_ = true, prefsTab_ = 0; });
    add("Preferences: navigation / trackpad", -1, none, [this] { prefsOpen_ = true, prefsTab_ = 1; });
    add("Preferences: UI scale", -1, none, [this] { prefsOpen_ = true, prefsTab_ = 2; });
    add("Toggle trackpad navigation", -1, none, [this] {
        camSet_.trackpad = !camSet_.trackpad;
        configDirty_ = true;
        setStatus(camSet_.trackpad ? "Trackpad navigation on" : "Trackpad navigation off");
    });
    add("Help: controls", -1, A::Help, [this] { showHelp_ = true; });
    add("Stats overlay", -1, A::Stats, [this] { showStats_ = !showStats_; });
    add("Screenshot", -1, A::Screenshot, [this] { screenshotPending_ = true; });
    add("Undo", -1, A::Undo, [this] { undo(); });
    add("Redo", -1, A::Redo, [this] { redo(); });
    // File
    add("File: new scene", -1, none, [this] { newScene(); });
    add("File: save", -1, A::Save, [this] { saveFile(); });
    add("File: open", -1, A::Open, [this] { loadFile(); });
    add("File: import OBJ", -1, none, [this] { importObj(); });
    add("File: export OBJ", -1, none, [this] { exportObj(); });
    // Model
    for (int s = 0; s < 10; ++s) add((std::string("Add ") + shapeDef(s).name).c_str(), WS_MODEL, none, [this, s] { addShape(s); });
    add("Add empty", WS_MODEL, none, [this] { addEmpty(); });
    add("Object mode / Edit mode", WS_MODEL, A::ToggleEditMode, [this] { setMode(mode_ == Mode::Object ? Mode::Edit : Mode::Object); });
    add("Select all / none", -1, A::SelectAll, [this] { selectAll(); });
    add("Duplicate", WS_MODEL, A::Duplicate, [this] { duplicateSelected(); });
    add("Delete", WS_MODEL, A::Delete, [this] { deleteSelected(); });
    add("Join objects", WS_MODEL, A::JoinObjects, [this] { joinSelected(); });
    add("Parent to active", -1, A::Parent, [this] { parentSelected(); });
    add("Clear parent", -1, A::Unparent, [this] { unparentSelected(); });
    add("Subdivide (Catmull-Clark)", WS_MODEL, none, [this] { subdivideSelected(); });
    add("Flip normals", WS_MODEL, none, [this] { flipSelected(); });
    add("Shade flat", WS_MODEL, none, [this] { setSmoothSelected(false); });
    add("Shade smooth", WS_MODEL, none, [this] { setSmoothSelected(true); });
    add("Boolean union", WS_MODEL, none, [this] { booleanSelected(csg::Op::Union); });
    add("Boolean difference", WS_MODEL, none, [this] { booleanSelected(csg::Op::Difference); });
    add("Boolean intersection", WS_MODEL, none, [this] { booleanSelected(csg::Op::Intersection); });
    add("Triangulate n-gons", WS_MODEL, none, [this] { meshCleanupSelected(0); });
    add("Triangulate all", WS_MODEL, none, [this] { meshCleanupSelected(1); });
    add("Clean up (weld, T-junctions)", WS_MODEL, none, [this] { meshCleanupSelected(2); });
    add("Triangles to quads", WS_MODEL, none, [this] { meshCleanupSelected(3); });
    add("Vertex select", WS_MODEL, A::SelectVertices, [this] { setSelMode(SelMode::Vertex); });
    add("Edge select", WS_MODEL, A::SelectEdges, [this] { setSelMode(SelMode::Edge); });
    add("Face select", WS_MODEL, A::SelectFaces, [this] { setSelMode(SelMode::Face); });
    add("Select edge loop", WS_MODEL, A::SelectLoop, [this] { selectLoop(false); });
    add("Select edge ring", WS_MODEL, A::SelectRing, [this] { selectLoop(true); });
    add("Select linked", WS_MODEL, A::SelectLinked, [this] { selectLinked(); });
    add("Extrude", WS_MODEL, A::Extrude, [this] { extrude(); });
    add("Inset faces", WS_MODEL, A::Inset, [this] { insetSelected(true); });
    add("Bevel edges / vertices", WS_MODEL, A::Bevel, [this] { startMeshOp(OpType::Bevel, true); });
    add("Loop cut", WS_MODEL, A::LoopCut, [this] { startMeshOp(OpType::LoopCut, false); });
    add("Split (subdivide) edges", WS_MODEL, none, [this] { startMeshOp(OpType::Subdivide, false); });
    add("Connect vertices", WS_MODEL, A::Connect, [this] { connectSelected(); });
    add("Poke faces", WS_MODEL, none, [this] { startMeshOp(OpType::Poke, false); });
    add("Bridge edge loops / faces", WS_MODEL, none, [this] { startMeshOp(OpType::Bridge, false); });
    add("Fill (make face)", WS_MODEL, A::Fill, [this] { fillSelected(); });
    add("Push in (inverse extrude)", WS_MODEL, A::PushIn, [this] {
        pushDefaults_.through = false;
        startMeshOp(OpType::Push, true);
    });
    add("Punch hole through", WS_MODEL, none, [this] {
        pushDefaults_.through = true;
        startMeshOp(OpType::Push, false);
        pushDefaults_.through = false;
    });
    add("Merge vertices at centre", WS_MODEL, A::Merge, [this] { mergeSelected(); });
    // Texture
    add("Smart UV unwrap", WS_TEXTURE, A::SmartUnwrap, [this] { unwrapActive(uv::Method::Smart); });
    add("Box UV unwrap", WS_TEXTURE, none, [this] { unwrapActive(uv::Method::Box); });
    add("Planar UV unwrap", WS_TEXTURE, none, [this] { unwrapActive(uv::Method::Planar); });
    add("Cylindrical UV unwrap", WS_TEXTURE, none, [this] { unwrapActive(uv::Method::Cylindrical); });
    add("Spherical UV unwrap", WS_TEXTURE, none, [this] { unwrapActive(uv::Method::Spherical); });
    add("Pack UV islands", WS_TEXTURE, none, [this] { uvTool(1); });
    add("Fit UVs to 0-1", WS_TEXTURE, none, [this] { uvTool(0); });
    add("UV editor", WS_TEXTURE, none, [this] { uvEditor_ = !uvEditor_; });
    // Rig
    add("Add bone", WS_RIG, none, [this] { addBone(false); });
    add("Extrude bone (child)", WS_RIG, none, [this] { addBone(true); });
    add("Bind skin to bones", WS_RIG, none, [this] { bindSelected(); });
    add("Unbind skin", WS_RIG, none, [this] { unbindSelected(); });
    add("Rest pose", WS_RIG, none, [this] { resetPose(); });
    add("Automatic weights", WS_RIG, none, [this] { recomputeWeights(); });
    add("Normalize weights", WS_RIG, none, [this] { normalizeWeights(); });
    add("Weights view", WS_RIG, none, [this] { shading_ = shading_ == SHADE_WEIGHTS ? SHADE_STUDIO : SHADE_WEIGHTS; });
    // Light
    add("Add point light", WS_LIGHT, none, [this] { addLight(LightType::Point); });
    add("Add sun light", WS_LIGHT, none, [this] { addLight(LightType::Sun); });
    add("Add spot light", WS_LIGHT, none, [this] { addLight(LightType::Spot); });
    add("Add area light", WS_LIGHT, none, [this] { addLight(LightType::Area); });
    add("Add camera", WS_LIGHT, none, [this] { addCamera(); });
    add("Look through camera", WS_LIGHT, A::LookThroughCamera, [this] { lookThroughCamera(); });
    add("Camera to this view", WS_LIGHT, A::AlignCameraToView, [this] { alignActiveCameraToView(); });
    add("Add particle emitter", WS_LIGHT, none, [this] { addEmitter(); });
    add("Play / pause particles", WS_LIGHT, A::PlayPause, [this] { togglePlay(); });
    // Render
    add("Render (path tracer)", WS_RENDER, A::Render, [this] { toggleRenderView(); });
    // View
    add("Frame selection", -1, A::FrameSelected, [this] { frameSelected(); });
    add("View front", -1, A::ViewFront, [this] { setView(0, 0); });
    add("View back", -1, A::ViewBack, [this] { setView(180, 0); });
    add("View right", -1, A::ViewRight, [this] { setView(90, 0); });
    add("View left", -1, A::ViewLeft, [this] { setView(-90, 0); });
    add("View top", -1, A::ViewTop, [this] { setView(0, 89.9f); });
    add("View bottom", -1, A::ViewBottom, [this] { setView(0, -89.9f); });
    add("Perspective / orthographic", -1, A::ToggleOrtho, [this] { cam_.ortho = !cam_.ortho; });
    add("Wireframe (x-ray)", -1, A::ToggleWireframe, [this] { wireframe_ = !wireframe_; });
    add("Grid", -1, none, [this] { showGrid_ = !showGrid_; });
    add("Cycle shading", -1, A::CycleShading, [this] { shading_ = (shading_ + 1) % SHADE_COUNT; });
    add("Move tool", -1, A::ToolMove, [this] { setTool(Tool::Move); });
    add("Rotate tool", -1, A::ToolRotate, [this] { setTool(Tool::Rotate); });
    add("Scale tool", -1, A::ToolScale, [this] { setTool(Tool::Scale); });
    add("Hand (view) tool", -1, A::ToolHand, [this] { setTool(Tool::Hand); });
    add("Grab (move with the mouse / typed value)", -1, A::ModalGrab, [this] { beginTransform(Xform::Grab, true); });
    add("Rotate (modal, typed degrees)", -1, A::ModalRotate, [this] { beginTransform(Xform::Rotate, true); });
    add("Scale (modal, typed factor)", -1, A::ModalScale, [this] { beginTransform(Xform::Scale, true); });
}

void Editor::openPalette() {
    if (xf_ != Xform::None || opModal_) return;
    if (commands_.empty()) buildCommands();
    paletteOpen_ = true;
    paletteQuery_.clear();
    paletteSel_ = 0;
    fileMenu_ = false;
    ui_.cancelEdit();
}

std::vector<int> Editor::paletteMatches() const {
    // Every word of the query must appear in the name (case-insensitive);
    // commands of the current workspace come first.
    std::vector<std::string> words;
    std::string w;
    for (char c : paletteQuery_ + " ") {
        if (c == ' ') {
            if (!w.empty()) words.push_back(w);
            w.clear();
        } else {
            w += (char)std::tolower((unsigned char)c);
        }
    }
    std::vector<int> out, later;
    for (int i = 0; i < (int)commands_.size(); ++i) {
        std::string name = commands_[i].name;
        for (char& c : name) c = (char)std::tolower((unsigned char)c);
        bool all = true;
        for (const std::string& q : words) all = all && name.find(q) != std::string::npos;
        if (!all) continue;
        (commands_[i].workspace == workspace_ ? out : later).push_back(i);
    }
    out.insert(out.end(), later.begin(), later.end());
    return out;
}

void Editor::runCommand(int index) {
    if (index < 0 || index >= (int)commands_.size()) return;
    paletteOpen_ = false;
    const Command& c = commands_[index];
    // Running a stage's command from elsewhere switches to that stage.
    if (c.workspace >= 0 && c.workspace != workspace_ && !prefsOpen_) workspace_ = c.workspace, leftScroll_ = 0;
    c.run();
}

bool Editor::handlePalette(const Input& in) {
    if (!paletteOpen_) return false;
    if (in.pressed(KEY_ESCAPE)) {
        paletteOpen_ = false;
        return true;
    }
    paletteMouseMoved_ = in.mouseDX != 0 || in.mouseDY != 0;
    for (char c : in.text)
        if (c >= 32 && c < 127) paletteQuery_ += c, paletteSel_ = 0;
    if (in.keyRepeat[KEY_BACKSPACE] && !paletteQuery_.empty()) paletteQuery_.pop_back(), paletteSel_ = 0;
    std::vector<int> m = paletteMatches();
    if (in.keyRepeat[KEY_DOWN] || (in.keyRepeat[KEY_TAB] && !in.shift())) ++paletteSel_;
    if (in.keyRepeat[KEY_UP] || (in.keyRepeat[KEY_TAB] && in.shift())) --paletteSel_;
    if (in.wheel != 0) paletteSel_ -= (int)std::round(in.wheel);
    paletteSel_ = m.empty() ? 0 : std::max(0, std::min(paletteSel_, (int)m.size() - 1));
    if (in.pressed(KEY_ENTER) && !m.empty()) runCommand(m[paletteSel_]);
    return true;
}

void Editor::buildPalette() {
    if (!paletteOpen_) return;
    const float fs = (float)fontScale_;
    const float pad = 5 * fs, rowH = ui_.rowHeight();
    const float w = std::min(240.0f * fs, (float)screenW_ - 4 * fs);
    std::vector<int> m = paletteMatches();
    const int maxRows = std::max(3, std::min(14, (int)((screenH_ - topBar_.h - 40 * fs) / rowH) - 3));
    const int rows = std::min((int)m.size(), maxRows);
    const float h = 2 * pad + (rows + 2) * rowH;
    Rect box{std::floor((screenW_ - w) * 0.5f), topBar_.y + topBar_.h + 6 * fs, w, h};
    ui_.rect({0, 0, (float)screenW_, (float)screenH_}, {0, 0, 0, 0.35f});
    ui_.rect(box, theme::panel);
    ui_.border(box, theme::accent, fs);
    Rect field{box.x + pad, box.y + pad, box.w - 2 * pad, rowH};
    ui_.rect(field, theme::field);
    const bool blink = std::fmod(clock_, 1.0) < 0.6;
    std::string shown = paletteQuery_.empty() ? std::string("Type to search commands...") : paletteQuery_;
    ui_.text(field.x + 3 * fs, field.y + (rowH - ui_.glyphH()) * 0.5f, ui_.fitText(shown, field.w - 8 * fs),
             paletteQuery_.empty() ? theme::textDim : theme::white);
    if (blink && !paletteQuery_.empty())
        ui_.rect({field.x + 3 * fs + ui_.textWidth(paletteQuery_), field.y + 2 * fs, (float)fs, rowH - 4 * fs}, theme::white);
    int first = std::max(0, std::min(paletteSel_ - rows / 2, (int)m.size() - rows));
    for (int k = 0; k < rows; ++k) {
        const int idx = m[first + k];
        const Command& c = commands_[idx];
        Rect r{box.x + pad, box.y + pad + (k + 1) * rowH + 2 * fs, box.w - 2 * pad, rowH};
        const bool sel = first + k == paletteSel_;
        if (ui_.hovered(r) && paletteMouseMoved_) paletteSel_ = first + k;
        if (ui_.selectable(uiHash("pal", (uint32_t)idx), r, "", sel, false)) {
            runCommand(idx);
            return;
        }
        std::string where = c.workspace >= 0 ? workspaceName(c.workspace) : "";
        std::string key = c.key != input::Action::Count ? input::toString(keys_.slots[(int)c.key][0]) : "";
        if (key == "-") key.clear();
        std::string right = key.empty() ? where : (where.empty() ? key : key + "  " + where);
        float rw = ui_.textWidth(right);
        ui_.text(r.x + 3 * fs, r.y + (rowH - ui_.glyphH()) * 0.5f, ui_.fitText(c.name, r.w - rw - 10 * fs),
                 sel ? theme::white : theme::text);
        ui_.text(r.x + r.w - rw - 3 * fs, r.y + (rowH - ui_.glyphH()) * 0.5f, right, sel ? theme::white : theme::textDim);
    }
    std::string foot = m.empty() ? "No command matches." : strf("%d commands   Up/Down/Tab choose, Enter run, Esc close", (int)m.size());
    ui_.text(box.x + pad, box.y + box.h - pad - ui_.glyphH(), ui_.fitText(foot, box.w - 2 * pad), theme::textDim);
}

bool Editor::runNamedCommand(const std::string& name, std::string& matched) {
    if (commands_.empty()) buildCommands();
    std::string want = name;
    for (char& c : want) c = (char)std::tolower((unsigned char)c);
    int found = -1;
    for (int i = 0; i < (int)commands_.size() && found < 0; ++i) {
        std::string n = commands_[i].name;
        for (char& c : n) c = (char)std::tolower((unsigned char)c);
        if (n == want) found = i;
    }
    if (found < 0) {  // else the first search match
        std::string saved = paletteQuery_;
        paletteQuery_ = name;
        std::vector<int> m = paletteMatches();
        paletteQuery_ = saved;
        if (!m.empty()) found = m[0];
    }
    if (found < 0) return false;
    matched = commands_[found].name;
    runCommand(found);
    return true;
}
