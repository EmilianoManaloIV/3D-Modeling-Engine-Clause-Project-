// Commands, workspaces, the command palette, the top bar with its menus,
// and the Settings page.
//
// Every operation the UI offers is registered once as a Command (id, label,
// hint, shortcut action, run / available / checked callbacks). Panel buttons,
// the menus, the command palette (Ctrl+K: type to search, Enter to run) and
// the stress test's random action fuzzer all go through this one table, so a
// feature reachable by mouse is also reachable by keyboard alone - which is
// what makes the editor usable on a 60-65% keyboard or with a trackpad.
#include "editor_internal.h"
#include "jobs.h"
#include "skin.h"

#include <algorithm>
#include <cctype>
#include <cmath>

using namespace ed;

namespace {
std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
const char* const kWorkspaceNames[5] = {"Model", "Texture", "Rig", "Light", "Render"};
const char* const kWorkspaceHints[5] = {
    "1. Model: shapes, edit mode tools (extrude, inset, bevel, loop cut, bridge...), booleans",
    "2. Texture: UV unwrapping, the UV editor, materials and PBR texture maps",
    "3. Rig: bones, binding a mesh to a skeleton, weights, hierarchy, effects",
    "4. Light: lamps (point, sun, spot, area), colour temperature, cameras, ambient light",
    "5. Render: path tracing on the GPU, CPU or RTX, samples, camera exposure, saving the image",
};
}  // namespace

// ============================================================================
// Registry
// ============================================================================
void Editor::registerCommands() {
    commands_.clear();
    commandIndex_.clear();
    using A = input::Action;
    auto add = [&](const std::string& id, const std::string& label, const std::string& category, const std::string& hint,
                   input::Action action, std::function<void()> run, std::function<bool()> available = nullptr,
                   std::function<bool()> checked = nullptr, const std::string& why = "") {
        Command c;
        c.id = id;
        c.label = label;
        c.category = category;
        c.hint = hint;
        c.action = action == A::Count ? -1 : (int)action;
        c.run = std::move(run);
        c.available = std::move(available);
        c.checked = std::move(checked);
        c.why = why;
        commandIndex_[id] = (int)commands_.size();
        commands_.push_back(std::move(c));
    };
    auto editMode = [this] { return mode_ == Mode::Edit && activeMesh() != nullptr; };
    auto objectMode = [this] { return mode_ == Mode::Object; };
    auto hasMesh = [this] { return activeMesh() != nullptr; };
    const char* needEdit = "Needs Edit mode (Tab) on a mesh";
    const input::Action none = A::Count;

    // --- File ---
    add("file.new", "New scene", "File", "Start an empty scene (undo brings the old one back)", none, [this] { newScene(); });
    add("file.open", "Open...", "File", "Open a .m3d scene or import an .obj with the file dialog", A::Open, [this] {
        std::string p = platform::openFileDialog("Open a scene (.m3d) or a model (.obj)", false);
        if (!p.empty()) openPath(p);
        else loadFile();
    });
    add("file.save", "Save", "File", "Save the scene to the file named in the File menu", A::Save, [this] { saveFile(); });
    add("file.import", "Import OBJ...", "File", "Add the objects of a Wavefront .obj (+ .mtl materials)", none, [this] {
        std::string p = platform::openFileDialog("Import a Wavefront .obj", false);
        if (!p.empty()) {
            fileField_ = p;
            importObj();
        }
    });
    add("file.export", "Export OBJ", "File", "Write every mesh to <file>.obj + .mtl (transforms and pose baked)", none,
        [this] { exportObj(); });
    add("file.screenshot", "Screenshot", "File", "Save the window as screenshot_NNN.png", A::Screenshot,
        [this] { screenshotPending_ = true; });
    add("file.quit", "Quit", "File", "Close Modeler3D (asks to save changes)", none, [this] { requestQuit(); });

    // --- Edit ---
    add("edit.undo", "Undo", "Edit", "Step back (64 steps)", A::Undo, [this] { undo(); });
    add("edit.redo", "Redo", "Edit", "Step forward again", A::Redo, [this] { redo(); });
    add("edit.palette", "Search commands", "Edit", "Find any command by name and run it from the keyboard", A::Palette,
        [this] { openPalette(); });
    add("edit.settings", "Settings", "Edit", "Input (trackpad, UI size, tooltips), navigation and key bindings", none,
        [this] { settingsOpen_ = !settingsOpen_; }, nullptr, [this] { return settingsOpen_; });
    add("edit.selectall", "Select all / none", "Edit", "Select everything, or nothing if something is selected",
        A::SelectAll, [this] { selectAll(); });
    add("edit.duplicate", "Duplicate", "Edit", "Copy the selection (objects keep hierarchy and rigs)", A::Duplicate,
        [this] { duplicateSelected(); });
    add("edit.delete", "Delete", "Edit", "Delete the selected objects, or vertices / edges / faces in Edit mode",
        A::Delete, [this] { deleteSelected(); });
    add("edit.next", "Select next object", "Edit", "Keyboard selection: cycle forward through the objects", A::NextObject,
        [this] { cycleObject(1); }, objectMode, nullptr, "Object mode only");
    add("edit.prev", "Select previous object", "Edit", "Keyboard selection: cycle backwards through the objects",
        A::PrevObject, [this] { cycleObject(-1); }, objectMode, nullptr, "Object mode only");

    // --- Workspaces ---
    for (int w = 0; w < kWorkspaceCount; ++w)
        add(std::string("ws.") + lower(kWorkspaceNames[w]), std::string("Workspace: ") + kWorkspaceNames[w], "Workspace",
            kWorkspaceHints[w], (A)((int)A::WorkspaceModel + w), [this, w] { setWorkspace((Workspace)w); }, nullptr,
            [this, w] { return (int)ws_ == w; });

    // --- View ---
    add("view.front", "View front", "View", "Look along -Z", A::ViewFront, [this] { setView(0, 0); });
    add("view.back", "View back", "View", "Look along +Z", A::ViewBack, [this] { setView(180, 0); });
    add("view.right", "View right", "View", "Look along -X", A::ViewRight, [this] { setView(90, 0); });
    add("view.left", "View left", "View", "Look along +X", A::ViewLeft, [this] { setView(-90, 0); });
    add("view.top", "View top", "View", "Look down", A::ViewTop, [this] { setView(0, 89.9f); });
    add("view.bottom", "View bottom", "View", "Look up", A::ViewBottom, [this] { setView(0, -89.9f); });
    add("view.ortho", "Perspective / orthographic", "View", "Toggle the projection", A::ToggleOrtho,
        [this] { cam_.ortho = !cam_.ortho; }, nullptr, [this] { return cam_.ortho; });
    add("view.frame", "Frame selection", "View", "Move the camera to the selection (animated)", A::FrameSelected,
        [this] { frameSelected(); });
    add("view.wire", "Wireframe", "View", "Wireframe overlay; selection sees through the mesh (x-ray)",
        A::ToggleWireframe, [this] { wireframe_ = !wireframe_; }, nullptr, [this] { return wireframe_; });
    add("view.grid", "Grid", "View", "Show the ground grid", none, [this] { showGrid_ = !showGrid_; }, nullptr,
        [this] { return showGrid_; });
    add("view.stats", "Performance overlay", "View", "Frame time per section, GPU time, counters", A::Stats,
        [this] { showStats_ = !showStats_; }, nullptr, [this] { return showStats_; });
    add("view.help", "Help", "View", "Controls overview", A::Help, [this] { showHelp_ = true; });
    add("view.fly", "Fly mode", "View",
        "Fly the camera without holding a mouse button: W A S D Q E move, drag (or arrow keys) to look, Esc ends",
        A::FlyMode, [this] {
            flyToggle_ = !flyToggle_;
            setStatus(flyToggle_ ? "Fly mode: W A S D Q E move, drag or Alt+arrows look, Shift faster, Esc / Shift+F ends"
                                 : "Fly mode off");
        },
        nullptr, [this] { return flyToggle_; });
    add("view.zoomin", "Zoom in", "View", "Keyboard zoom (hold)", A::ZoomIn,
        [this] { cam_.distance = clampf(cam_.distance * 0.8f, 0.05f, 5000.0f); });
    add("view.zoomout", "Zoom out", "View", "Keyboard zoom (hold)", A::ZoomOut,
        [this] { cam_.distance = clampf(cam_.distance * 1.25f, 0.05f, 5000.0f); });
    add("view.camera", "Look through camera", "View", "View the scene through the render camera", A::LookThroughCamera,
        [this] { lookThroughCamera(); });
    add("view.camtoview", "Camera to view", "View", "Move the render camera to the current view",
        A::AlignCameraToView, [this] { alignActiveCameraToView(); });
    static const char* const kShade[4] = {"Studio", "Lit", "Checker", "Weights"};
    static const char* const kShadeHint[4] = {"Studio lighting with textures", "Scene lamps (up to 8)",
                                              "UV checker texture: stretching and seams",
                                              "Bone weights (blue 0 .. red 1)"};
    for (int s = 0; s < 4; ++s)
        add(std::string("view.shade.") + lower(kShade[s]), std::string("Shading: ") + kShade[s], "View", kShadeHint[s],
            none, [this, s] { shading_ = s; }, nullptr, [this, s] { return shading_ == s; });
    add("view.shading", "Cycle shading", "View", "Studio, Lit, Checker, Weights", A::CycleShading,
        [this] { shading_ = (shading_ + 1) % SHADE_COUNT; });

    // --- Create ---
    for (int s = 0; s < PS_Count; ++s)
        add(std::string("add.") + lower(shapeDef(s).name), std::string("Add ") + shapeDef(s).name, "Create",
            "Parametric shape: its settings stay editable in Properties", none, [this, s] { addShape(s); });
    add("add.empty", "Add empty", "Create", "A transform handle to group / parent objects under", none,
        [this] { addEmpty(); });
    add("add.point", "Add point light", "Create", "Shines in every direction", none,
        [this] { addLight(LightType::Point); });
    add("add.sun", "Add sun", "Create", "Parallel light from one direction", none, [this] { addLight(LightType::Sun); });
    add("add.spot", "Add spot light", "Create", "A cone of light", none, [this] { addLight(LightType::Spot); });
    add("add.area", "Add area light", "Create", "A glowing rectangle: soft shadows", none,
        [this] { addLight(LightType::Area); });
    add("add.camera", "Add camera", "Create", "A physical render camera at the current view", none,
        [this] { addCamera(); });
    add("add.emitter", "Add particle emitter", "Create", "Fire, smoke, sparks (Space plays / pauses)", none,
        [this] { addEmitter(); });
    add("add.bone", "Add bone", "Create", "Start a skeleton", none, [this] { addBone(false); });

    // --- Object ---
    add("obj.editmode", "Edit mode", "Object", "Edit the vertices, edges and faces of the active mesh", A::ToggleEditMode,
        [this] { setMode(mode_ == Mode::Object ? Mode::Edit : Mode::Object); }, nullptr,
        [this] { return mode_ == Mode::Edit; });
    add("obj.parent", "Parent", "Object", "Parent the selected objects to the active one (keeps them in place)",
        A::Parent, [this] { parentSelected(); });
    add("obj.unparent", "Unparent", "Object", "Clear the parent (keeps them in place)", A::Unparent,
        [this] { unparentSelected(); });
    add("obj.join", "Join", "Object", "Merge the selected meshes into the active one", A::JoinObjects,
        [this] { joinSelected(); }, objectMode, nullptr, "Object mode only");
    add("obj.flat", "Flat shading", "Object", "Faceted look", none, [this] { setSmoothSelected(false); });
    add("obj.smooth", "Smooth shading", "Object", "Smooth normals (sharp edges stay crisp)", none,
        [this] { setSmoothSelected(true); });
    add("obj.subdivide", "Subdivide smooth", "Object",
        "Catmull-Clark: every face becomes quads and the shape rounds off", none, [this] { subdivideSelected(); },
        hasMesh, nullptr, "Select a mesh");
    add("obj.flip", "Flip normals", "Object", "Turn faces inside out", none, [this] { flipSelected(); });

    // --- Boolean / clean-up ---
    add("bool.union", "Union", "Boolean", "Select cutters, then the target last: merges them", none,
        [this] { booleanSelected(csg::Op::Union); });
    add("bool.diff", "Difference", "Boolean", "Select cutters, then the target last: cuts them away", none,
        [this] { booleanSelected(csg::Op::Difference); });
    add("bool.inter", "Intersection", "Boolean", "Select cutters, then the target last: keeps the overlap", none,
        [this] { booleanSelected(csg::Op::Intersection); });
    add("bool.tris", "Triangulate result", "Boolean", "Booleans output triangles (off: n-gons)", none,
        [this] { csgTriangulate_ = !csgTriangulate_; }, nullptr, [this] { return csgTriangulate_; });
    add("bool.keep", "Keep cutters", "Boolean", "Keep the cutter objects after the operation", none,
        [this] { csgKeepCutters_ = !csgKeepCutters_; }, nullptr, [this] { return csgKeepCutters_; });
    add("clean.ngons", "N-gons to triangles", "Clean up", "Triangulate faces with 5+ sides", none,
        [this] { meshCleanupSelected(0); });
    add("clean.tris", "Triangulate all", "Clean up", "Triangulate every face", none, [this] { meshCleanupSelected(1); });
    add("clean.cleanup", "Clean up", "Clean up", "Weld close vertices, repair T-junctions, drop slivers", none,
        [this] { meshCleanupSelected(2); });
    add("clean.quads", "Triangles to quads", "Clean up", "Merge coplanar triangle pairs", none,
        [this] { meshCleanupSelected(3); });

    // --- Mesh (edit mode) ---
    add("mesh.vertex", "Vertex select", "Mesh", "Select vertices", A::SelectVertices,
        [this] { setSelMode(SelMode::Vertex); }, nullptr, [this] { return selMode_ == SelMode::Vertex; });
    add("mesh.edge", "Edge select", "Mesh", "Select edges (Alt+click: loop, Ctrl+Alt+click: ring)", A::SelectEdges,
        [this] { setSelMode(SelMode::Edge); }, nullptr, [this] { return selMode_ == SelMode::Edge; });
    add("mesh.face", "Face select", "Mesh", "Select faces", A::SelectFaces, [this] { setSelMode(SelMode::Face); },
        nullptr, [this] { return selMode_ == SelMode::Face; });
    add("mesh.more", "Select more", "Mesh", "Grow the selection by one ring", A::SelectMore,
        [this] { growSelection(true); }, editMode, nullptr, needEdit);
    add("mesh.less", "Select less", "Mesh", "Shrink the selection", A::SelectLess, [this] { growSelection(false); },
        editMode, nullptr, needEdit);
    add("mesh.loop", "Select edge loop", "Mesh", "The loop through the selected edge (Alt+click an edge)", none,
        [this] { selectLoopRing(false, false, false); }, editMode, nullptr, needEdit);
    add("mesh.ring", "Select edge ring", "Mesh", "The ring across the selected edge (Ctrl+Alt+click)", none,
        [this] { selectLoopRing(true, false, false); }, editMode, nullptr, needEdit);
    add("mesh.extrude", "Extrude", "Mesh", "Faces (or edges) grow out; move the mouse, click to place", A::Extrude,
        [this] { extrude(); });
    add("mesh.inset", "Inset", "Mesh", "A ring of faces inside the selected faces; then adjust width / depth / dish",
        A::Inset, [this] { insetSelected(true); }, editMode, nullptr, needEdit);
    add("mesh.bevel", "Bevel", "Mesh",
        "Round or chamfer the selected edges (vertex mode: corners). Mouse sets the width, wheel the segments",
        A::Bevel, [this] { bevelSelected(true); }, editMode, nullptr, needEdit);
    add("mesh.loopcut", "Loop cut", "Mesh", "Cut a new edge loop across the edge under the mouse (or the selected one)",
        A::LoopCut, [this] { loopCutAt(true); }, editMode, nullptr, needEdit);
    add("mesh.subdivide", "Subdivide edges", "Mesh", "Split the selected edges; faces with two cut edges are split",
        none, [this] { subdivideEdit(); }, editMode, nullptr, needEdit);
    add("mesh.connect", "Connect vertices", "Mesh", "Split faces between selected vertices (adds an edge)", A::Connect,
        [this] { connectSelected(); }, editMode, nullptr, needEdit);
    add("mesh.poke", "Poke faces", "Mesh", "A centre vertex in each selected face, with a fan of triangles", none,
        [this] { pokeSelected(); }, editMode, nullptr, needEdit);
    add("mesh.bridge", "Bridge", "Mesh",
        "Connect two edge loops with a tube, or two groups of faces with a tunnel (they are removed)", A::Bridge,
        [this] { bridgeSelected(); }, editMode, nullptr, needEdit);
    add("mesh.fill", "Fill", "Mesh", "Close the selected border loop with a face", A::Fill, [this] { fillSelected(); },
        editMode, nullptr, needEdit);
    add("mesh.push", "Push through", "Mesh",
        "Punch the selected faces through to the other side: a hole and a tunnel (set Inset for a frame)",
        A::PushThrough, [this] { pushThroughSelected(); }, editMode, nullptr, needEdit);
    add("mesh.merge", "Merge at centre", "Mesh", "Collapse the selected vertices into one", A::Merge,
        [this] { mergeSelected(false); }, editMode, nullptr, needEdit);
    add("mesh.collapse", "Collapse groups", "Mesh", "Each connected group of selected vertices becomes one vertex",
        none, [this] { mergeSelected(true); }, editMode, nullptr, needEdit);

    // --- UV / material ---
    static const uv::Method kMethods[6] = {uv::Method::Smart,       uv::Method::Box,       uv::Method::Planar,
                                           uv::Method::Cylindrical, uv::Method::Spherical, uv::Method::PerFace};
    static const char* const kUv[6] = {"Smart", "Box", "Planar", "Cylinder", "Sphere", "Per face"};
    for (int k = 0; k < 6; ++k)
        add(std::string("uv.") + lower(kUv[k]), std::string("Unwrap: ") + kUv[k], "UV",
            k == 0 ? "Charts by face direction, packed (U)" : "Project UVs; Edit mode: only the selected faces",
            k == 0 ? A::SmartUnwrap : none, [this, k] { unwrapActive(kMethods[k]); }, hasMesh, nullptr,
            "Select a mesh");
    static const char* const kUvTool[5] = {"Fit 0-1", "Pack islands", "Rotate 90", "Flip U", "Flip V"};
    for (int k = 0; k < 5; ++k)
        add(std::string("uv.tool") + std::to_string(k), std::string("UV: ") + kUvTool[k], "UV", "UV layout tool", none,
            [this, k] { uvTool(k); }, hasMesh, nullptr, "Select a mesh");
    add("uv.editor", "UV editor", "UV", "Show the UV layout; drag selected faces to move their UVs", none,
        [this] { uvEditor_ = !uvEditor_; }, nullptr, [this] { return uvEditor_; });
    auto preset = [this](int which) {
        Object* o = activeMesh();
        if (!o) return;
        beginEdit(uiHash("mat.preset", o->id));
        for (Object& x : scene_.objects) {
            if (!x.isMesh() || !(x.selected || &x == o)) continue;
            if (which == 0) x.transmission = 1, x.roughness = 0, x.metallic = 0, x.ior = 1.5f, x.opacity = 1;
            if (which == 1) x.metallic = 1, x.roughness = 0.25f, x.transmission = 0;
            if (which == 2) x.metallic = 0, x.roughness = 0.6f, x.transmission = 0, x.ior = 1.45f;
        }
    };
    add("mat.glass", "Material: glass", "Material", "Clear glass (path traced: refraction)", none,
        [preset] { preset(0); }, hasMesh, nullptr, "Select a mesh");
    add("mat.metal", "Material: metal", "Material", "Polished metal", none, [preset] { preset(1); }, hasMesh, nullptr,
        "Select a mesh");
    add("mat.matte", "Material: matte", "Material", "Rough, non-metal", none, [preset] { preset(2); }, hasMesh, nullptr,
        "Select a mesh");
    add("mat.folder", "Load texture folder...", "Material",
        "Pick any image of a texture set: color / normal / roughness ... are assigned by file name", none, [this] {
            Object* o = activeMesh();
            if (!o) return;
            std::string picked = platform::openFileDialog("Choose any image in the texture set", true);
            if (picked.empty()) return;
            size_t slash = picked.find_last_of("/\\");
            pbrFolder_ = slash == std::string::npos ? picked : picked.substr(0, slash);
            loadPbrFolder(*o, pbrFolder_);
        },
        hasMesh, nullptr, "Select a mesh");
    add("mat.reload", "Reload textures", "Material", "Read every texture from disk again", none, [this] {
        textureCache().clear();
        setStatus("Textures will be reloaded from disk");
    });

    // --- Rig / effects ---
    add("rig.bone", "Add bone", "Rig", "A new bone (root of a chain)", none, [this] { addBone(false); });
    add("rig.extrude", "Extrude bone", "Rig", "Grow the chain from the selected bone's tip", none,
        [this] { addBone(true); });
    add("rig.rest", "Rest pose", "Rig", "Reset every bone to the bind pose", none, [this] { resetPose(); });
    add("rig.bind", "Bind to skeleton", "Rig", "Select the mesh, then the root bone: automatic weights", none,
        [this] { bindSelected(); });
    add("rig.unbind", "Unbind", "Rig", "Remove the skin (keeps the current shape)", none, [this] { unbindSelected(); });
    add("rig.auto", "Automatic weights", "Rig", "Recompute the weights from the bones", none,
        [this] { recomputeWeights(); });
    add("rig.normalize", "Normalize weights", "Rig", "Make each vertex's weights add up to 1", none,
        [this] { normalizeWeights(); });
    add("rig.weights", "Weights view", "Rig", "Colour the mesh by one bone's influence", none,
        [this] { shading_ = shading_ == SHADE_WEIGHTS ? SHADE_STUDIO : SHADE_WEIGHTS; }, nullptr,
        [this] { return shading_ == SHADE_WEIGHTS; });
    add("rig.assign", "Assign weight", "Rig", "Give the selected vertices the weight value for the shown bone", none,
        [this] { assignWeight(false); }, editMode, nullptr, needEdit);
    add("rig.remove", "Remove weight", "Rig", "Remove the shown bone's weight from the selected vertices", none,
        [this] { assignWeight(true); }, editMode, nullptr, needEdit);
    add("fx.play", "Play / pause effects", "Effects", "Run the particle simulation", A::PlayPause,
        [this] { togglePlay(); }, nullptr, [this] { return playing_; });
    add("fx.restart", "Restart effects", "Effects", "Clear and restart every emitter", none,
        [this] { restartParticles(); });

    // --- Light / render ---
    add("light.lit", "Lit view", "Light", "Show the scene's lamps in the viewport", none,
        [this] { shading_ = shading_ == SHADE_LIT ? SHADE_STUDIO : SHADE_LIT; }, nullptr,
        [this] { return shading_ == SHADE_LIT; });
    add("render.view", "Render view", "Render", "Path-trace the viewport (progressive)", A::Render,
        [this] { toggleRenderView(); }, nullptr, [this] { return renderView_; });
    add("render.save", "Save render", "Render", "Save the rendered image as <file>_render.png (+ a .txt report)", none,
        [this] {
            std::string base = fileField_;
            size_t dot = base.find_last_of('.');
            size_t slash = base.find_last_of("/\\");
            if (dot != std::string::npos && (slash == std::string::npos || slash < dot)) base = base.substr(0, dot);
            saveRender(base + "_render.png");
        });
    add("render.gpu", "Render device: GPU", "Render", "OpenGL fragment-shader path tracer", none,
        [this] {
            if (gpuTracerOk_) renderSet_.device = DEV_GPU;
            else setStatus("GPU tracer unavailable: " + gpuTracerError_, true);
        },
        nullptr, [this] { return renderSet_.device == DEV_GPU; });
    add("render.cpu", "Render device: CPU", "Render", "Multithreaded CPU path tracer", none,
        [this] { renderSet_.device = DEV_CPU; }, nullptr, [this] { return renderSet_.device == DEV_CPU; });
    add("render.rtx", "Render device: RTX", "Render", "Hardware ray tracing (DXR 1.1 / RT cores)", none,
        [this] {
            renderSet_.device = DEV_RTX;
            if (!hwrtInfo_.available && !renderSet_.allowWarp)
                setStatus("No ray-tracing GPU (DXR 1.1) found. Enable 'Software DXR' to test the RTX path on WARP.", true);
        },
        nullptr, [this] { return renderSet_.device == DEV_RTX; });
    add("render.live", "Live render", "Render", "Restart the render when the scene or view changes", none,
        [this] { renderAutoUpdate_ = !renderAutoUpdate_; }, nullptr, [this] { return renderAutoUpdate_; });

    // --- Settings ---
    add("set.trackpad", "Trackpad navigation", "Settings",
        "Two-finger scroll orbits, Shift+scroll pans, pinch or Ctrl+scroll zooms", none,
        [this] {
            access_.trackpad = !access_.trackpad;
            configDirty_ = true;
            setStatus(access_.trackpad ? "Trackpad navigation: two fingers orbit, Shift pans, pinch zooms"
                                       : "Mouse navigation: the wheel zooms");
        },
        nullptr, [this] { return access_.trackpad; });
    add("set.tooltips", "Tooltips", "Settings", "Show what a button does when the mouse rests on it", none,
        [this] {
            access_.tooltips = !access_.tooltips;
            configDirty_ = true;
        },
        nullptr, [this] { return access_.tooltips; });
    add("set.navpads", "Navigation pads", "Settings", "Orbit / pan / zoom pads under the scene gizmo (drag them)", none,
        [this] {
            access_.navButtons = !access_.navButtons;
            configDirty_ = true;
        },
        nullptr, [this] { return access_.navButtons; });
    add("set.bigger", "UI larger", "Settings", "Bigger text and buttons", none, [this] {
        access_.uiScale = std::min(6, (access_.uiScale ? access_.uiScale : fontScale_) + 1);
        applyUiScale();
        configDirty_ = true;
    });
    add("set.smaller", "UI smaller", "Settings", "Smaller text and buttons (more room)", none, [this] {
        access_.uiScale = std::max(1, (access_.uiScale ? access_.uiScale : fontScale_) - 1);
        applyUiScale();
        configDirty_ = true;
    });
    add("set.unity", "Keymap: Unity", "Settings", "Q W E R Y tools, Alt+LMB orbit, RMB fly", none,
        [this] { setKeymap(Keymap::Unity); }, nullptr, [this] { return keymap_ == Keymap::Unity; });
    add("set.blender", "Keymap: Blender", "Settings", "G R S modal transforms, MMB / RMB orbit", none,
        [this] { setKeymap(Keymap::Blender); }, nullptr, [this] { return keymap_ == Keymap::Blender; });
}

const Editor::Command* Editor::command(const std::string& id) const {
    auto it = commandIndex_.find(id);
    return it == commandIndex_.end() ? nullptr : &commands_[it->second];
}

bool Editor::runCommand(const std::string& id, bool fromPalette) {
    const Command* c = command(id);
    if (!c) return false;
    if (xf_ != Xform::None || insetModal_) {
        setStatus("Finish the current operation first (click / Enter, or Esc)", true);
        return false;
    }
    if (c->available && !c->available()) {
        setStatus(c->label + ": " + (c->why.empty() ? std::string("not available now") : c->why), true);
        return false;
    }
    if (fromPalette) {
        recentCommands_.erase(std::remove(recentCommands_.begin(), recentCommands_.end(), id), recentCommands_.end());
        recentCommands_.insert(recentCommands_.begin(), id);
        if (recentCommands_.size() > 8) recentCommands_.pop_back();
    }
    c->run();
    return true;
}

std::string Editor::shortcutText(int action) const {
    if (action < 0 || action >= input::kActionCount) return "";
    std::string s;
    for (int k = 0; k < 2; ++k) {
        const input::Binding& b = keys_.slots[action][k];
        if (!b.bound()) continue;
        if (!s.empty()) s += " / ";
        s += input::toString(b);
    }
    return s;
}

bool Editor::cmdButton(const char* id, const Rect& r, const char* label) {
    const Command* c = command(id);
    if (!c) {
        ui_.button(uiHash(id), r, "?");
        return false;
    }
    const bool on = c->checked && c->checked();
    const bool avail = !c->available || c->available();
    const bool clicked = ui_.button(uiHash("cmd", uiHash(id)), r, label ? label : c->label, on, !avail);
    std::string tip = c->label;
    std::string keys = shortcutText(c->action);
    if (!keys.empty()) tip += "   (" + keys + ")";
    tip += "\n" + c->hint;
    if (!avail && !c->why.empty()) tip += "\n" + c->why;
    ui_.tooltip(tip);
    if (clicked) runCommand(id);
    return clicked;
}

void Editor::applyUiScale() {
    fontScale_ = access_.uiScale > 0 ? access_.uiScale : std::max(1, (int)(dpi_ * 2.0f + 0.25f));
    ui_.tooltipsEnabled = access_.tooltips;
}

// ============================================================================
// Workspaces
// ============================================================================
void Editor::setWorkspace(Workspace w) {
    if (w == ws_) return;
    if (xf_ != Xform::None) endTransform(true);
    wsShading_[(int)ws_] = shading_;
    const Workspace from = ws_;
    ws_ = w;
    settingsOpen_ = false;
    leftScroll_ = 0;
    shading_ = wsShading_[(int)w];
    // Edit mode belongs to modeling / texturing / weight painting.
    if ((w == Workspace::Light || w == Workspace::Render) && mode_ == Mode::Edit) setMode(Mode::Object);
    if (w == Workspace::Texture) uvEditor_ = true;
    else if (from == Workspace::Texture) uvEditor_ = false;
    if (w == Workspace::Render && !renderView_) toggleRenderView();
    else if (from == Workspace::Render && renderView_) toggleRenderView();
    propsTab_ = w == Workspace::Texture ? 1 : (w == Workspace::Light ? 2 : 0);
    setStatus(kWorkspaceHints[(int)w]);
}

// ============================================================================
// Command palette
// ============================================================================
void Editor::openPalette() {
    paletteOpen_ = true;
    paletteQuery_.clear();
    paletteSel_ = 0;
    paletteScroll_ = 0;
    openMenu_ = -1;
}

void Editor::buildPalette(const Input& in) {
    const float fs = (float)fontScale_, rowH = ui_.rowHeight(), pad = 5 * fs;
    // Matches: every word of the query in "category label id".
    std::vector<int> matches;
    std::vector<std::string> words;
    {
        std::string q = lower(paletteQuery_), w;
        for (char c : q + " ") {
            if (c == ' ') {
                if (!w.empty()) words.push_back(w);
                w.clear();
            } else {
                w += c;
            }
        }
    }
    if (words.empty()) {
        for (const std::string& id : recentCommands_)
            if (commandIndex_.count(id)) matches.push_back(commandIndex_[id]);
    }
    std::vector<std::pair<int, int>> scored;  // (score, index)
    for (int i = 0; i < (int)commands_.size(); ++i) {
        if (std::find(matches.begin(), matches.end(), i) != matches.end()) continue;
        const Command& c = commands_[i];
        std::string hay = lower(c.category + " " + c.label + " " + c.id), label = lower(c.label);
        int score = 0;
        bool all = true;
        for (const std::string& w : words) {
            size_t p = hay.find(w);
            if (p == std::string::npos) {
                all = false;
                break;
            }
            if (label.rfind(w, 0) == 0) score += 4;
            else if (label.find(" " + w) != std::string::npos) score += 2;
            else if (label.find(w) != std::string::npos) score += 1;
        }
        if (!all) continue;
        if (c.available && !c.available()) score -= 3;
        scored.push_back({-score, i});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& s : scored) matches.push_back(s.second);

    const int shown = std::min(12, (int)matches.size());
    const float w = std::min((float)screenW_ - 20 * fs, 260.0f * fs);
    const float h = pad * 2 + rowH * 1.3f + std::max(1, shown) * rowH + 12 * fs;
    paletteRect_ = {std::floor((screenW_ - w) * 0.5f), std::floor(topBar_.h + 8 * fs), w, h};
    ui_.rect({0, 0, (float)screenW_, (float)screenH_}, {0, 0, 0, 0.35f});
    ui_.rect(paletteRect_, theme::panel);
    ui_.border(paletteRect_, theme::accent, fs);
    Rect field{paletteRect_.x + pad, paletteRect_.y + pad, w - 2 * pad, rowH * 1.1f};
    // Keyboard navigation: arrows, Ctrl+J/K or Ctrl+N/P (no arrow keys on 60% boards), PgUp/PgDn.
    const int count = (int)matches.size();
    auto move = [&](int d) {
        if (count) paletteSel_ = ((paletteSel_ + d) % count + count) % count;
    };
    if (in.keyRepeat[KEY_DOWN] || (in.ctrl() && (in.keyRepeat['J'] || in.keyRepeat['N'])) || in.keyRepeat[KEY_TAB]) move(1);
    if (in.keyRepeat[KEY_UP] || (in.ctrl() && (in.keyRepeat['K'] || in.keyRepeat['P']))) move(-1);
    if (in.keyRepeat[KEY_PAGEDOWN]) move(std::min(10, std::max(1, count - 1 - paletteSel_)));
    if (in.keyRepeat[KEY_PAGEUP]) move(-std::min(10, paletteSel_));
    std::string before = paletteQuery_;
    int r = ui_.searchField(uiHash("palette.field"), field, paletteQuery_);
    if (paletteQuery_ != before) paletteSel_ = 0;
    paletteSel_ = count ? std::max(0, std::min(paletteSel_, count - 1)) : 0;
    int first = std::max(0, std::min(paletteSel_ - shown + 1, count - shown));
    first = std::max(0, std::min(first, paletteSel_));
    float y = field.y + field.h + 4 * fs;
    int clicked = -1;
    for (int k = 0; k < shown; ++k) {
        const int idx = matches[first + k];
        const Command& c = commands_[idx];
        Rect row{paletteRect_.x + pad, y, w - 2 * pad, rowH};
        const bool sel = first + k == paletteSel_;
        const bool avail = !c.available || c.available();
        if (ui_.selectable(uiHash("palette.row", (uint32_t)idx), row, "", sel, false)) clicked = first + k;
        if (sel) ui_.rect(row, withAlpha(theme::accent, 0.55f));
        std::string keys = shortcutText(c.action);
        float kw = keys.empty() ? 0 : ui_.textWidth(keys) + 6 * fs;
        Color tc = avail ? theme::white : theme::textDim;
        ui_.textIn({row.x, row.y, row.w - kw, row.h}, c.label + (c.checked && c.checked() ? "  [on]" : ""), tc, false);
        ui_.textIn({row.x, row.y, row.w - kw - 2 * fs, row.h}, "", tc, false);
        if (!keys.empty()) ui_.textIn({row.x + row.w - kw, row.y, kw, row.h}, keys, theme::textDim, false);
        y += rowH;
    }
    if (count == 0) ui_.textIn({paletteRect_.x + pad, y, w - 2 * pad, rowH}, "No command matches", theme::textDim, false);
    const std::string help = count ? commands_[matches[paletteSel_]].category + ": " + commands_[matches[paletteSel_]].hint
                                   : std::string("Type part of a name, e.g. \"bev\", \"add cube\", \"render\"");
    ui_.textIn({paletteRect_.x + pad, paletteRect_.y + paletteRect_.h - pad - 9 * fs, w - 2 * pad, 9 * fs}, help,
               theme::textDim, false);
    if (clicked >= 0) {
        paletteSel_ = clicked;
        r = 1;
    }
    if (r == -1 || (in.mousePressed[MOUSE_LEFT] && !paletteRect_.contains(in.mouseX, in.mouseY))) {
        paletteOpen_ = false;
        ui_.cancelEdit();
    } else if (r == 1) {
        paletteOpen_ = false;
        ui_.cancelEdit();
        if (count) runCommand(commands_[matches[paletteSel_]].id, true);
    }
}

// ============================================================================
// Top bar: menus | pipeline workspaces | search, settings, help
// ============================================================================
void Editor::buildTopBar(const Input& in) {
    (void)in;
    const float fs = (float)fontScale_, gap = 3 * fs, pad = 3 * fs, h = ui_.rowHeight();
    ui_.rect(topBar_, theme::panelDark);
    ui_.rect({topBar_.x, topBar_.y + topBar_.h - 1, topBar_.w, 1}, theme::border);
    float x = topBar_.x + pad;
    const float y = topBar_.y + pad;
    static const char* const kMenus[4] = {"File", "Edit", "View", "Add"};
    for (int m = 0; m < 4; ++m) {
        float w = ui_.textWidth(kMenus[m]) + 10 * fs;
        menuButtons_[m] = {x, y, w, h};
        if (ui_.button(uiHash("menu", (uint32_t)m), menuButtons_[m], kMenus[m], openMenu_ == m))
            openMenu_ = openMenu_ == m ? -1 : m;
        else if (openMenu_ >= 0 && openMenu_ != m && ui_.hovered(menuButtons_[m]))
            openMenu_ = m;  // slide between open menus
        x += w + gap;
    }
    // Workspaces, in pipeline order.
    x += 6 * fs;
    for (int w = 0; w < kWorkspaceCount; ++w) {
        std::string label = std::to_string(w + 1) + " " + kWorkspaceNames[w];
        float bw = ui_.textWidth(label) + 10 * fs;
        std::string id = std::string("ws.") + lower(kWorkspaceNames[w]);
        cmdButton(id.c_str(), {x, y, bw, h}, label.c_str());
        x += bw + (w + 1 < kWorkspaceCount ? fs : 0);
        if (w + 1 < kWorkspaceCount) {
            ui_.text(x, y + (h - ui_.glyphH()) * 0.5f, ">", theme::textDim);
            x += ui_.textWidth(">") + fs;
        }
    }
    // Right side.
    float rx = topBar_.x + topBar_.w - pad;
    auto rightButton = [&](const char* id, const char* label) {
        float bw = ui_.textWidth(label) + 10 * fs;
        rx -= bw;
        cmdButton(id, {rx, y, bw, h}, label);
        rx -= gap;
    };
    rightButton("view.help", "?");
    rightButton("edit.settings", "Settings");
    std::string search = "Search " + shortcutText((int)input::Action::Palette);
    if (rx - (ui_.textWidth(search) + 10 * fs) < x + 4 * fs) search = "Search";
    rightButton("edit.palette", search.c_str());
}

void Editor::buildMenu(const Input& in) {
    if (openMenu_ < 0) return;
    static const std::vector<std::vector<std::string>> kItems = {
        {"file.new", "file.open", "file.save", "-", "file.import", "file.export", "-", "file.screenshot", "-", "file.quit"},
        {"edit.undo", "edit.redo", "-", "edit.selectall", "edit.duplicate", "edit.delete", "edit.next", "edit.prev", "-",
         "obj.join", "obj.parent", "obj.unparent", "-", "edit.palette", "edit.settings"},
        {"view.front", "view.right", "view.top", "view.back", "view.left", "view.bottom", "view.ortho", "view.frame", "-",
         "view.shade.studio", "view.shade.lit", "view.shade.checker", "view.shade.weights", "-", "view.wire",
         "view.grid", "uv.editor", "view.stats", "-", "view.fly", "view.camera", "view.camtoview", "-", "view.help"},
        {"add.cube", "add.sphere", "add.cylinder", "add.cone", "add.plane", "add.torus", "add.stairs", "add.gear",
         "add.pipe", "add.spring", "-", "add.empty", "add.bone", "add.emitter", "-", "add.point", "add.sun", "add.spot",
         "add.area", "add.camera"},
    };
    const float fs = (float)fontScale_, rowH = ui_.rowHeight(), pad = 3 * fs;
    const auto& items = kItems[openMenu_];
    float w = 0;
    for (const std::string& id : items) {
        const Command* c = command(id);
        if (!c) continue;
        std::string keys = shortcutText(c->action);
        w = std::max(w, ui_.textWidth(c->label) + ui_.textWidth(keys) + 34 * fs);
    }
    float h = 2 * pad;
    for (const std::string& id : items) h += id == "-" ? 4 * fs : rowH;
    const float fieldH = openMenu_ == 0 ? rowH + 2 * pad + 9 * fs : 0;  // File: the file name field
    h += fieldH;
    Rect b = menuButtons_[openMenu_];
    menuRect_ = {b.x, b.y + b.h + fs, w, h};
    if (menuRect_.y + menuRect_.h > screenH_) menuRect_.h = screenH_ - menuRect_.y;
    ui_.rect(menuRect_, theme::panel);
    ui_.border(menuRect_, theme::border, fs);
    float y = menuRect_.y + pad;
    std::string run;
    for (const std::string& id : items) {
        if (id == "-") {
            ui_.rect({menuRect_.x + pad, y + 1.5f * fs, w - 2 * pad, (float)std::max(1, (int)fs / 2)}, theme::border);
            y += 4 * fs;
            continue;
        }
        const Command* c = command(id);
        if (!c) continue;
        Rect row{menuRect_.x + pad, y, w - 2 * pad, rowH};
        const bool avail = !c->available || c->available();
        const bool on = c->checked && c->checked();
        if (ui_.selectable(uiHash("menu.item", uiHash(id.c_str())), row, "", false, false)) run = id;
        ui_.textIn({row.x + 6 * fs, row.y, row.w, row.h}, c->label, avail ? theme::text : theme::textDim, false);
        if (on) ui_.text(row.x + fs, row.y + (rowH - ui_.glyphH()) * 0.5f, "*", theme::accent);
        std::string keys = shortcutText(c->action);
        if (!keys.empty())
            ui_.textIn({row.x + row.w - ui_.textWidth(keys) - 4 * fs, row.y, ui_.textWidth(keys) + 4 * fs, row.h}, keys,
                       theme::textDim, false);
        ui_.tooltip(c->hint);
        y += rowH;
    }
    // The file name used by Save / Export lives in the File menu.
    if (openMenu_ == 0) {
        ui_.text(menuRect_.x + pad + 2 * fs, y + pad, "File name (Save / Export):", theme::textDim);
        ui_.textField(uiHash("file.name"), {menuRect_.x + pad, y + pad + 9 * fs, w - 2 * pad, rowH}, fileField_);
        ui_.tooltip("Name used by Save, Export OBJ and Save render (.m3d / .obj added)");
    }
    if (!run.empty()) {
        openMenu_ = -1;
        runCommand(run);
    } else if (in.mousePressed[MOUSE_LEFT] && !menuRect_.contains(in.mouseX, in.mouseY) &&
               !menuButtons_[openMenu_].contains(in.mouseX, in.mouseY)) {
        bool onOther = false;
        for (const Rect& mb : menuButtons_) onOther = onOther || mb.contains(in.mouseX, in.mouseY);
        if (!onOther) openMenu_ = -1;
    } else if (in.pressed(KEY_ESCAPE)) {
        openMenu_ = -1;
    }
}

// ============================================================================
// Settings page (left panel): Input, Navigation, Keys
// ============================================================================
void Editor::buildSettings(PanelLayout& L, const Input& in) {
    (void)in;
    const float fs = (float)fontScale_, gap = 3 * fs, rowH = ui_.rowHeight();
    auto note = [&](const char* text) { label(L, text, theme::textDim); };
    Rect title = L.row(rowH);
    ui_.text(title.x, title.y + (rowH - ui_.glyphH()) * 0.5f, "SETTINGS", theme::accent);
    if (ui_.button(uiHash("settings.close"), {title.x + title.w - 30 * fs, title.y, 30 * fs, rowH}, "Done"))
        settingsOpen_ = false;
    static const char* const kTabs[3] = {"Input", "Nav", "Keys"};
    Rect row = L.row(rowH);
    for (int t = 0; t < 3; ++t)
        if (ui_.button(uiHash("settings.tab", (uint32_t)t), cell(row, t, 3, gap), kTabs[t], settingsTab_ == t))
            settingsTab_ = t, leftScroll_ = 0;
    L.y += gap;
    if (settingsTab_ == 0) {
        if (section(L, "set.pointer", "POINTER")) {
            cmdButton("set.trackpad", L.row(rowH), "Trackpad navigation");
            note(access_.trackpad ? "2 fingers: orbit" : "Wheel: zoom");
            note(access_.trackpad ? "Shift+2 fingers: pan" : "Alt+LMB orbit,");
            note(access_.trackpad ? "Pinch / Ctrl: zoom" : "Alt+Ctrl+LMB pan,");
            if (!access_.trackpad) note("Alt+Shift+LMB zoom");
            cmdButton("set.navpads", L.row(rowH), "Navigation pads");
            note("Pan / Zoom pads under");
            note("the axis gizmo: drag");
            note("them. No middle or");
            note("right button needed.");
        }
        if (section(L, "set.display", "DISPLAY")) {
            row = L.row(rowH);
            cmdButton("set.smaller", cell(row, 0, 3, gap), "A-");
            ui_.textIn(cell(row, 1, 3, gap), strf("Size %d", fontScale_), theme::text, true);
            cmdButton("set.bigger", cell(row, 2, 3, gap), "A+");
            if (access_.uiScale && ui_.button(uiHash("set.autoscale"), L.row(rowH), "Automatic size")) {
                access_.uiScale = 0;
                applyUiScale();
                configDirty_ = true;
            }
            cmdButton("set.tooltips", L.row(rowH), "Tooltips");
        }
        if (section(L, "set.keyboard", "KEYBOARD")) {
            note("All commands: Ctrl+K");
            note("or Shift+Space, type,");
            note("Enter. Up/Down or");
            note("Ctrl+J/K pick a line.");
            note("No F-keys needed:");
            note("help S-/, stats C-I,");
            note("render C-S-R.");
            note("Alt+arrows: orbit");
            note("= and -: zoom");
            note("[ and ]: pick objects");
            note("Shift+F: fly mode");
            note("G/R/S take numbers:");
            note("G X 2 Enter.");
        }
    } else if (settingsTab_ == 1) {
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
        if (section(L, "nav.look", "LOOK AROUND")) {
            setRow("Orbit", "camset.orbit", c.orbitSensitivity, 0.002f, 0.01f, 5.0f);
            setRow("Mouse look", "camset.look", c.lookSensitivity, 0.002f, 0.01f, 5.0f);
            row = L.row(rowH);
            if (ui_.button(uiHash("camset.invx"), cell(row, 0, 2, gap), "Invert X", c.invertX))
                c.invertX = !c.invertX, configDirty_ = true;
            if (ui_.button(uiHash("camset.invy"), cell(row, 1, 2, gap), "Invert Y", c.invertY))
                c.invertY = !c.invertY, configDirty_ = true;
        }
        if (section(L, "nav.move", "MOVE")) {
            setRow("Pan speed", "camset.pan", c.panSpeed, 0.01f, 0.05f, 20.0f);
            setRow("Zoom speed", "camset.zoom", c.zoomSpeed, 0.01f, 0.05f, 20.0f);
            setRow("Fly speed", "camset.fly", flySpeed_, 0.01f, 0.01f, 100.0f);
            setRow("Arrow keys", "camset.arrow", c.arrowSpeed, 0.01f, 0.05f, 20.0f);
            setRow("Shift x", "camset.fast", c.fastMultiplier, 0.02f, 1.0f, 20.0f);
            if (ui_.button(uiHash("camset.accel"), L.row(rowH), "Fly acceleration", c.flyAcceleration))
                c.flyAcceleration = !c.flyAcceleration, configDirty_ = true;
        }
        if (section(L, "nav.view", "VIEW")) {
            setRow("FOV", "camset.fov", cam_.fovY, 0.2f, 10.0f, 120.0f);
            setRow("Transition", "camset.trans", c.transition, 0.005f, 0.0f, 3.0f);
            if (ui_.button(uiHash("camset.reset"), L.row(rowH), "Reset camera")) {
                c = CameraSettings();
                flySpeed_ = 1.0f;
                cam_.fovY = 50.0f;
                configDirty_ = true;
                setStatus("Camera settings reset");
            }
        }
    } else {
        row = L.row(rowH);
        cmdButton("set.unity", cell(row, 0, 2, gap), "Unity");
        cmdButton("set.blender", cell(row, 1, 2, gap), "Blender");
        note("Click a slot, press");
        note("keys. Esc cancels,");
        note("Backspace clears.");
        {
            Rect fr = L.row(rowH);
            float lw = std::floor(fr.w * 0.3f);
            ui_.textIn({fr.x, fr.y, lw, fr.h}, "Find", theme::textDim, false);
            ui_.textField(uiHash("keys.filter"), {fr.x + lw, fr.y, fr.w - lw, fr.h}, keysFilter_);
        }
        const char* const kContextNames[4] = {"SHORTCUTS", "FLY (RMB HELD)", "TRANSFORM (MODAL)", "EDIT MODE"};
        int lastContext = -1;
        std::string filter = lower(keysFilter_);
        for (int a = 0; a < input::kActionCount; ++a) {
            const input::ActionInfo& inf = input::info((input::Action)a);
            if (!filter.empty() && lower(inf.label).find(filter) == std::string::npos) continue;
            if ((int)inf.context != lastContext) {
                lastContext = (int)inf.context;
                ui_.header(L.row(11 * fs), kContextNames[lastContext]);
            }
            Rect r = L.row(rowH);
            float lw = std::floor(r.w * 0.47f), sw = std::floor((r.w - lw - gap) * 0.5f);
            ui_.textIn({r.x, r.y, lw, r.h}, ui_.fitText(inf.label, lw), theme::textDim, false);
            for (int slot = 0; slot < 2; ++slot) {
                Rect sr{r.x + lw + slot * (sw + gap), r.y, slot == 0 ? sw : r.w - lw - sw - gap, r.h};
                const bool capturing = captureAction_ == a && captureSlot_ == slot;
                std::string text = capturing ? "press..." : input::toString(keys_.slots[a][slot]);
                if (ui_.button(uiHash("keys.slot", (uint32_t)(a * 2 + slot)), sr, ui_.fitText(text, sr.w - 2), capturing)) {
                    if (capturing) {
                        captureAction_ = -1;
                    } else {
                        captureAction_ = a;
                        captureSlot_ = slot;
                        setStatus(std::string("Press a key (with Ctrl/Shift/Alt) for \"") + inf.label +
                                  "\" - Esc cancels, Backspace clears");
                    }
                }
            }
        }
        if (ui_.button(uiHash("keys.reset"), L.row(rowH), "Reset to preset")) setKeymap(keymap_);
    }
}

bool Editor::section(PanelLayout& L, const char* id, const char* title, bool defaultOpen) {
    const uint32_t h = uiHash("section", uiHash(id));
    auto it = sectionOpen_.find(h);
    bool open = it == sectionOpen_.end() ? defaultOpen : it->second;
    if (ui_.collapsingHeader(h, L.row(11.0f * fontScale_), title, open)) {
        open = !open;
        sectionOpen_[h] = open;
    }
    return open;
}
