#pragma once
// The modeling application: owns the scene, camera, tools, undo history and
// UI. One call to frame() per iteration of the main loop (GEA Vol. I ch. 8).
#include "platform.h"
#include "renderer.h"
#include "scene.h"
#include "ui.h"

#include <deque>
#include <string>
#include <vector>

struct AppOptions {
    std::string openPath;
    int demo = 0;  // 0 = default cube, 1 = sample scene, 2 = edit-mode sample
    bool showHelp = false;
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
    enum class Nav { None, Orbit, Pan };
    struct Snapshot {
        std::vector<Object> objects;
        int active;
        std::vector<char> vsel;
    };

    // --- actions ---
    void addPrimitive(int kind);
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
    void newScene();
    void saveFile();
    void loadFile();
    void openPath(const std::string& path);
    void exportObj();
    void importObj();
    void buildDemo(int which);

    // --- undo ---
    Snapshot snapshot() const;
    void pushUndo();
    void beginEdit(uint32_t widgetId);
    void undo();
    void redo();
    void restore(Snapshot s);
    void markDirty();

    // --- transform tool ---
    bool beginTransform(Xform type, bool pushUndoState);
    void updateTransform(const Input& in);
    void endTransform(bool confirm);

    // --- input ---
    void handleShortcuts(const Input& in);
    void handleViewport(const Input& in);
    void clickSelect(Vec2 p, bool extend);
    void boxSelect(Vec2 a, Vec2 b, bool extend, bool subtract);
    int pickVertex(Vec2 p);

    // --- ui ---
    void buildLeftPanel(const Input& in);
    void buildRightPanel(const Input& in);
    void buildStatusBar();
    void buildViewportOverlay();
    void buildHelp();
    void buildQuitDialog();
    bool vec3Fields(float& y, float x, float w, const char* key, uint32_t salt, Vec3& v, float speed, float lo, float hi,
                    const Color* colors);

    // --- rendering ---
    void renderViewport();
    void saveScreenshot();
    void buildGrid();

    // --- helpers ---
    void updateMatrices();
    bool worldToScreen(Vec3 p, Vec2& out) const;
    void viewRay(Vec2 p, Vec3& origin, Vec3& dir) const;
    float worldPerPixel(Vec3 p) const;
    Vec3 camRight() const { return view_.row3(0); }
    Vec3 camUp() const { return view_.row3(1); }
    Vec3 camForward() const { return -view_.row3(2); }
    std::vector<char>& vertSel();
    Object* active() { return scene_.activeObject(); }
    void selectOnly(int index);
    void toggleSelect(int index);
    void setStatus(const std::string& msg, bool error = false);
    void updateTitle();

    Scene scene_;
    Renderer renderer_;
    UI ui_;
    Camera cam_;
    Mode mode_ = Mode::Object;
    std::vector<char> vsel_;  // edit-mode vertex selection of the active object

    // matrices & layout (recomputed every frame)
    Mat4 view_, proj_, viewProj_;
    Rect viewport_, leftPanel_, rightPanel_, statusBar_, outlinerRect_;
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
    std::string xfInfo_;

    // navigation / selection state
    Nav nav_ = Nav::None;
    int navButton_ = -1;
    bool lmbInViewport_ = false;
    bool boxSelecting_ = false;
    Vec2 pressPos_;

    // panels
    float leftScroll_ = 0, rightScroll_ = 0, outlinerScroll_ = 0;
    float leftContentH_ = 0, rightContentH_ = 0;

    // options
    bool wireframe_ = false, showGrid_ = true, showHelp_ = false;

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
    uint64_t editEdgesVersion_ = ~0ull;
};
