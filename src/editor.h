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
#include "meshtools.h"
#include "particles.h"
#include "platform.h"
#include "renderer.h"
#include "scene.h"
#include "ui.h"
#include "uv.h"

#include <chrono>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct AppOptions {
    std::string openPath;
    int demo = 0;  // 0 = default scene, 1..4 = sample scenes (for screenshots)
    bool showHelp = false;
    std::string benchmarkReport;  // non-empty: run the performance benchmark and exit
    std::string stressReport;     // non-empty: run the stress test (editor_stress.cpp) and exit
    std::string stressOnly;       // letters of the stress groups to run (default: all, "ABCDEFG")
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
    // Undo snapshot. Meshes are kept as shared, immutable copies: a mesh that
    // did not change between two snapshots (same Mesh::version) is stored once,
    // so 64 undo steps of edits to one small object don't copy a large mesh
    // elsewhere in the scene 64 times.
    struct Snapshot {
        std::vector<Object> objects;  // with empty meshes
        std::vector<std::shared_ptr<const Mesh>> meshes;
        int active = -1;
        std::vector<char> vsel;
        Vec3 ambient;
    };
    std::unordered_map<uint32_t, std::pair<uint64_t, std::shared_ptr<const Mesh>>> meshShare_;  // id -> (version, copy)
    size_t undoBytes() const;  // memory held by the undo / redo history (shared meshes counted once)

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
    void applyLastOp();
    bool lastOpAdjustable();
    bool updateInsetModal(const Input& in);
    // --- more mesh tools (editor_tools.cpp) ---
    std::vector<meshedit::Edge> editEdgeList();  // selected edges (edge mode), edges of selected faces / vertices
    void selectResult(const std::vector<int>& newFaces, const std::vector<char>& vsel);
    bool beginMeshOp(int type);                  // pushes undo, captures the mesh and the selection
    void bevelSelected(bool interactive);
    void loopCutAt(bool underMouse);
    void subdivideEdit();
    void connectSelected();
    void pokeSelected();
    void bridgeSelected();
    void fillSelected();
    void mergeSelected(bool perGroup);
    void pushThroughSelected();
    void selectLoopRing(bool ring, bool underMouse, bool extend);
    void growSelection(bool grow);
    void joinSelected();
    void cycleObject(int dir);
    void lastOpPanel(PanelLayout& L);
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

    // --- stress test (editor_stress.cpp) ---
    struct StressStep {
        std::string name;
        std::function<bool()> setup;        // false: nothing to render for this step
        std::function<void(int)> perFrame;  // called before each measured frame
        int frames = 0;
        std::function<void(double first, double avg, double worst)> done;
    };
    struct LadderRow {
        int tris = 0;
        bool skipped = false, bevelOk = true;
        double genMs = -1, firstMs = -1, idleMs = -1, dragMs = -1, bvhMs = -1, pickMs = -1, undoFirst = -1,
               undoAgain = -1, loopSelMs = -1, bevelMs = -1, loopCutMs = -1, saveMs = -1, loadMs = -1, rtMs = -1;
        double memMB = 0;
    };
    struct ObjRow {
        int n = 0;
        bool skipped = false;
        double buildMs = -1, firstMs = -1, idleMs = -1, selMs = -1, rotMs = -1, dupMs = -1, undoMs = -1, delMs = -1;
        double memMB = 0, memCreatedMB = 0, memDrawnMB = 0, memBeforeMB = 0;
    };
    struct ChainRow {
        int depth = 0;
        double buildMs = -1, firstMs = -1, idleMs = -1, worldMs = -1, allWorldsMs = -1, descMs = -1, cycleMs = -1,
               rootsMs = -1, deleteMs = -1;
        bool worldOk = false, descOk = false, cycleRefused = false, deleteOk = false;
    };
    void stressInit();
    void stressTick();
    void stressFinish();
    bool stressCheck(std::string& problem);
    Input stressInput(const Input& real, int w, int h);
    void stressToolFuzz(int ops);
    std::vector<StressStep> stressSteps_;
    std::vector<LadderRow> ladder_;
    std::vector<ObjRow> objLadder_;
    ChainRow chains_[3];
    std::string stressReport_, stressText_, layoutRows_, problemScratch_, stressOnly_;
    size_t stressIndex_ = 0;
    int stressFrame_ = -1;
    bool stressStepActive_ = false;
    std::vector<double> stressTimes_, fuzzFrameMs_;
    std::chrono::steady_clock::time_point stressLast_;
    bool stressing_ = false, fuzzInput_ = false;
    int stressW_ = 0, stressH_ = 0;  // forced frame size (layout test)
    int stressRealW_ = 0, stressRealH_ = 0;
    bool layoutDone_ = false;  // the first frame's layout has run
    uint32_t fuzzRng_ = 12345;
    struct FuzzMouse {
        float x = 400, y = 300;
        bool down[3] = {};
        int hold[3] = {};
    } fuzzMouse_;
    std::vector<std::string> fuzzProblems_;
    int fuzzCommands_ = 0, stressFailures_ = 0;

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

    // --- workspaces, commands, palette, menus, settings (editor_commands.cpp) ---
    // The left panel follows the modeling pipeline: one workspace per stage.
    enum class Workspace { Model, Texture, Rig, Light, Render };
    static constexpr int kWorkspaceCount = 5;
    struct Command {
        std::string id, label, category, hint;
        int action = -1;                   // input::Action whose binding is shown (-1: none)
        std::function<void()> run;
        std::function<bool()> available;   // nullptr = always
        std::function<bool()> checked;     // toggles: highlighted when on
        std::string why;                   // shown when run while unavailable
    };
    void registerCommands();
    const Command* command(const std::string& id) const;
    bool runCommand(const std::string& id, bool fromPalette = false);
    bool cmdButton(const char* id, const Rect& r, const char* label = nullptr);
    std::string shortcutText(int action) const;
    void setWorkspace(Workspace w);
    void openPalette();
    void buildPalette(const Input& in);
    void buildTopBar(const Input& in);
    void buildMenu(const Input& in);
    void buildSettings(PanelLayout& L, const Input& in);
    bool section(PanelLayout& L, const char* id, const char* title, bool defaultOpen = true);
    void applyUiScale();
    std::vector<Command> commands_;
    std::unordered_map<std::string, int> commandIndex_;
    std::vector<std::string> recentCommands_;
    Workspace ws_ = Workspace::Model;
    int wsShading_[kWorkspaceCount] = {0, 0, 0, 1, 0};  // SHADE_* per workspace (Light = Lit)
    bool paletteOpen_ = false;
    std::string paletteQuery_;
    int paletteSel_ = 0;
    float paletteScroll_ = 0;
    Rect paletteRect_;
    int openMenu_ = -1;  // 0 File, 1 Edit, 2 View, 3 Add
    Rect menuRect_, menuButtons_[4], topBar_;
    bool settingsOpen_ = false;
    int settingsTab_ = 0;
    std::unordered_map<uint32_t, bool> sectionOpen_;
    int propsTab_ = 0;  // 0 object, 1 material, 2 data (light / camera / particles / bone)
    // Settings > Input: laptop / trackpad / keyboard-only options (saved in Modeler3D.cfg).
    struct AccessSettings {
        bool trackpad = false;    // two-finger scroll orbits, Shift+scroll pans, pinch / Ctrl+scroll zooms
        int uiScale = 0;          // 0 = automatic from the display DPI, else 1..6
        bool tooltips = true;
        bool navButtons = true;   // drag pads next to the scene gizmo (orbit / pan / zoom without buttons)
        bool trackpadHintShown = false;
    } access_;
    bool flyToggle_ = false;       // fly mode switched on from the keyboard (no right button held)
    std::string modalNumber_;      // number typed during a modal transform (G 2 Enter)
    int navPad_ = -1;              // nav pad being dragged: 0 orbit (scene gizmo), 1 pan, 2 zoom
    int navPadPart_ = -1;          // scene gizmo part under the press (a click on it = view axis)
    bool navPadMoved_ = false;
    Vec2 navPadStart_;
    Rect navPads_[3];
    double lastViewportClick_ = -1;
    Vec2 lastViewportClickPos_;
    void layoutNavPads();
    void flyLook(float dx, float dy);
    void flyMove(const Input& in);

    // --- ui (editor_ui.cpp) ---
    void buildWorkspacePanel(PanelLayout& L, const Input& in);
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
    void transformProperties(PanelLayout& L, Object& o);
    void materialProperties(PanelLayout& L, Object& o);
    void dataProperties(PanelLayout& L, Object& o);
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
    struct MeshOp {  // the last topology tool, re-run when its settings change
        enum Type { None, Inset, Bevel, LoopCut, Subdivide, Bridge, PushThrough, Poke } type = None;
        uint32_t objectId = 0;
        Mesh before;
        std::vector<int> faces;
        std::vector<meshedit::Edge> edges;
        std::vector<int> verts;
        meshedit::InsetParams inset;
        meshedit::BevelParams bevel;
        meshedit::BridgeParams bridge;
        meshedit::PushThroughParams push;
        int cuts = 1;
        float offset = 0.0f;   // poke height
        bool regions = false;  // bridge: two face regions instead of edge loops
        SelMode resultMode = SelMode::Face;
        uint64_t resultVersion = 0;
        std::string error;
    } lastOp_;
    meshedit::InsetParams insetDefaults_;
    meshedit::BevelParams bevelDefaults_;
    bool insetModal_ = false;
    int modalOp_ = 0;  // MeshOp::Inset or MeshOp::Bevel while the mouse adjusts it
    Vec2 insetCenter_, insetStartMouse_;
    float insetWorldPerPixel_ = 0.01f;
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
    int leftTab_ = 0;
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
    // Indexed edit overlay (see Renderer::drawEditElements)
    std::vector<uint32_t> editEdgeIdx_, editSelEdgeIdx_, editPointIdx_, editSelPointIdx_, editFillIdx_;
    std::vector<Vec3> editPos_;
    uint64_t editTopoKey_ = ~0ull, editSelKey_ = ~0ull, editPosKey_ = ~0ull;
    std::vector<Vec3> scratch_;
};
