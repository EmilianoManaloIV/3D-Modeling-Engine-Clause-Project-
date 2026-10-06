// Left panel (one workspace per stage of the modeling pipeline), the Last
// operation panel, status bar, viewport header / tool palette / overlays,
// the UV editor, help and the quit dialog. The right panel (outliner +
// properties) is in editor_props.cpp; the top bar, menus, command palette and
// Settings page in editor_commands.cpp.
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
const char* const kShadingNames[SHADE_COUNT] = {"Studio", "Lit", "Checker", "Weights"};
}  // namespace

// ============================================================================
// Widget helpers
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
    ui_.tooltip("Drag sideways (Shift: fine), or click and type a value / expression (2*pi, +=0.5). Tab: next field");
    return false;
}

void Editor::processKeyCapture(const Input& in) {
    if (captureAction_ < 0) return;
    if (!settingsOpen_ || settingsTab_ != 2) {
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
            if (input::info(a).held) b.ctrl = b.shift = false;  // movement keys: Shift = boost (Alt kept: Alt+arrows)
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

// ============================================================================
// Left panel: the tools of the current workspace (or Settings)
// ============================================================================
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
    if (settingsOpen_) buildSettings(L, in);
    else buildWorkspacePanel(L, in);
    (void)rowH;
    leftContentH_ = (L.y - top) + 2 * pad;
    ui_.popClip();
    // Scroll bar when the content is taller than the panel.
    if (leftContentH_ > leftPanel_.h) {
        float k = leftPanel_.h / leftContentH_;
        float barH = std::max(12 * fs, leftPanel_.h * k);
        float y = leftPanel_.y + (leftPanel_.h - barH) * (leftScroll_ / std::max(1.0f, leftContentH_ - leftPanel_.h));
        ui_.rect({leftPanel_.x + leftPanel_.w - 2 * fs, y, 1.5f * fs, barH}, withAlpha(theme::textDim, 0.5f));
    }
    ui_.rect({leftPanel_.x + leftPanel_.w - 1, leftPanel_.y, 1, leftPanel_.h}, theme::border);
}

void Editor::buildWorkspacePanel(PanelLayout& L, const Input& in) {
    (void)in;
    const float fs = (float)fontScale_;
    const float gap = 3 * fs, rowH = ui_.rowHeight();
    auto note = [&](const char* text) { label(L, text, theme::textDim); };
    // A grid of command buttons: {id, short label}.
    auto grid = [&](std::initializer_list<std::pair<const char*, const char*>> items, int cols) {
        int i = 0;
        Rect row;
        for (const auto& it : items) {
            if (i % cols == 0) row = L.row(rowH);
            cmdButton(it.first, cell(row, i % cols, cols, gap), it.second);
            ++i;
        }
    };
    auto modeRow = [&]() {
        Rect row = L.row(rowH);
        if (ui_.button(uiHash("mode.object"), cell(row, 0, 2, gap), "Object", mode_ == Mode::Object))
            setMode(Mode::Object);
        ui_.tooltip("Object mode: select and transform whole objects   (" +
                    shortcutText((int)input::Action::ToggleEditMode) + ")");
        if (ui_.button(uiHash("mode.edit"), cell(row, 1, 2, gap), "Edit", mode_ == Mode::Edit)) setMode(Mode::Edit);
        ui_.tooltip("Edit mode: vertices, edges and faces of the active mesh   (" +
                    shortcutText((int)input::Action::ToggleEditMode) + ")");
    };
    static const char* const kTitles[kWorkspaceCount] = {"MODEL", "TEXTURE", "RIG", "LIGHT", "RENDER"};
    {
        Rect title = L.row(rowH);
        std::string t = kTitles[(int)ws_];
        if (ws_ == Workspace::Model || ws_ == Workspace::Texture || ws_ == Workspace::Rig)
            t += mode_ == Mode::Edit ? "  (edit)" : "  (object)";
        ui_.text(title.x, title.y + (rowH - ui_.glyphH()) * 0.5f, t, theme::accent);
    }

    switch (ws_) {
        case Workspace::Model: {
            modeRow();
            if (mode_ == Mode::Edit) {
                if (section(L, "m.select", "SELECT")) {
                    grid({{"mesh.vertex", "Vertex"}, {"mesh.edge", "Edge"}, {"mesh.face", "Face"}}, 3);
                    grid({{"edit.selectall", "All"}, {"mesh.more", "More"}, {"mesh.less", "Less"}}, 3);
                    grid({{"mesh.loop", "Loop"}, {"mesh.ring", "Ring"}}, 2);
                }
                if (section(L, "m.add", "ADD GEOMETRY")) {
                    grid({{"mesh.extrude", "Extrude"}, {"mesh.inset", "Inset"}, {"mesh.bevel", "Bevel"},
                          {"mesh.loopcut", "Loop cut"}, {"mesh.subdivide", "Subdivide"}, {"mesh.connect", "Connect"},
                          {"mesh.poke", "Poke"}, {"mesh.fill", "Fill"}, {"mesh.bridge", "Bridge"},
                          {"mesh.push", "Push thru"}},
                         2);
                }
                if (lastOpAdjustable()) lastOpPanel(L);
                if (section(L, "m.remove", "REMOVE / MERGE")) {
                    grid({{"edit.delete", "Delete"}, {"mesh.merge", "Merge"}}, 2);
                    grid({{"mesh.collapse", "Collapse groups"}}, 1);
                }
                if (section(L, "m.normals", "SHADING", false)) {
                    grid({{"obj.flat", "Flat"}, {"obj.smooth", "Smooth"}, {"obj.flip", "Flip"},
                          {"obj.subdivide", "Subdiv"}},
                         2);
                }
            } else {
                if (section(L, "m.create", "CREATE")) {
                    Rect row;
                    for (int s = 0; s < PS_Count; ++s) {
                        if (s % 2 == 0) row = L.row(rowH);
                        std::string id = std::string("add.") + shapeDef(s).name;
                        for (char& c : id) c = (char)std::tolower((unsigned char)c);
                        cmdButton(id.c_str(), cell(row, s % 2, 2, gap), shapeDef(s).name);
                    }
                    grid({{"add.empty", "Empty"}, {"add.emitter", "Emitter"}}, 2);
                }
                if (section(L, "m.object", "OBJECT")) {
                    grid({{"edit.duplicate", "Duplicate"}, {"edit.delete", "Delete"}, {"obj.join", "Join"},
                          {"obj.parent", "Parent"}, {"obj.flat", "Flat"}, {"obj.smooth", "Smooth"},
                          {"obj.subdivide", "Subdivide"}, {"obj.flip", "Flip"}},
                         2);
                }
                if (section(L, "m.boolean", "BOOLEAN", false)) {
                    grid({{"bool.union", "Union"}, {"bool.diff", "Diff"}, {"bool.inter", "Inter"}}, 3);
                    grid({{"bool.tris", "Tris out"}, {"bool.keep", "Keep cut"}}, 2);
                    note("Cutters first, then");
                    note("the target last.");
                }
                if (section(L, "m.clean", "CLEAN UP", false)) {
                    grid({{"clean.ngons", "Ngon>Tri"}, {"clean.tris", "All>Tris"}, {"clean.cleanup", "Clean up"},
                          {"clean.quads", "Tri>Quad"}},
                         2);
                }
            }
            break;
        }
        case Workspace::Texture: {
            modeRow();
            if (section(L, "t.unwrap", "UNWRAP  (U = Smart)")) {
                grid({{"uv.smart", "Smart"}, {"uv.box", "Box"}, {"uv.planar", "Planar"}, {"uv.cylinder", "Cylinder"},
                      {"uv.sphere", "Sphere"}, {"uv.per face", "Per face"}},
                     2);
                note(mode_ == Mode::Edit ? "Only selected faces." : "Whole mesh.");
            }
            if (section(L, "t.tools", "UV TOOLS")) {
                grid({{"uv.tool0", "Fit 0-1"}, {"uv.tool1", "Pack"}, {"uv.tool2", "Rotate 90"}, {"uv.tool3", "Flip U"},
                      {"uv.tool4", "Flip V"}, {"uv.editor", "UV editor"}},
                     2);
                grid({{"view.shade.checker", "Checker view"}}, 1);
            }
            if (section(L, "t.material", "MATERIAL")) {
                grid({{"mat.glass", "Glass"}, {"mat.metal", "Metal"}, {"mat.matte", "Matte"}}, 3);
                grid({{"mat.folder", "Texture folder..."}}, 1);
                grid({{"mat.reload", "Reload textures"}}, 1);
                note("Values and texture");
                note("maps: Properties >");
                note("Material (right).");
                note("Or drop images on");
                note("the window.");
            }
            break;
        }
        case Workspace::Rig: {
            modeRow();
            if (section(L, "r.bones", "BONES")) {
                grid({{"rig.bone", "Add bone"}, {"rig.extrude", "Extrude"}, {"rig.rest", "Rest pose"},
                      {"view.frame", "Frame"}},
                     2);
            }
            if (section(L, "r.skin", "SKIN")) {
                grid({{"rig.bind", "Bind"}, {"rig.unbind", "Unbind"}, {"rig.auto", "Auto wgt"},
                      {"rig.normalize", "Normalize"}},
                     2);
                grid({{"rig.weights", "Weights view"}}, 1);
                if (mode_ == Mode::Edit) {
                    floatRow(L, "Weight", "skin.w", 0, weightValue_, 0.005f, 0.0f, 1.0f);
                    grid({{"rig.assign", "Assign"}, {"rig.remove", "Remove"}}, 2);
                } else {
                    note("Select the mesh, then");
                    note("the root bone: Bind.");
                }
            }
            if (section(L, "r.hier", "HIERARCHY")) {
                grid({{"obj.parent", "Parent"}, {"obj.unparent", "Unparent"}}, 2);
                note("Or drag rows in the");
                note("scene list.");
            }
            if (section(L, "r.fx", "EFFECTS", false)) {
                grid({{"add.emitter", "Emitter"}, {"fx.play", "Play"}, {"fx.restart", "Restart"}}, 3);
            }
            break;
        }
        case Workspace::Light: {
            if (section(L, "l.add", "ADD LIGHT")) {
                grid({{"add.point", "Point"}, {"add.sun", "Sun"}, {"add.spot", "Spot"}, {"add.area", "Area"}}, 2);
                grid({{"light.lit", "Lit view"}}, 1);
            }
            if (section(L, "l.camera", "CAMERA")) {
                grid({{"add.camera", "Add camera"}}, 1);
                grid({{"view.camera", "Look (0)"}, {"view.camtoview", "To view"}}, 2);
            }
            if (section(L, "l.env", "ENVIRONMENT")) {
                label(L, "Ambient light (R G B)", theme::textDim);
                vec3Fields(L, "ambient", 0, scene_.ambient, 0.003f, 0.0f, 1.0f, kRgbColor);
            }
            if (section(L, "l.list", "LIGHTS IN THE SCENE")) {
                int n = 0;
                for (int i = 0; i < (int)scene_.objects.size(); ++i) {
                    const Object& o = scene_.objects[i];
                    if (o.kind != ObjectKind::Light && o.kind != ObjectKind::Camera) continue;
                    Rect r = L.row(rowH);
                    if (ui_.selectable(uiHash("lightlist", o.id), r, "", o.selected, i == scene_.active)) selectOnly(i);
                    Color c = o.kind == ObjectKind::Light ? toColor(o.light.finalColor()) : Color{0.55f, 0.85f, 1.0f, 1};
                    ui_.rect({r.x + 2 * fs, r.y + r.h * 0.5f - 2.5f * fs, 5 * fs, 5 * fs}, c);
                    static const char* const kTypes[4] = {"point", "sun", "spot", "area"};
                    std::string t = o.name + "  " + (o.kind == ObjectKind::Camera ? "camera" : kTypes[(int)o.light.type]);
                    ui_.textIn({r.x + 9 * fs, r.y, r.w - 9 * fs, r.h}, t, theme::text, false);
                    ++n;
                }
                if (!n) note("No lights yet.");
            }
            break;
        }
        case Workspace::Render: {
            if (section(L, "rd.device", "DEVICE")) {
                grid({{"render.gpu", "GPU"}, {"render.cpu", "CPU"}, {"render.rtx", "RTX"}}, 3);
                label(L, hwrtInfo_.available ? "RTX: " + hwrtInfo_.adapter : std::string("RTX: no DXR 1.1 GPU"),
                      hwrtInfo_.available && !hwrtInfo_.software ? theme::accent : theme::textDim);
            }
            if (section(L, "rd.render", "RENDER")) {
                grid({{"render.view", renderView_ ? "Stop" : "Render"}, {"render.save", "Save PNG"}}, 2);
                if (renderView_) {
                    const int spp = renderSamples();
                    Rect bar = L.row(rowH * 0.6f);
                    ui_.rect(bar, theme::field);
                    float k = clampf((float)spp / std::max(1.0f, renderSet_.samples), 0.0f, 1.0f);
                    ui_.rect({bar.x, bar.y, bar.w * k, bar.h}, theme::accent);
                    label(L, strf("%d / %d spp  %.1fs", spp, (int)renderSet_.samples, renderSeconds()),
                          renderRunning() ? theme::text : theme::white);
                    label(L, strf("%.2f Msamples/s", renderRate() * 1e-6), theme::textDim);
                    if (rtScene_)
                        label(L, strf("%d tris, BVH %.0f ms", (int)rtScene_->triangleCount(), rtScene_->buildMs),
                              theme::textDim);
                }
                Rect row = L.row(rowH);
                const bool hasCam = scene_.renderCameraIndex() >= 0;
                if (ui_.button(uiHash("rt.fromcam"), cell(row, 0, 2, gap), "Camera", renderSet_.useCamera && hasCam)) {
                    if (hasCam) renderSet_.useCamera = true;
                    else setStatus("No camera object yet: Light > Add camera adds one at the current view", true);
                }
                ui_.tooltip("Render through the scene's camera (exposure, depth of field)");
                if (ui_.button(uiHash("rt.fromview"), cell(row, 1, 2, gap), "Viewport", !renderSet_.useCamera || !hasCam))
                    renderSet_.useCamera = false;
                ui_.tooltip("Render the editor view");
                if (renderSet_.useCamera && hasCam) {
                    const Object& c = scene_.objects[scene_.renderCameraIndex()];
                    label(L, strf("%s: f/%.1g %.0fmm", c.name.c_str(), c.camera.fStop, c.camera.focalLength),
                          theme::textDim);
                    label(L, strf("ISO %.0f 1/%.0fs x%.2f", c.camera.iso, 1.0f / std::max(1e-6f, c.camera.shutter),
                                  c.camera.exposure()),
                          theme::textDim);
                }
            }
            if (section(L, "rd.sampling", "SAMPLING")) {
                auto setRow = [&](const char* text, const char* key, float& v, float speed, float lo, float hi,
                                  bool integer, const char* tip) {
                    Rect r = L.row(rowH);
                    float lw = std::floor(r.w * 0.52f);
                    ui_.textIn({r.x, r.y, lw, r.h}, text, theme::textDim, false);
                    float value = v;
                    if (ui_.dragFloat(uiHash(key), {r.x + lw, r.y, r.w - lw, r.h}, value, speed, theme::accent, lo, hi))
                        v = integer ? std::round(value) : value;
                    ui_.tooltip(tip);
                };
                setRow("Samples", "rt.spp", renderSet_.samples, 1.0f, 1.0f, 65536.0f, true,
                       "Samples per pixel: more = less noise (raising it continues a finished render)");
                Rect row = L.row(rowH);
                static const int kSpp[4] = {16, 64, 256, 1024};
                for (int c = 0; c < 4; ++c)
                    if (ui_.button(uiHash("rt.sppq", (uint32_t)c), cell(row, c, 4, gap), std::to_string(kSpp[c]),
                                   (int)renderSet_.samples == kSpp[c]))
                        renderSet_.samples = (float)kSpp[c];
                setRow("Bounces", "rt.bounce", renderSet_.bounces, 0.05f, 0.0f, 32.0f, true,
                       "Light bounces after the first hit");
                setRow("Res. %", "rt.res", renderSet_.resolution, 0.5f, 5.0f, 200.0f, true,
                       "Render resolution, % of the viewport");
                setRow("Clamp", "rt.clamp", renderSet_.clamp, 0.05f, 0.0f, 1000.0f, false,
                       "Firefly clamp (0 = off)");
                setRow("Sky", "rt.env", renderSet_.envStrength, 0.01f, 0.0f, 50.0f, false, "Sky / ambient strength");
                setRow("Soft shad.", "rt.lsize", renderSet_.lightSize, 0.002f, 0.0f, 2.0f, false,
                       "Size of point / spot lamps (shadow softness)");
                row = L.row(rowH);
                if (ui_.button(uiHash("rt.studio"), cell(row, 0, 2, gap), "Studio", renderSet_.studioLights))
                    renderSet_.studioLights = !renderSet_.studioLights;
                ui_.tooltip("Studio key / fill lights when the scene has no lamps");
                cmdButton("render.live", cell(row, 1, 2, gap), "Live");
            }
            if (section(L, "rd.devices", "PERFORMANCE", false)) {
                auto setRow = [&](const char* text, const char* key, float& v, float speed, float lo, float hi, bool integer) {
                    Rect r = L.row(rowH);
                    float lw = std::floor(r.w * 0.52f);
                    ui_.textIn({r.x, r.y, lw, r.h}, text, theme::textDim, false);
                    float value = v;
                    if (ui_.dragFloat(uiHash(key), {r.x + lw, r.y, r.w - lw, r.h}, value, speed, theme::accent, lo, hi))
                        v = integer ? std::round(value) : value;
                };
                setRow("GPU ms", "rt.budget", renderSet_.gpuBudgetMs, 0.1f, 1.0f, 200.0f, false);
                setRow("Threads", "rt.threads", renderSet_.cpuThreads, 0.05f, 0.0f, 256.0f, true);
                note(renderSet_.cpuThreads < 1 ? strf("(0 = all %d threads)", jobs::hardwareThreads()).c_str()
                                               : strf("of %d hw threads", jobs::hardwareThreads()).c_str());
                if (ui_.button(uiHash("rt.warp"), L.row(rowH), "Software DXR", renderSet_.allowWarp)) {
                    renderSet_.allowWarp = !renderSet_.allowWarp;
                    hwrtInfo_ = HwRayTracer::probe(renderSet_.allowWarp);
                }
                ui_.tooltip("Let the RTX device use Microsoft WARP (CPU) when no DXR GPU exists - for testing");
                for (const std::string& ad : hwrtInfo_.adapters) label(L, ad, theme::textDim);
                for (const std::string& line : gpuReport()) label(L, line, softwareGl_ ? theme::error : theme::textDim);
            }
            break;
        }
    }
}

// The settings of the last mesh tool; changing one re-runs the tool.
void Editor::lastOpPanel(PanelLayout& L) {
    static const char* const kNames[] = {"", "INSET", "BEVEL", "LOOP CUT", "SUBDIVIDE", "BRIDGE", "PUSH THROUGH", "POKE"};
    const float fs = (float)fontScale_, rowH = ui_.rowHeight();
    ui_.header(L.row(11 * fs), std::string("LAST: ") + kNames[lastOp_.type]);
    bool changed = false;
    auto opRow = [&](const char* text, const char* key, float& v, float speed, float lo, float hi, const char* tip) {
        Rect r = L.row(rowH);
        float lw = std::floor(r.w * 0.45f);
        ui_.textIn({r.x, r.y, lw, r.h}, text, theme::textDim, false);
        if (ui_.dragFloat(uiHash(key), {r.x + lw, r.y, r.w - lw, r.h}, v, speed, theme::accent, lo, hi)) changed = true;
        ui_.tooltip(tip);
    };
    auto intRow = [&](const char* text, const char* key, int& v, int lo, int hi, const char* tip) {
        float f = (float)v;
        opRow(text, key, f, 0.05f, (float)lo, (float)hi, tip);
        int n = (int)std::lround(f);
        if (n != v) v = n, changed = true;
    };
    switch (lastOp_.type) {
        case MeshOp::Inset: {
            meshedit::InsetParams& ip = lastOp_.inset;
            opRow("Width", "op.thick", ip.thickness, 0.002f, 0.0f, 1000.0f, "Width of the new ring of faces");
            opRow("Depth", "op.depth", ip.depth, 0.002f, -1000.0f, 1000.0f, "< 0 recesses the inset part, > 0 raises it");
            opRow("Dish", "op.dish", ip.dish, 0.002f, -1000.0f, 1000.0f, "Centre vertex height: < 0 concave, > 0 convex");
            if (ui_.button(uiHash("op.ind"), L.row(rowH), "Individual faces", ip.individual))
                ip.individual = !ip.individual, changed = true;
            if (changed) insetDefaults_ = ip;
            break;
        }
        case MeshOp::Bevel: {
            meshedit::BevelParams& bp = lastOp_.bevel;
            opRow("Width", "op.bw", bp.width, 0.002f, 0.0001f, 1000.0f, "How far the cut moves along each edge");
            intRow("Segments", "op.bseg", bp.segments, 1, 32, "Faces across the bevel: 1 chamfers, more rounds it");
            opRow("Profile", "op.bprof", bp.profile, 0.005f, 0.0f, 1.0f, "0 flat, 0.5 round, 1 back to the corner");
            if (ui_.button(uiHash("op.bclamp"), L.row(rowH), "Clamp overlap", bp.clamp)) bp.clamp = !bp.clamp, changed = true;
            if (changed) bevelDefaults_ = bp;
            break;
        }
        case MeshOp::LoopCut:
        case MeshOp::Subdivide:
            intRow("Cuts", "op.cuts", lastOp_.cuts, 1, 64, "Number of new loops / points per edge");
            break;
        case MeshOp::Bridge:
            intRow("Segments", "op.brseg", lastOp_.bridge.segments, 1, 256, "Rings of faces along the bridge");
            intRow("Twist", "op.twist", lastOp_.bridge.twist, -64, 64, "Rotate the pairing of the two loops");
            break;
        case MeshOp::PushThrough:
            opRow("Inset", "op.pinset", lastOp_.push.inset, 0.002f, 0.0f, 1000.0f,
                  "Inset first: leaves a frame around the hole");
            if (!lastOp_.error.empty()) {
                label(L, "Could not push through:", theme::error);
                label(L, "try a larger Inset.", theme::error);
            }
            break;
        case MeshOp::Poke:
            opRow("Offset", "op.poke", lastOp_.offset, 0.002f, -1000.0f, 1000.0f, "Height of the centre vertex");
            break;
        default:
            break;
    }
    if (changed) applyLastOp();
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
    const std::string palette = shortcutText((int)input::Action::Palette);
    if (xf_ != Xform::None) {
        msg = xfInfo_ + (modalNumber_.empty() ? "" : "  [" + modalNumber_ + "]") +
              "   X/Y/Z axis | type a number | Ctrl snap | LMB/Enter confirm | RMB/Esc cancel";
        c = theme::selection;
    } else if (flyToggle_) {
        msg = "Fly mode: W A S D Q E move | drag or Alt+arrows look | Shift faster | Esc ends";
        c = theme::selection;
    } else if (!status_.empty() && statusTime_ < 6.0f) {
        msg = status_;
        c = statusError_ ? theme::error : theme::text;
    } else if (mode_ == Mode::Edit) {
        msg = "1 2 3 vertex/edge/face | I inset | Ctrl+B bevel | Ctrl+R loop cut | double-click edge: loop | " +
              palette + " all commands";
        c = theme::textDim;
    } else {
        msg = keymap_ == Keymap::Unity
                  ? "Q W E R Y tools | Alt+LMB orbit | MMB pan | RMB+WASD fly | " + palette + " search commands | ? help"
                  : "Click: select | RMB drag: orbit | Shift+RMB: pan | G/R/S | " + palette + " search commands | ? help";
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
    static const char* const kIds[SHADE_COUNT] = {"view.shade.studio", "view.shade.lit", "view.shade.checker",
                                                  "view.shade.weights"};
    static const char* const kShort[SHADE_COUNT] = {"Std", "Lit", "Chk", "Wgt"};
    // Short labels when the viewport is narrow (big UI scale, small window).
    float full = 0;
    for (int s = 0; s < SHADE_COUNT; ++s) full += ui_.textWidth(kShadingNames[s]) + 8 * fs + gap;
    full += 3 * gap + ui_.textWidth("WireGridUVPause") + 4 * (8 * fs + gap);
    const bool compact = full > headerRect_.w - 2 * pad;
    auto button = [&](const char* id, const char* text) {
        float w = ui_.textWidth(text) + (compact ? 5 : 8) * fs;
        cmdButton(id, {x, y, w, h}, text);
        x += w + (compact ? fs : gap);
    };
    for (int s = 0; s < SHADE_COUNT; ++s) button(kIds[s], compact ? kShort[s] : kShadingNames[s]);
    x += compact ? gap : 3 * gap;
    button("view.wire", "Wire");
    button("view.grid", "Grid");
    button("uv.editor", "UV");
    button("fx.play", playing_ ? (compact ? "||" : "Pause") : (compact ? ">" : "Play"));
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
        static const char* const kSel[3] = {"vertices", "edges", "faces"};
        ui_.text(x, y, strf("Editing: %s  (%d %s)", active()->name.c_str(), countEditSelection(), kSel[(int)selMode_]),
                 withAlpha(theme::selection, 0.9f));
        y += 10 * fs;
    }
    if (shading_ == SHADE_LIT) {
        int lights = 0;
        for (const Object& o : scene_.objects) lights += o.kind == ObjectKind::Light;
        ui_.text(x, y,
                 lights ? strf("Lit: %d light%s%s", lights, lights == 1 ? "" : "s", lights > kMaxLights ? " (8 used)" : "")
                        : std::string("Lit: no lights - add one (Light workspace)"),
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
    if (flyToggle_) {
        ui_.text(x, y, "FLY MODE  (Esc ends)", theme::selection);
        y += 10 * fs;
    }
    if (xf_ != Xform::None)
        ui_.text(x, y, xfInfo_ + (modalNumber_.empty() ? "" : "   typed: " + modalNumber_), theme::selection);

    if (scene_.objects.empty()) {
        const std::string msg = "Empty scene - Add menu, or Model > Create";
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
        disc(c, len + 6 * fs, withAlpha(theme::panelDark, navPad_ == 0 ? 0.8f : 0.45f));
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
        // Navigation pads (drag them): pan and zoom without a middle / right button.
        if (access_.navButtons) {
            static const char* const kPad[3] = {"Orbit", "Pan", "Zoom"};
            for (int p = 1; p < 3; ++p) {
                const Rect& r = navPads_[p];
                bool hotPad = navPad_ == p || r.contains(mouse_.x + viewport_.x, mouse_.y + viewport_.y);
                ui_.rect(r, withAlpha(hotPad ? theme::buttonHover : theme::panelDark, 0.8f));
                ui_.border(r, withAlpha(theme::textDim, 0.6f), std::max(1.0f, fs * 0.5f));
                ui_.textIn(r, kPad[p], hotPad ? theme::white : theme::text, true);
            }
        }
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
        ui_.textIn({r.x, r.y + r.h * 0.45f, r.w, 10 * fs}, o ? "No UVs - unwrap (Texture)" : "Select a mesh",
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
    if (o->mesh.faces.size() > limit)
        ui_.text(r.x + 2 * fs, r.y + r.h - 9 * fs, strf("first %d of %d faces", (int)limit, (int)o->mesh.faces.size()),
                 theme::textDim);
    ui_.popClip();
    ui_.border(r, theme::textDim, std::max(1.0f, fs * 0.5f));
}

void Editor::buildHelp() {
    static const char* const kUnity[][2] = {
        {"UNITY KEYMAP", ""},
        {"Q W E R Y", "Hand / Move / Rotate / Scale / All tools"},
        {"Drag a handle", "Ctrl snaps; Shift+drag (Edit) extrudes"},
        {"Alt+LMB  MMB  Alt+RMB", "Orbit / pan / zoom; Alt+Ctrl+LMB pans too"},
        {"RMB + W A S D Q E", "Fly (Shift+F: fly without holding RMB)"},
        {"Click / drag", "Select / box (Shift or Ctrl adds)"},
        {"Ctrl+D / Delete / G", "Duplicate / delete / grab (type a number)"},
    };
    static const char* const kBlender[][2] = {
        {"BLENDER KEYMAP", ""},
        {"G / R / S", "Move / rotate / scale; X Y Z axis; 2 Enter"},
        {"RMB / MMB drag", "Orbit; Shift: pan; wheel: zoom"},
        {"A / E / X / Shift+D", "Select all / extrude / delete / duplicate"},
    };
    static const char* const kCommon[][2] = {
        {"", ""},
        {"BOTH KEYMAPS", ""},
        {"Ctrl+K  Shift+Space", "Search and run any command (keyboard)"},
        {"Alt+1 .. Alt+5", "Model > Texture > Rig > Light > Render"},
        {"Tab, then 1 2 3", "Edit mode: vertex / edge / face"},
        {"I  Ctrl+B  Ctrl+R", "Inset / bevel / loop cut"},
        {"J  M  Alt+F  Alt+B", "Connect / merge / fill / bridge"},
        {"Alt+E", "Push through (hole and tunnel)"},
        {"Dbl-click edge", "Select loop; Ctrl+= / Ctrl+- grow, shrink"},
        {"Ctrl+P  Alt+P  Ctrl+J", "Parent / clear parent / join"},
        {"F   [ ]   0", "Frame / prev, next object / camera view"},
        {"Alt+arrows  =  -", "Orbit / zoom from the keyboard"},
        {"Ctrl+Z Y  Ctrl+S O", "Undo, redo / save, open"},
        {"F1 F3 F5 F12", "Help, stats, render, screenshot"},
        {"  no F-keys:", "S-/ (?)   C-I   C-S-R   C-S-P"},
        {"Trackpad", "Settings > Input: 2 fingers orbit, pinch zooms"},
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
    const float keyW = 23 * 6 * fs, descW = 46 * 6 * fs;
    const float w = std::min((float)screenW_, keyW + descW + 2 * pad), h = (n + 3) * lineH + 2 * pad;
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
        input::Action action;
        const char* hint;
    };
    static const Entry tools[] = {
        {"Hand", Tool::Hand, input::Action::ToolHand, "Drag to pan the view"},
        {"Move", Tool::Move, input::Action::ToolMove, "Arrows: one axis, squares: a plane, centre: the view plane"},
        {"Rotate", Tool::Rotate, input::Action::ToolRotate, "Rings rotate around an axis; inside: trackball"},
        {"Scale", Tool::Scale, input::Action::ToolScale, "Cubes scale one axis; centre: uniform"},
        {"All", Tool::Universal, input::Action::ToolUniversal, "Move, rotate and scale handles together"}};
    for (const Entry& e : tools) {
        if (ui_.button(uiHash("tool", (uint32_t)e.tool), {x, y, w, h}, e.label, tool_ == e.tool)) setTool(e.tool);
        std::string keys = shortcutText((int)e.action);
        ui_.tooltip(std::string(e.label) + " tool" + (keys.empty() ? "" : "   (" + keys + ")") + "\n" + e.hint);
        y += h + gap;
    }
    y += 3 * fs;
    if (ui_.button(uiHash("tool.space"), {x, y, w, h}, localSpace_ ? "Local" : "Global", false)) {
        localSpace_ = !localSpace_;
        setStatus(localSpace_ ? "Handle orientation: Local" : "Handle orientation: Global");
    }
    ui_.tooltip("Handle axes: world (Global) or the object's own (Local)   (" +
                shortcutText((int)input::Action::ToggleLocalGlobal) + ")");
    y += h + gap;
    if (ui_.button(uiHash("tool.pivot"), {x, y, w, h}, pivotCenter_ ? "Center" : "Pivot", false)) {
        pivotCenter_ = !pivotCenter_;
        setStatus(pivotCenter_ ? "Handle position: Center" : "Handle position: Pivot");
    }
    ui_.tooltip("Handle at each object's origin (Pivot) or the selection centre (Center)   (" +
                shortcutText((int)input::Action::TogglePivotCenter) + ")");
    y += h + gap + 3 * fs;
    cmdButton("render.view", {x, y, w, h}, "Render");
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
    Rect box{viewport_.x + viewport_.w - w - 6 * fs, navPads_[2].y + navPads_[2].h + 4 * fs, w, h};
    ui_.rect(box, withAlpha(theme::panelDark, 0.88f));
    float y = box.y + 4 * fs;
    for (size_t i = 0; i < lines.size(); ++i, y += lineH)
        ui_.text(box.x + 4 * fs, y, lines[i], i == 0 ? theme::white : theme::text);
}
