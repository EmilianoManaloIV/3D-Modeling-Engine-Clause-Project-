#include "input_map.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace input {
namespace {

using C = Context;
const ActionInfo kInfo[kActionCount] = {
    {"help", "Help", C::Global, false},
    {"stats", "Stats", C::Global, false},
    {"screenshot", "Screenshot", C::Global, false},
    {"undo", "Undo", C::Global, false},
    {"redo", "Redo", C::Global, false},
    {"save", "Save", C::Global, false},
    {"open", "Open", C::Global, false},
    {"parent", "Parent", C::Global, false},
    {"unparent", "Unparent", C::Global, false},
    {"edit_mode", "Edit mode", C::Global, false},
    {"frame", "Frame", C::Global, false},
    {"smart_uv", "Smart UV", C::Global, false},
    {"play", "Play FX", C::Global, false},
    {"delete", "Delete", C::Global, false},
    {"duplicate", "Duplicate", C::Global, false},
    {"select_all", "Select all", C::Global, false},
    {"extrude", "Extrude", C::Global, false},
    {"view_front", "View front", C::Global, false},
    {"view_back", "View back", C::Global, false},
    {"view_right", "View right", C::Global, false},
    {"view_left", "View left", C::Global, false},
    {"view_top", "View top", C::Global, false},
    {"view_bottom", "View bottom", C::Global, false},
    {"ortho", "Persp/Orth", C::Global, false},
    {"tool_hand", "Hand tool", C::Global, false},
    {"tool_move", "Move tool", C::Global, false},
    {"tool_rotate", "Rotate tool", C::Global, false},
    {"tool_scale", "Scale tool", C::Global, false},
    {"tool_all", "All tool", C::Global, false},
    {"local_global", "Local/Glob", C::Global, false},
    {"pivot_center", "Pivot/Cntr", C::Global, false},
    {"shading", "Shading", C::Global, false},
    {"wireframe", "Wireframe", C::Global, false},
    {"grab", "Grab", C::Global, false},
    {"rotate", "Rotate", C::Global, false},
    {"scale", "Scale", C::Global, false},
    {"render", "Render", C::Global, false},
    {"look_through_camera", "Camera view", C::Global, false},
    {"align_camera", "Cam to view", C::Global, false},
    {"cam_forward", "Cam fwd", C::Global, true},
    {"cam_back", "Cam back", C::Global, true},
    {"cam_left", "Cam left", C::Global, true},
    {"cam_right", "Cam right", C::Global, true},
    {"palette", "Commands", C::Global, false},
    {"zoom_in", "Zoom in", C::Global, true},
    {"zoom_out", "Zoom out", C::Global, true},
    {"orbit_left", "Orbit left", C::Global, true},
    {"orbit_right", "Orbit right", C::Global, true},
    {"orbit_up", "Orbit up", C::Global, true},
    {"orbit_down", "Orbit down", C::Global, true},
    {"fly_mode", "Fly mode", C::Global, false},
    {"next_object", "Next object", C::Global, false},
    {"prev_object", "Prev object", C::Global, false},
    {"join", "Join objects", C::Global, false},
    {"ws_model", "Model space", C::Global, false},
    {"ws_texture", "Texture space", C::Global, false},
    {"ws_rig", "Rig space", C::Global, false},
    {"ws_light", "Light space", C::Global, false},
    {"ws_render", "Render space", C::Global, false},
    {"fly_forward", "Forward", C::Fly, true},
    {"fly_back", "Back", C::Fly, true},
    {"fly_left", "Left", C::Fly, true},
    {"fly_right", "Right", C::Fly, true},
    {"fly_up", "Up", C::Fly, true},
    {"fly_down", "Down", C::Fly, true},
    {"axis_x", "Axis X", C::Modal, false},
    {"axis_y", "Axis Y", C::Modal, false},
    {"axis_z", "Axis Z", C::Modal, false},
    {"select_vertices", "Vertex mode", C::Edit, false},
    {"select_edges", "Edge mode", C::Edit, false},
    {"select_faces", "Face mode", C::Edit, false},
    {"inset", "Inset", C::Edit, false},
    {"bevel", "Bevel", C::Edit, false},
    {"loop_cut", "Loop cut", C::Edit, false},
    {"connect", "Connect", C::Edit, false},
    {"merge", "Merge", C::Edit, false},
    {"fill", "Fill", C::Edit, false},
    {"bridge", "Bridge", C::Edit, false},
    {"push_through", "Push thru", C::Edit, false},
    {"select_more", "Select more", C::Edit, false},
    {"select_less", "Select less", C::Edit, false},
};

Binding B(const char* text) {
    Binding b;
    parse(text, b);
    return b;
}

struct Default {
    Action a;
    const char *unity1, *unity2, *blender1, *blender2;
};
using A = Action;
const Default kDefaults[] = {
    // Every F-key action has a second binding that works on 60-65% keyboards.
    {A::Help, "F1", "S-/", "F1", "S-/"},
    {A::Stats, "F3", "C-I", "F3", "C-I"},
    {A::Screenshot, "F12", "C-S-P", "F12", "C-S-P"},
    {A::Undo, "C-Z", "", "C-Z", ""},
    {A::Redo, "C-Y", "C-S-Z", "C-Y", "C-S-Z"},
    {A::Save, "C-S", "", "C-S", ""},
    {A::Open, "C-O", "", "C-O", ""},
    {A::Parent, "C-P", "", "C-P", ""},
    {A::Unparent, "A-P", "", "A-P", ""},
    {A::ToggleEditMode, "Tab", "", "Tab", ""},
    {A::FrameSelected, "F", "", "F", ""},
    {A::SmartUnwrap, "U", "", "U", ""},
    {A::PlayPause, "Space", "", "Space", ""},
    {A::Delete, "Delete", "", "Delete", "X"},
    {A::Duplicate, "C-D", "", "S-D", ""},
    {A::SelectAll, "C-A", "", "A", ""},
    {A::Extrude, "C-E", "", "E", ""},
    {A::ViewFront, "1", "", "1", ""},
    {A::ViewBack, "C-1", "", "C-1", ""},
    {A::ViewRight, "3", "", "3", ""},
    {A::ViewLeft, "C-3", "", "C-3", ""},
    {A::ViewTop, "7", "", "7", ""},
    {A::ViewBottom, "C-7", "", "C-7", ""},
    {A::ToggleOrtho, "5", "", "5", ""},
    {A::ToolHand, "Q", "", "", ""},
    {A::ToolMove, "W", "", "", ""},
    {A::ToolRotate, "E", "", "", ""},
    {A::ToolScale, "R", "", "", ""},
    {A::ToolUniversal, "Y", "", "", ""},
    {A::ToggleLocalGlobal, "X", "", "", ""},
    {A::TogglePivotCenter, "Z", "", "", ""},
    {A::CycleShading, "S-Z", "", "Z", ""},
    {A::ToggleWireframe, "", "", "W", ""},
    {A::ModalGrab, "G", "", "G", ""},
    {A::ModalRotate, "", "", "R", ""},
    {A::ModalScale, "", "", "S", ""},
    {A::Render, "F5", "C-S-R", "F5", "C-S-R"},
    {A::LookThroughCamera, "0", "", "0", ""},
    {A::AlignCameraToView, "C-A-0", "", "C-A-0", ""},
    {A::CameraForward, "Up", "", "", ""},
    {A::CameraBack, "Down", "", "", ""},
    {A::CameraLeft, "Left", "", "", ""},
    {A::CameraRight, "Right", "", "", ""},
    {A::Palette, "C-K", "S-Space", "C-K", "S-Space"},
    {A::ZoomIn, "Equals", "", "Equals", ""},
    {A::ZoomOut, "Minus", "", "Minus", ""},
    {A::OrbitLeft, "A-Left", "", "A-Left", ""},
    {A::OrbitRight, "A-Right", "", "A-Right", ""},
    {A::OrbitUp, "A-Up", "", "A-Up", ""},
    {A::OrbitDown, "A-Down", "", "A-Down", ""},
    {A::FlyMode, "S-F", "", "S-F", ""},
    {A::NextObject, "]", "", "]", ""},
    {A::PrevObject, "[", "", "[", ""},
    {A::JoinObjects, "C-J", "", "C-J", ""},
    {A::WorkspaceModel, "A-1", "", "A-1", ""},
    {A::WorkspaceTexture, "A-2", "", "A-2", ""},
    {A::WorkspaceRig, "A-3", "", "A-3", ""},
    {A::WorkspaceLight, "A-4", "", "A-4", ""},
    {A::WorkspaceRender, "A-5", "", "A-5", ""},
    {A::FlyForward, "W", "", "W", ""},
    {A::FlyBack, "S", "", "S", ""},
    {A::FlyLeft, "A", "", "A", ""},
    {A::FlyRight, "D", "", "D", ""},
    {A::FlyUp, "E", "", "E", ""},
    {A::FlyDown, "Q", "", "Q", ""},
    {A::AxisX, "X", "", "X", ""},
    {A::AxisY, "Y", "", "Y", ""},
    {A::AxisZ, "Z", "", "Z", ""},
    {A::SelectVertices, "1", "", "1", ""},
    {A::SelectEdges, "2", "", "2", ""},
    {A::SelectFaces, "3", "", "3", ""},
    {A::Inset, "I", "", "I", ""},
    {A::Bevel, "C-B", "", "C-B", ""},
    {A::LoopCut, "C-R", "", "C-R", ""},
    {A::Connect, "J", "", "J", ""},
    {A::Merge, "M", "", "M", ""},
    {A::Fill, "A-F", "", "A-F", ""},
    {A::Bridge, "A-B", "", "A-B", ""},
    {A::PushThrough, "A-E", "", "A-E", ""},
    {A::SelectMore, "C-Equals", "", "C-Equals", ""},
    {A::SelectLess, "C-Minus", "", "C-Minus", ""},
};

struct NamedKey {
    int key;
    const char* name;
};
const NamedKey kNames[] = {
    {KEY_SPACE, "Space"},   {KEY_ESCAPE, "Esc"},     {KEY_ENTER, "Enter"}, {KEY_TAB, "Tab"},
    {KEY_BACKSPACE, "Bksp"}, {KEY_DELETE, "Delete"}, {KEY_LEFT, "Left"},   {KEY_RIGHT, "Right"},
    {KEY_UP, "Up"},         {KEY_DOWN, "Down"},      {KEY_HOME, "Home"},   {KEY_END, "End"},
    {KEY_PAGEUP, "PgUp"},   {KEY_PAGEDOWN, "PgDn"},  {KEY_INSERT, "Ins"},  {'-', "Minus"},
    {'=', "Equals"},
};

bool modsMatch(const Binding& b, const Input& in) {
    return b.ctrl == in.ctrl() && b.shift == in.shift() && b.alt == in.alt();
}

}  // namespace

const ActionInfo& info(Action a) { return kInfo[(int)a]; }

bool actionFromId(const std::string& id, Action& out) {
    for (int i = 0; i < kActionCount; ++i)
        if (id == kInfo[i].id) {
            out = (Action)i;
            return true;
        }
    return false;
}

std::string keyName(int key) {
    if (key <= 0) return "-";
    for (const NamedKey& n : kNames)
        if (n.key == key) return n.name;
    if (key >= KEY_F1 && key <= KEY_F12) return "F" + std::to_string(key - KEY_F1 + 1);
    if (key < 128 && std::isprint(key)) return std::string(1, (char)key);
    return "#" + std::to_string(key);
}

std::string toString(const Binding& b) {
    if (!b.bound()) return "-";
    std::string s;
    if (b.ctrl) s += "C-";
    if (b.shift) s += "S-";
    if (b.alt) s += "A-";
    return s + keyName(b.key);
}

bool parse(const std::string& text, Binding& out) {
    out = Binding();
    if (text.empty() || text == "-" || text == "none") return true;
    std::string rest = text;
    while (rest.size() > 2 && rest[1] == '-') {
        char m = (char)std::toupper((unsigned char)rest[0]);
        if (m == 'C') out.ctrl = true;
        else if (m == 'S') out.shift = true;
        else if (m == 'A') out.alt = true;
        else return false;
        rest = rest.substr(2);
    }
    for (const NamedKey& n : kNames)
        if (rest == n.name) return (out.key = n.key), true;
    if (rest.size() >= 2 && rest[0] == 'F' && std::isdigit((unsigned char)rest[1])) {
        int f = std::atoi(rest.c_str() + 1);
        if (f >= 1 && f <= 12) return (out.key = KEY_F1 + f - 1), true;
    }
    if (rest.size() > 1 && rest[0] == '#') {
        int k = std::atoi(rest.c_str() + 1);
        if (k > 0 && k < KEY_COUNT) return (out.key = k), true;
    }
    if (rest.size() == 1 && std::isprint((unsigned char)rest[0])) {
        out.key = std::toupper((unsigned char)rest[0]);
        return true;
    }
    out = Binding();
    return false;
}

void KeyMap::setPreset(Preset p) {
    for (int i = 0; i < kActionCount; ++i) reset((Action)i, p);
}

void KeyMap::reset(Action a, Preset p) {
    slots[(int)a][0] = slots[(int)a][1] = Binding();
    for (const Default& d : kDefaults)
        if (d.a == a) {
            slots[(int)a][0] = B(p == Preset::Unity ? d.unity1 : d.blender1);
            slots[(int)a][1] = B(p == Preset::Unity ? d.unity2 : d.blender2);
        }
}

bool KeyMap::pressed(Action a, const Input& in) const {
    for (const Binding& b : slots[(int)a])
        if (b.bound() && in.pressed(b.key) && modsMatch(b, in)) return true;
    return false;
}

bool KeyMap::down(Action a, const Input& in) const {
    for (const Binding& b : slots[(int)a]) {
        if (!b.bound() || b.key >= KEY_COUNT || !in.keyDown[b.key]) continue;
        if ((b.ctrl && !in.ctrl()) || (b.alt && !in.alt())) continue;
        if (!b.ctrl && in.ctrl()) continue;  // Ctrl+key belongs to shortcuts
        if (!b.alt && in.alt()) continue;    // Alt+arrows orbit; plain arrows move
        return true;
    }
    return false;
}

Action KeyMap::conflict(Action a, const Binding& b) const {
    if (!b.bound()) return Action::Count;
    for (int i = 0; i < kActionCount; ++i) {
        if (i == (int)a || kInfo[i].context != info(a).context) continue;
        for (const Binding& o : slots[i])
            if (o == b) return (Action)i;
    }
    return Action::Count;
}

Action KeyMap::assign(Action a, int slot, const Binding& b) {
    Action lost = conflict(a, b);
    if (lost != Action::Count)
        for (Binding& o : slots[(int)lost])
            if (o == b) o = Binding();
    Binding* mine = slots[(int)a];
    if (b.bound() && mine[1 - slot] == b) mine[1 - slot] = Binding();
    mine[slot] = b;
    return lost;
}

std::string KeyMap::serialize() const {
    std::string s;
    for (int i = 0; i < kActionCount; ++i) {
        s += "bind ";
        s += kInfo[i].id;
        s += ' ' + toString(slots[i][0]) + ' ' + toString(slots[i][1]) + '\n';
    }
    return s;
}

bool KeyMap::parseLine(const std::string& line) {
    std::istringstream ss(line);
    std::string word, id, b0, b1;
    if (!(ss >> word >> id) || word != "bind") return false;
    ss >> b0 >> b1;
    Action a;
    if (!actionFromId(id, a)) return false;
    Binding x, y;
    if (!parse(b0, x) || !parse(b1, y)) return false;
    slots[(int)a][0] = x;
    slots[(int)a][1] = y;
    return true;
}

}  // namespace input
