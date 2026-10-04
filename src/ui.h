#pragma once
// Immediate-mode GUI (the style of in-game debug menus in GEA Vol. I sec. 10.3
// and the overlay/HUD layer of GEA Vol. II sec. 12.8). Widgets are plain
// function calls made every frame; the UI only keeps which widget is hot /
// active / being typed into. Output is a list of textured quads that the
// renderer draws in one pass with scissor rectangles for clipping.
#include "platform.h"

#include <cstdint>
#include <string>
#include <vector>

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

struct Color {
    float r = 1, g = 1, b = 1, a = 1;
};
inline Color withAlpha(Color c, float a) { return {c.r, c.g, c.b, a}; }

namespace theme {
inline const Color background{0.205f, 0.215f, 0.235f, 1};
inline const Color panel{0.150f, 0.155f, 0.170f, 1};
inline const Color panelDark{0.110f, 0.114f, 0.125f, 1};
inline const Color border{0.070f, 0.072f, 0.080f, 1};
inline const Color button{0.235f, 0.245f, 0.270f, 1};
inline const Color buttonHover{0.300f, 0.315f, 0.350f, 1};
inline const Color buttonPress{0.190f, 0.200f, 0.220f, 1};
inline const Color field{0.100f, 0.104f, 0.115f, 1};
inline const Color accent{0.290f, 0.500f, 0.880f, 1};
inline const Color text{0.880f, 0.890f, 0.910f, 1};
inline const Color textDim{0.540f, 0.560f, 0.600f, 1};
inline const Color white{1, 1, 1, 1};
inline const Color selection{1.000f, 0.600f, 0.160f, 1};
inline const Color error{1.000f, 0.430f, 0.380f, 1};
}  // namespace theme

// Hash a string (plus an optional integer salt) into a widget id.
uint32_t uiHash(const char* s, uint32_t salt = 0);

class UI {
public:
    struct Vertex {
        float x, y, u, v;
        float r, g, b, a;
    };
    struct DrawCmd {
        Rect clip;
        int first = 0;
        int count = 0;
    };

    int fs = 2;  // integer font scale (pixels per font pixel)

    void begin(const Input& in, int screenW, int screenH, int fontScale);
    void end();
    void setInputEnabled(bool on) { inputEnabled_ = on; }
    bool inputEnabled() const { return inputEnabled_; }

    const std::vector<Vertex>& vertices() const { return verts_; }
    const std::vector<DrawCmd>& commands() const { return cmds_; }

    // Metrics
    float glyphH() const { return 7.0f * fs; }
    float rowHeight() const { return 12.0f * fs; }
    float textWidth(const std::string& s) const;
    std::string fitText(const std::string& s, float maxWidth) const;

    // Drawing
    void rect(const Rect& r, Color c);
    void border(const Rect& r, Color c, float thickness);
    void line(float x0, float y0, float x1, float y1, float thickness, Color c);
    void text(float x, float y, const std::string& s, Color c);
    void textIn(const Rect& r, const std::string& s, Color c, bool centered);
    void pushClip(const Rect& r);
    void popClip();

    // Widgets
    bool button(uint32_t id, const Rect& r, const std::string& label, bool toggled = false);
    bool selectable(uint32_t id, const Rect& r, const std::string& label, bool selected, bool active);
    bool swatch(uint32_t id, const Rect& r, Color c, bool selected);
    // Drag horizontally to change; click (without dragging) to type a value.
    bool dragFloat(uint32_t id, const Rect& r, float& value, float speed, Color accent, float lo = -1e9f,
                   float hi = 1e9f);
    // Returns true when the user commits an edit (Enter or clicking away).
    bool textField(uint32_t id, const Rect& r, std::string& value);
    void header(const Rect& r, const std::string& label);

    bool hovered(const Rect& r) const;
    bool isActive() const { return activeId_ != 0; }
    bool wantsKeyboard() const { return editId_ != 0; }
    bool keyboardUsedThisFrame() const { return keyboardUsed_; }
    void cancelEdit() { editId_ = 0; }

private:
    void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, Color c);
    void vert(float x, float y, float u, float v, Color c);
    void beginCmd(const Rect& clip);
    float mx() const { return in_->mouseX; }
    float my() const { return in_->mouseY; }
    void editKeys(std::string& buf, bool numeric);

    const Input* in_ = nullptr;
    std::vector<Vertex> verts_;
    std::vector<DrawCmd> cmds_;
    std::vector<Rect> clips_;
    bool inputEnabled_ = true;

    uint32_t activeId_ = 0;  // widget holding the mouse
    uint32_t editId_ = 0;    // widget holding the keyboard
    bool activeSeen_ = false, editSeen_ = false, keyboardUsed_ = false;
    std::string editBuf_;
    float dragStartX_ = 0, dragStartValue_ = 0;
    bool dragMoved_ = false;
};
