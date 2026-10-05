#pragma once
// Rebindable input: a table of named actions, each with up to two key
// bindings (key + Ctrl/Shift/Alt). Game engines decouple "what the player
// pressed" from "what it means" with exactly this kind of indirection layer
// (GEA Vol. I sec. 9.5, game engine HID systems: input re-mapping and
// context-sensitive controls). Presets reproduce the Unity and Blender keymaps; the user can
// rebind anything in the Keys tab and the result is saved in Modeler3D.cfg.
#include "platform.h"

#include <string>

namespace input {

struct Binding {
    int key = 0;  // 0 = unbound
    bool ctrl = false, shift = false, alt = false;
    bool bound() const { return key > 0; }
    bool operator==(const Binding& o) const {
        return key == o.key && ctrl == o.ctrl && shift == o.shift && alt == o.alt;
    }
};

// Contexts: two actions may share a binding only if they live in different
// contexts (e.g. W = Move tool, but W = fly forward while RMB is held).
enum class Context { Global, Fly, Modal, Edit };  // Edit: only in Edit mode, checked before Global

enum class Action {
    // Global
    Help, Stats, Screenshot, Undo, Redo, Save, Open, Parent, Unparent,
    ToggleEditMode, FrameSelected, SmartUnwrap, PlayPause, Delete, Duplicate, SelectAll, Extrude,
    ViewFront, ViewBack, ViewRight, ViewLeft, ViewTop, ViewBottom, ToggleOrtho,
    ToolHand, ToolMove, ToolRotate, ToolScale, ToolUniversal, ToggleLocalGlobal, TogglePivotCenter,
    CycleShading, ToggleWireframe, ModalGrab, ModalRotate, ModalScale,
    Render, LookThroughCamera, AlignCameraToView, CameraForward, CameraBack, CameraLeft, CameraRight,
    // Fly (RMB held, Unity)
    FlyForward, FlyBack, FlyLeft, FlyRight, FlyUp, FlyDown,
    // Modal transform
    AxisX, AxisY, AxisZ,
    // Edit mode
    SelectVertices, SelectEdges, SelectFaces, Inset,
    Count
};
constexpr int kActionCount = (int)Action::Count;

struct ActionInfo {
    const char* id;     // stable name used in the config file
    const char* label;  // shown in the Keys tab
    Context context;
    bool held;          // continuous (checked with down()), modifiers ignored
};
const ActionInfo& info(Action a);
bool actionFromId(const std::string& id, Action& out);

std::string keyName(int key);
std::string toString(const Binding& b);  // "C-S-Z", "F1", "Space", "-" when unbound
bool parse(const std::string& text, Binding& out);

enum class Preset { Unity, Blender };

struct KeyMap {
    Binding slots[kActionCount][2];

    void setPreset(Preset p);
    void reset(Action a, Preset p);  // restore one action from a preset
    // Discrete actions: pressed this frame with exactly these modifiers.
    bool pressed(Action a, const Input& in) const;
    // Held actions: key down (extra modifiers allowed, e.g. Shift = faster).
    bool down(Action a, const Input& in) const;
    // Assigns a binding; any other action in the same context using it is
    // unbound. Returns the action that lost it (or Action::Count).
    Action assign(Action a, int slot, const Binding& b);
    Action conflict(Action a, const Binding& b) const;  // same context, other action

    // Config lines "bind <id> <binding> [<binding>]".
    std::string serialize() const;
    bool parseLine(const std::string& line);
};

}  // namespace input
