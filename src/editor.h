#pragma once
// The modeling application: owns the scene, camera, tools, undo history and
// UI. One call to frame() per iteration of the main loop (GEA Vol. I ch. 8).
//
// Implementation is split by concern:
//   editor.cpp         frame loop, input, selection, transform tool, undo
//   editor_actions.cpp commands (create, mesh/UV/rig tools, hierarchy, files)
//   editor_ui.cpp      panels, viewport header, UV editor, dialogs
//   editor_render.cpp  3D viewport drawing, gizmos, lights, particles
#include "particles.h"
#include "platform.h"
#include "renderer.h"
#include "scene.h"
#include "ui.h"
#include "uv.h"

#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

struct AppOptions {
    std::string openPath;
    int demo = 0;  // 0 = default scene, 1..4 = sample scenes (for screenshots)
    bool showHelp = false;
    std::string benchmarkReport;  // non-empty: run the performance benchmark and exit
    std::string configPath;       // settings file (keymap); empty = don't persist
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
    struct Snapshot {
        std::vector<Object> objects;
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
    void duplicateSelected();
    void deleteSelected();
    void subdivideSelected();
    void flipSelected();
    void setSmoothSelected(bool smooth);
    void selectAll();
    void extrude();
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
    Snapshot snapshot() const;
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
    Rect sceneGizmoRect_;
    int sceneGizmoHover_ = -1;
    uint32_t lastOutlinerClickId_ = 0;
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
    uint64_t editEdgesVersion_ = ~0ull;  // topology stamp the edge list was built from
    std::vector<LineVertex> editLines_, editPoints_;
    uint64_t editOverlayKey_ = ~0ull;
    std::vector<Vec3> scratch_;
};
