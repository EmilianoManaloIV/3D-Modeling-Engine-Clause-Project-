#pragma once
// The modeling application: owns the scene, camera, tools, undo history and
// UI. One call to frame() per iteration of the main loop (GEA Vol. I ch. 8).
//
// Implementation is split by concern:
//   editor.cpp         frame loop, input, selection, transform tool, undo
//   editor_actions.cpp commands (create, mesh/UV/rig tools, hierarchy, files)
//   editor_ui.cpp      panels, viewport header, UV editor, dialogs
//   editor_render.cpp  3D viewport drawing, gizmos, lights, particles
#include "bvh.h"
#include "csg.h"
#include "gpu_tracer.h"
#include "hwrt.h"
#include "pathtracer.h"
#include "input_map.h"
#include "meshedit.h"
#include "particles.h"
#include "platform.h"
#include "renderer.h"
#include "scene.h"
#include "ui.h"
#include "uv.h"

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct AppOptions {
    std::string openPath;
    int demo = 0;  // 0 = default scene, 1..4 = sample scenes (for screenshots)
    bool showHelp = false;
    std::string benchmarkReport;  // non-empty: run the performance benchmark and exit
    std::vector<int> benchmarkOnly;  // scenario numbers (1-based) to run; empty = all, plus the one-shot timings
    std::string configPath;       // settings file (keymap); empty = don't persist
    // --render <out.png>: path-trace the scene, save it (+ a .txt report) and exit.
    std::string renderOut;
    std::string renderDevice;     // "cpu" / "gpu" / "rtx" (default: RTX, else GPU, if available)
    bool allowWarp = false;       // --rt-warp: let the RTX device use software DXR (WARP)
    int renderSamples = 0;        // 0 = Render tab default
    int renderPercent = 0;        // resolution % (0 = default)
};

// Handles of the Unity-style transform gizmo.
enum GizmoHandle {
    GH_None = 0,
    GH_MoveX, GH_MoveY, GH_MoveZ,     // arrows
    GH_MoveYZ, GH_MoveXZ, GH_MoveXY,  // plane squares (normal = X, Y, Z)
    GH_MoveFree,                      // centre square: move in the view plane
    GH_RotX, GH_RotY, GH_RotZ,        // rings
    GH_RotView,                       // outer ring: rotate around the view axis
    GH_RotFree,                       // inside the rings: trackball
    GH_ScaleX, GH_ScaleY, GH_ScaleZ,  // axis cubes
    GH_ScaleUniform,                  // centre cube
};

// Orbit camera around a target point (FoCG 5e ch. 8).
struct Camera {
    Vec3 target{0, 0, 0};
    float yaw = 35.0f;    // degrees around +Y
    float pitch = 25.0f;  // degrees above the XZ plane
    float distance = 9.0f;
    float fovY = 50.0f;
    bool ortho = false;

    Vec3 eye() const;
    Mat4 view() const;
    Mat4 projection(float aspect) const;
    float orthoHalfHeight() const;
};

// Vertical stacking of UI rows inside a panel.
struct PanelLayout {
    float x, y, w, gap;
    Rect row(float h) {
        Rect r{x, y, w, h};
        y += h + gap;
        return r;
    }
};
Rect cell(const Rect& r, int i, int n, float gap);
std::string strf(const char* fmt, ...);

class Editor {
public:
    bool init(const AppOptions& options, std::string& error);
    void shutdown();
    void frame(const Input& in, int width, int height, float dt);
    void requestQuit();
    bool shouldExit() const { return exit_; }
    void readPixels(int w, int h, std::vector<uint8_t>& rgb) { renderer_.readPixels(w, h, rgb); }
    // Automation (scripts / stress tests).
    bool runNamedCommand(const std::string& name, std::string& matched);  // like the command search
    bool checkInvariants(std::string& error) const;  // scene, meshes, selection and camera are sane
    std::string summary() const;                      // one line: mode, objects, active mesh size, status
    const std::string& statusText() const { return status_; }
    // Random selection + a random command (stress testing). Returns its name.
    std::string runRandomCommand(uint32_t seed);

private:
    enum class Mode { Object, Edit };
    enum class Xform { None, Grab, Rotate, Scale };
    enum class Nav { None, Orbit, Pan, Zoom, Fly };
    enum class Tool { Hand, Move, Rotate, Scale, Universal };  // Unity's Q W E R Y
    enum class Keymap { Unity, Blender };
    // One transform step relative to the originals captured by beginTransform():
    // scale (per axis of `basis`) about the pivot, then rotation about the
    // pivot, then translation.
    struct XfDelta {
        Vec3 move;
        Vec3 rotAxis{0, 1, 0};
        float rotDeg = 0;
        Vec3 scale{1, 1, 1};
        Mat4 basis;  // columns = scale axes
    };
    // Undo snapshot. Meshes are shared between snapshots (and with the live
    // scene's last saved state) by content stamp, so an edit only copies the
    // meshes it changed - not every mesh in the scene.
    struct Snapshot {
        std::vector<Object> objects;  // with empty meshes; the data is in `meshes`
        std::vector<std::shared_ptr<const Mesh>> meshes;
        int active;
        std::vector<char> vsel;
        Vec3 ambient;
    };

    // --- actions (editor_actions.cpp) ---
    int addObject(Object o, const char* status);
    void addShape(int shape);
    void addLight(LightType type);
    void addEmpty();
    void addBone(bool asChildOfActive);
    void addEmitter();
    void addCamera();
    void reportTexture(const std::string& path);
    void handleDroppedFiles(const std::vector<std::string>& files);
    void loadPbrFolder(Object& o, const std::string& folder);         // assign maps found in a folder by name
    int assignTextureByName(Object& o, const std::string& file);      // slot used, or -1
    std::string pbrFolder_;
    void alignCameraToView(Object& camera);  // camera object <- the editor view (position, direction, FOV)
    void lookThroughCamera();                // editor view <- the render camera
    void alignActiveCameraToView();
    void duplicateSelected();
    void deleteSelected();
    void subdivideSelected();
    void flipSelected();
    // --- path-traced rendering (editor_renderview.cpp) ---
    void initGpuInfo();
    void shutdownRender();
    void toggleRenderView();
    void startRender();
    void stopRender();
    void updateRender();
    void presentRender(int vx, int vy, int vw, int vh);
    bool saveRender(const std::string& path);
    uint64_t renderStamp() const;
    void renderSize(int& w, int& h) const;
    int renderSamples() const;
    bool renderRunning() const;
    std::vector<std::string> gpuReport() const;
    void processKeyCapture(const Input& in);
    void booleanSelected(csg::Op op);   // editor_meshops.cpp
    void meshCleanupSelected(int tool); // 0 n-gons->tris, 1 all->tris, 2 clean up, 3 tris->quads
    void setSmoothSelected(bool smooth);
    void selectAll();
    void extrude();
    // --- edit mode: selection modes, edge extrude, inset (editor_editmode.cpp) ---
    enum class SelMode { Vertex, Edge, Face };
    void setSelMode(SelMode m);
    void syncEditSelection();
    void selectionFromVertices();
    void verticesFromSelection();
    std::vector<int> editFaceList();
    int countEditSelection();
    int pickEdge(Vec2 p, meshedit::Edge& out);
    int pickFace(Vec2 p);
    void clickSelectEdit(Vec2 p, bool extend);
    void boxSelectEdit(Vec2 a, Vec2 b, bool extend, bool subtract);
    void selectAllEdit();
    void extrudeEdit();
    void deleteEdit();
    void insetSelected(bool interactive);
    // --- topology tools with an adjustable "last operation" (editor_meshtools.cpp) ---
    enum class OpType { None, Inset, Bevel, LoopCut, Subdivide, Bridge, Push, Poke };
    bool startMeshOp(OpType type, bool interactive);
    void applyLastOp();
    bool lastOpAdjustable();
    bool updateOpModal(const Input& in);
    void lastOpPanel(PanelLayout& L);
    void setEditResult(const std::vector<int>* faces, const std::vector<meshedit::Edge>* edges);
    std::vector<meshedit::Edge> editEdgeList();  // selected edges in any select mode
    void connectSelected();
    void fillSelected();
    void mergeSelected();
    void selectLoop(bool ring);
    void selectLinked();
    void joinSelected();
    static const char* opName(OpType t);
    const MeshAccel& meshAccel(const Object& o);
    void setMode(Mode m);
    void frameSelected();
    void setView(float yaw, float pitch);
    void animateCameraTo(const Camera& goal);  // eased transition (Unity scene-camera style)
    void updateCameraAnimation(float dt);
    int pickSceneGizmo(Vec2 windowPos);        // 0..5 = +X -X +Y -Y +Z -Z, 6 = projection toggle, -1 none
    void sceneGizmoLayout(Vec2& center, float& length) const;
    void clickSceneGizmo(int part);
    void parentSelected();
    void unparentSelected();
    void bindSelected();
    void unbindSelected();
    void resetPose();
    void recomputeWeights();
    void assignWeight(bool remove);
    void normalizeWeights();
    void unwrapActive(uv::Method method);
    void uvTool(int tool);  // 0 fit, 1 pack, 2 rotate, 3 flip U, 4 flip V
    void regenerate(Object& o);
    bool makeEditable(Object& o);  // bakes a parametric recipe; returns true if it was parametric
    void togglePlay();
    void restartParticles();
    void newScene();
    void saveFile();
    void loadFile();
    void openPath(const std::string& path);
    void exportObj();
    void importObj();
    void buildDemo(int which);
    std::vector<int> editFaces();          // selected faces (edit mode) or all faces of the active mesh
    std::vector<int> transformRoots();     // selected objects without a selected ancestor

    // --- undo (editor.cpp) ---
    Snapshot snapshot();
    std::shared_ptr<const Mesh> shareMesh(const Mesh& m);
    void pushUndo();
    void beginEdit(uint32_t widgetId);
    void undo();
    void redo();
    void restore(Snapshot s);
    void markDirty();

    // --- transform tool (editor.cpp) ---
    bool beginTransform(Xform type, bool pushUndoState, bool fromGizmo = false);
    void updateTransform(const Input& in);  // modal (keyboard) transforms
    void applyTransform(const XfDelta& d);
    void endTransform(bool confirm);
    float axisDrag(Vec3 pivot, Vec3 axis, Vec2 mouseDelta) const;  // world units along axis

    // --- Unity-style gizmo (editor_gizmo.cpp) ---
    void computeGizmo();
    int pickHandle(Vec2 p);
    bool beginGizmoDrag(int handle, const Input& in);
    void updateGizmoDrag(const Input& in);
    void drawGizmo();
    Vec3 gizmoAxis(int i) const { return {gizmoAxes_(0, i), gizmoAxes_(1, i), gizmoAxes_(2, i)}; }
    bool localBounds(const Object& o, Vec3& lo, Vec3& hi);
    void setTool(Tool t);
    void setKeymap(Keymap k);
    void loadConfig();
    void saveConfig();

    // --- benchmark (editor_bench.cpp) ---
    void benchmarkTick();
    void benchmarkSetup(int scenario);
    void benchmarkFrame(int scenario, int frame);
    void benchmarkOperations();
    void clearForBenchmark();

    // --- input (editor.cpp) ---
    void handleShortcuts(const Input& in);
    void handleViewport(const Input& in);
    bool handleUvEditor(const Input& in);
    void clickSelect(Vec2 p, bool extend);
    void boxSelect(Vec2 a, Vec2 b, bool extend, bool subtract);
    int pickVertex(Vec2 p);
    int pickIcon(Vec2 p);  // lights / bones / empties / emitters

    // --- workspaces, top bar, menus, command palette (editor_workspace.cpp) ---
    // The left panel follows the 3D pipeline: model -> texture -> rig ->
    // light -> render. Each workspace shows only that stage's tools.
    enum Workspace { WS_MODEL = 0, WS_TEXTURE, WS_RIG, WS_LIGHT, WS_RENDER, WS_COUNT };
    void setWorkspace(int ws);
    static const char* workspaceName(int ws);
    void buildTopBar(const Input& in);
    void buildFileMenu(const Input& in);
    bool section(PanelLayout& L, const char* key, const std::string& title, bool defaultOpen = true);
    struct Command {
        std::string name;   // what the palette lists ("Bevel edges")
        int workspace;      // -1 = anywhere
        input::Action key;  // shortcut shown next to it (Action::Count = none)
        std::function<void()> run;
    };
    void buildCommands();
    void openPalette();
    bool handlePalette(const Input& in);  // true = the palette owns the input this frame
    void buildPalette();
    std::vector<int> paletteMatches() const;
    void runCommand(int index);
    int navWidgetPick(Vec2 p) const;      // on-screen orbit / pan / zoom / frame (-1 none)
    void navWidgetLayout(Rect out[4]) const;
    void drawNavWidget();
    void prefsPanel(PanelLayout& L, const Input& in);
    void modelPanel(PanelLayout& L);
    void texturePanel(PanelLayout& L);
    void rigPanel(PanelLayout& L);
    void lightPanel(PanelLayout& L);
    void renderPanel(PanelLayout& L);

    // --- ui (editor_ui.cpp) ---
    void buildLeftPanel(const Input& in);
    void buildRightPanel(const Input& in);
    void buildStatusBar();
    void buildViewportHeader();
    void buildViewportToolbar();
    void buildStatsOverlay();
    void buildViewportOverlay();
    void buildUvEditor();
    void buildHelp();
    void buildQuitDialog();
    void objectProperties(PanelLayout& L, Object& o, const Input& in);
    bool vec3Fields(PanelLayout& L, const char* key, uint32_t salt, Vec3& v, float speed, float lo, float hi,
                    const Color* colors);
    bool floatRow(PanelLayout& L, const char* label, const char* key, uint32_t salt, float& v, float speed, float lo,
                  float hi, bool integer = false);
    void label(PanelLayout& L, const std::string& text, Color c);

    // --- rendering (editor_render.cpp) ---
    void renderViewport();
    void buildGrid();
    void appendGizmos(std::vector<LineVertex>& lines, std::vector<ParticleVertex>& glows);
    void collectLights(FrameParams& f);
    void updateParticles(float dt);
    uint64_t meshKey(int index, bool restPose, int weightSlot) const;

    // --- helpers (editor.cpp) ---
    void updateMatrices();
    bool worldToScreen(Vec3 p, Vec2& out) const;
    void viewRay(Vec2 p, Vec3& origin, Vec3& dir) const;
    float worldPerPixel(Vec3 p) const;
    Vec3 camRight() const { return view_.row3(0); }
    Vec3 camUp() const { return view_.row3(1); }
    Vec3 camForward() const { return -view_.row3(2); }
    std::vector<char>& vertSel();
    Object* active() { return scene_.activeObject(); }
    Object* activeMesh();
    void selectOnly(int index);
    void toggleSelect(int index);
    void setStatus(const std::string& msg, bool error = false);
    void updateTitle();
    void saveScreenshot();
    int weightSlotFor(const Object& o) const;
    int displayWeightSlot(const Object& o) const;  // slot shown in Weights view

    Scene scene_;
    Renderer renderer_;
    UI ui_;
    Camera cam_;
    Mode mode_ = Mode::Object;
    std::vector<char> vsel_;  // edit-mode vertex selection of the active mesh
    SelMode selMode_ = SelMode::Vertex;
    std::vector<char> fsel_;                // face selection (face mode)
    std::vector<meshedit::Edge> esel_;      // edge selection (edge mode), sorted
    uint64_t selTopology_ = 0;              // topology the edge / face selection belongs to
    uint32_t selObject_ = 0;
    struct MeshOp {  // the last topology tool, re-run from `before` when its settings change
        OpType type = OpType::None;
        uint32_t objectId = 0;
        Mesh before;
        std::vector<int> faces;
        std::vector<meshedit::Edge> edges;
        std::vector<char> verts;
        meshedit::InsetParams inset;
        meshedit::BevelParams bevel;
        meshedit::LoopCutParams loop;
        int cuts = 1;
        meshedit::BridgeParams bridge;
        meshedit::PushParams push;
        float poke = 0.0f;
        uint64_t resultVersion = 0;
        std::string error;  // why the last run failed (shown in the panel)
        std::string info;   // e.g. "width clamped to 0.48"
    } lastOp_;
    meshedit::InsetParams insetDefaults_;
    meshedit::BevelParams bevelDefaults_;
    meshedit::PushParams pushDefaults_;
    bool opModal_ = false;          // interactive inset / bevel / push (mouse or typed value)
    std::string opTyped_;           // value typed during a modal op or transform
    Vec2 opCenter_, opStartMouse_;
    float opWorldPerPixel_ = 0.01f;
    double lastEdgeClickTime_ = -1;
    meshedit::Edge lastEdgeClick_{-1, -1};
    std::unordered_map<uint32_t, MeshAccel> meshAccel_;  // picking BVHs, per object

    // matrices & layout (recomputed every frame)
    Mat4 view_, proj_, viewProj_;
    Rect viewport_, leftPanel_, rightPanel_, statusBar_, outlinerRect_, headerRect_, uvRect_, toolbarRect_;
    int screenW_ = 0, screenH_ = 0;
    float dpi_ = 1.0f;
    int fontScale_ = 2;
    Vec2 mouse_;  // viewport-local mouse position

    // transform tool state
    Xform xf_ = Xform::None;
    int xfAxis_ = -1;  // -1 free, 0..2 world axis, 3 custom axis
    Vec3 xfCustomAxis_;
    bool xfPushedUndo_ = false;
    Vec2 xfStart_;
    float xfPrevAngle_ = 0, xfAngle_ = 0;
    Vec3 xfPivot_;
    std::vector<int> xfItems_;
    std::vector<Vec3> xfPos_, xfRot_, xfScale_, xfLocal_;
    std::vector<Mat4> xfWorld_, xfParentInv_;
    std::string xfInfo_;
    bool xfFromGizmo_ = false;
    bool xfPerObjectPivot_ = false;

    // Unity-style tools
    Tool tool_ = Tool::Move;
    Keymap keymap_ = Keymap::Unity;
    bool localSpace_ = false;   // gizmo axes: global (false) or the active object's (true)
    bool pivotCenter_ = false;  // handle position: object pivot (false) or selection centre (true)
    int gizmoHover_ = GH_None, gizmoDrag_ = GH_None;
    bool gizmoVisible_ = false;
    Vec3 gizmoPivot_;
    Mat4 gizmoAxes_;
    float gizmoSize_ = 1.0f;
    Vec2 gizmoMouseStart_;
    Vec3 gizmoPlaneStart_;
    Vec2 gizmoTangent_;
    float gizmoRadiusPx_ = 1.0f;
    struct BoundsCache {
        uint64_t version = ~0ull;
        Vec3 lo, hi;
    };
    std::unordered_map<uint32_t, BoundsCache> boundsCache_;
    bool hasClipboard_ = false;
    Vec3 clipPosition_, clipRotation_, clipScale_{1, 1, 1};
    std::string configPath_;
    float lastDt_ = 1.0f / 60.0f;
    double clock_ = 0;  // seconds since start (double-click timing)

    // Unity-style scene camera
    bool camAnimating_ = false;
    Camera camFrom_, camTo_;
    float camAnimT_ = 0;
    float flySpeed_ = 1.0f;   // scroll while flying to change it
    float flyHold_ = 0;       // seconds the fly keys have been held (acceleration)
    // Look-around adjustability (Camera tab, saved in Modeler3D.cfg).
    struct CameraSettings {
        float orbitSensitivity = 0.4f;  // degrees per pixel (Alt+LMB / Blender MMB)
        float lookSensitivity = 0.25f;  // degrees per pixel while flying (RMB)
        float panSpeed = 1.0f;          // x the "grab the point under the cursor" rate
        float zoomSpeed = 1.0f;         // wheel / Alt+RMB multiplier
        float arrowSpeed = 1.0f;        // arrow-key camera movement
        float fastMultiplier = 3.0f;    // Shift while flying / arrows
        float transition = 0.3f;        // seconds for animated view changes (0 = instant)
        bool invertX = false, invertY = false;
        bool flyAcceleration = true;
        // Trackpad navigation: two-finger scroll orbits, Shift+scroll pans,
        // pinch / Ctrl+scroll zooms (instead of scroll = zoom).
        bool trackpad = false;
        bool navWidget = true;  // on-screen orbit / pan / zoom buttons
    } camSet_;
    // Rebindable keys (Keys tab).
    input::KeyMap keys_;
    int captureAction_ = -1, captureSlot_ = 0;  // waiting for a key press in the Keys tab
    bool swallowKeys_ = false;                   // a key was just captured: ignore it this frame
    std::string keysFilter_;
    bool configDirty_ = false;  // camera settings changed: save when the drag ends
    Rect sceneGizmoRect_;
    int sceneGizmoHover_ = -1;
    uint32_t lastOutlinerClickId_ = 0;
    uint32_t outlinerAnchorId_ = 0;  // Shift+click range start in the hierarchy
    // Hierarchy drag & drop
    uint32_t dragRowId_ = 0;        // row pressed (candidate for a drag)
    Vec2 dragPress_;
    bool dragActive_ = false;
    bool dragDeferSelect_ = false;  // plain click on a selected row: select it alone on release
    enum DropZone { DropNone, DropBefore, DropOnto, DropAfter, DropRoot };
public:
    // Moves `ids` (and their subtrees) in the hierarchy: onto = parent to the
    // target, before/after = become the target's sibling at that position,
    // root = unparent and move to the end. Keeps world transforms. Returns
    // false if the move is impossible (e.g. onto one of its own children).
    bool moveInHierarchy(const std::vector<uint32_t>& ids, uint32_t targetId, int zone);
private:
    double lastOutlinerClickTime_ = -1;
    bool showStats_ = false;

    // benchmark
    std::string benchReport_;
    std::vector<int> benchOnly_;
    bool skipScenarioStats_ = false;
    int benchScenario_ = -1, benchFrame_ = 0;
    std::string benchText_;

    // navigation / selection state
    Nav nav_ = Nav::None;
    int navButton_ = -1;
    bool lmbInViewport_ = false;
    bool boxSelecting_ = false;
    Vec2 pressPos_;

    // UV editor overlay
    bool uvEditor_ = false;
    bool uvDragging_ = false;
    Vec2 uvDragLast_;

    // panels
    int workspace_ = WS_MODEL;
    bool prefsOpen_ = false;
    int prefsTab_ = 0;  // 0 keys, 1 navigation, 2 interface
    bool fileMenu_ = false;
    Rect topBar_, fileMenuRect_, fileButtonRect_;
    std::unordered_map<uint32_t, bool> sectionOpen_;
    std::vector<Command> commands_;
    bool paletteOpen_ = false;
    std::string paletteQuery_;
    int paletteSel_ = 0;
    bool paletteMouseMoved_ = false;
    bool navFromWidget_ = false;  // the current orbit / pan / zoom drag started on the nav buttons
    int navHover_ = -1;
    int uiScale_ = 0;  // 0 = automatic (from the display DPI), else 1..4
    float leftScroll_ = 0, rightScroll_ = 0, outlinerScroll_ = 0;
    float leftContentH_ = 0, rightContentH_ = 0;

    // view options
    bool csgTriangulate_ = true, csgKeepCutters_ = false;
    // Rendering (Render tab)
    struct RenderSettings {
        int device = 0;             // DEV_GPU (OpenGL), DEV_CPU or DEV_RTX (DirectX ray tracing)
        bool allowWarp = false;     // RTX device: allow Microsoft's software DXR (WARP) for testing
        float samples = 128;        // samples per pixel (progressive target)
        float bounces = 4;          // max path length after the first hit
        float resolution = 100;     // % of the viewport size
        float clamp = 10;           // firefly clamp (0 = off)
        float envStrength = 1;      // sky / ambient multiplier
        float lightSize = 0.05f;    // soft shadow size
        bool studioLights = true;   // studio key/fill when the scene has no lamps
        float gpuBudgetMs = 12;     // GPU time spent tracing per frame
        float cpuThreads = 0;       // 0 = all hardware threads
        bool useCamera = true;      // render through the scene's camera object (if there is one)
    } renderSet_;
    enum { DEV_GPU = 0, DEV_CPU = 1, DEV_RTX = 2 };
    static const char* renderDeviceName(int device);
    bool ensureHwrt();
    double renderSeconds() const;
    double renderRate() const;
    bool renderView_ = false, renderAutoUpdate_ = true;
    int renderDevice_ = 0;
    float renderExposure_ = 1.0f;
    HwRayTracer hwrt_;
    HwRtInfo hwrtInfo_;
    std::string hwrtError_;
    bool hwrtWarp_ = false;
    std::vector<uint8_t> hwrtImage_;
    rt::CpuRenderer cpuRender_;
    GpuTracer gpuTracer_;
    bool gpuTracerOk_ = false;
    std::string gpuTracerError_;
    std::shared_ptr<rt::SceneData> rtScene_;
    uint64_t renderStamp_ = 0;
    std::string renderOut_;
    GpuTimer viewportTimer_;
    std::string glVendor_, glRenderer_, glVersion_;
    bool softwareGl_ = false;
    bool wireframe_ = false, showGrid_ = true, showHelp_ = false;
    int shading_ = SHADE_STUDIO;

    // skin weight tools
    int weightSlot_ = -1;
    float weightValue_ = 1.0f;

    // particles
    bool playing_ = true;
    std::unordered_map<uint32_t, ParticleSystemState> particles_;

    // file
    std::string filePath_ = "scene.m3d";
    std::string fileField_ = "scene.m3d";
    bool dirty_ = false;
    std::string lastTitle_;

    // status line
    std::string status_;
    bool statusError_ = false;
    float statusTime_ = 0;
    float fps_ = 60;

    // undo
    std::deque<Snapshot> undo_, redo_;
    std::unordered_map<uint64_t, std::weak_ptr<const Mesh>> meshPool_;  // content stamp -> stored copy
    uint32_t editGroup_ = 0;

    bool confirmQuit_ = false, exit_ = false;
    bool screenshotPending_ = false;
    int colorCursor_ = 1;
    std::vector<LineVertex> grid_;
    std::vector<std::pair<int, int>> editEdges_;
    std::vector<LineVertex> editFill_;  // selected faces (face mode)
    uint64_t editEdgesVersion_ = ~0ull;  // topology stamp the edge list was built from
    std::vector<LineVertex> editLines_, editPoints_;
    uint64_t editOverlayKey_ = ~0ull;
    uint64_t editStructKey_ = ~0ull;           // topology + selection the overlay buffers were built for
    std::vector<int> editSlotStart_, editSlots_;  // vertex -> overlay entries showing it (CSR)
    std::vector<Vec3> editPosCache_;             // positions the overlay buffers hold
    std::vector<Vec3> scratch_;
};
