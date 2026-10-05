#include "ui.h"

#include "font.h"
#include "math3d.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

uint32_t uiHash(const char* s, uint32_t salt) {
    uint32_t h = 2166136261u;  // FNV-1a
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    for (int i = 0; i < 4; ++i) {
        h ^= (salt >> (i * 8)) & 0xFFu;
        h *= 16777619u;
    }
    return h ? h : 1;
}

namespace {
Rect intersect(const Rect& a, const Rect& b) {
    float x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    float x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
    return {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
}

Color lighten(Color c, float amount) {
    return {std::min(1.0f, c.r + amount), std::min(1.0f, c.g + amount), std::min(1.0f, c.b + amount), c.a};
}

std::string formatNumber(float v, int decimals) {
    float eps = 0.5f * std::pow(10.0f, (float)-decimals);
    if (std::fabs(v) < eps) v = 0.0f;  // avoid "-0.000"
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

std::string editableNumber(float v) {
    std::string s = formatNumber(v, 4);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}
}  // namespace

void UI::begin(const Input& in, int screenW, int screenH, int fontScale) {
    in_ = &in;
    fs = std::max(1, fontScale);
    verts_.clear();
    cmds_.clear();
    clips_.clear();
    Rect screen{0, 0, (float)screenW, (float)screenH};
    clips_.push_back(screen);
    cmds_.push_back({screen, 0, 0});
    inputEnabled_ = true;
    activeSeen_ = editSeen_ = false;
    keyboardUsed_ = editId_ != 0;
}

void UI::end() {
    cmds_.back().count = (int)verts_.size() - cmds_.back().first;
    // A widget that disappeared (e.g. its object was deleted) releases focus.
    if (activeId_ && !activeSeen_) activeId_ = 0;
    if (editId_ && !editSeen_) editId_ = 0;
}

void UI::beginCmd(const Rect& clip) {
    DrawCmd& last = cmds_.back();
    last.count = (int)verts_.size() - last.first;
    if (last.count == 0) cmds_.pop_back();
    cmds_.push_back({clip, (int)verts_.size(), 0});
}

void UI::pushClip(const Rect& r) {
    clips_.push_back(intersect(clips_.back(), r));
    beginCmd(clips_.back());
}

void UI::popClip() {
    if (clips_.size() > 1) clips_.pop_back();
    beginCmd(clips_.back());
}

bool UI::hovered(const Rect& r) const {
    return inputEnabled_ && r.contains(mx(), my()) && clips_.back().contains(mx(), my());
}

// --- drawing -----------------------------------------------------------------

void UI::vert(float x, float y, float u, float v, Color c) { verts_.push_back({x, y, u, v, c.r, c.g, c.b, c.a}); }

void UI::quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, Color c) {
    vert(x0, y0, u0, v0, c);
    vert(x1, y0, u1, v0, c);
    vert(x1, y1, u1, v1, c);
    vert(x0, y0, u0, v0, c);
    vert(x1, y1, u1, v1, c);
    vert(x0, y1, u0, v1, c);
}

void UI::rect(const Rect& r, Color c) {
    const float u = (font::kWhiteX + 0.5f) / font::kAtlasW, v = (font::kWhiteY + 0.5f) / font::kAtlasH;
    quad(r.x, r.y, r.x + r.w, r.y + r.h, u, v, u, v, c);
}

void UI::border(const Rect& r, Color c, float t) {
    rect({r.x, r.y, r.w, t}, c);
    rect({r.x, r.y + r.h - t, r.w, t}, c);
    rect({r.x, r.y + t, t, r.h - 2 * t}, c);
    rect({r.x + r.w - t, r.y + t, t, r.h - 2 * t}, c);
}

void UI::line(float x0, float y0, float x1, float y1, float t, Color c) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-3f) return;
    float nx = -dy / len * t * 0.5f, ny = dx / len * t * 0.5f;
    const float u = (font::kWhiteX + 0.5f) / font::kAtlasW, v = (font::kWhiteY + 0.5f) / font::kAtlasH;
    vert(x0 + nx, y0 + ny, u, v, c);
    vert(x1 + nx, y1 + ny, u, v, c);
    vert(x1 - nx, y1 - ny, u, v, c);
    vert(x0 + nx, y0 + ny, u, v, c);
    vert(x1 - nx, y1 - ny, u, v, c);
    vert(x0 - nx, y0 - ny, u, v, c);
}

void UI::triangle(Vec2 a, Vec2 b, Vec2 c, Color col) {
    const float u = (font::kWhiteX + 0.5f) / font::kAtlasW, v = (font::kWhiteY + 0.5f) / font::kAtlasH;
    vert(a.x, a.y, u, v, col);
    vert(b.x, b.y, u, v, col);
    vert(c.x, c.y, u, v, col);
}

float UI::textWidth(const std::string& s) const {
    return s.empty() ? 0.0f : (float)(s.size() * font::kCellW * fs - fs);
}

std::string UI::fitText(const std::string& s, float maxWidth) const {
    if (textWidth(s) <= maxWidth) return s;
    std::string r = s;
    while (!r.empty() && textWidth(r + "..") > maxWidth) r.pop_back();
    return r + "..";
}

void UI::text(float x, float y, const std::string& s, Color c) {
    x = std::floor(x);
    y = std::floor(y);
    const float gw = (float)font::kGlyphW * fs, gh = (float)font::kGlyphH * fs;
    for (char ch : s) {
        int code = (unsigned char)ch;
        if (code < 32 || code > 126) code = '?';
        if (code != ' ') {
            int idx = code - 32;
            int col = idx % font::kCols, row = idx / font::kCols;
            float u0 = float(col * font::kCellW) / font::kAtlasW;
            float v0 = float(row * font::kCellH) / font::kAtlasH;
            float u1 = float(col * font::kCellW + font::kGlyphW) / font::kAtlasW;
            float v1 = float(row * font::kCellH + font::kGlyphH) / font::kAtlasH;
            quad(x, y, x + gw, y + gh, u0, v0, u1, v1, c);
        }
        x += (float)font::kCellW * fs;
    }
}

void UI::textIn(const Rect& r, const std::string& s, Color c, bool centered) {
    float pad = 2.0f * fs;
    std::string t = fitText(s, r.w - 2 * pad);
    float x = centered ? r.x + (r.w - textWidth(t)) * 0.5f : r.x + pad;
    float y = r.y + (r.h - glyphH()) * 0.5f;
    text(x, y, t, c);
}

void UI::header(const Rect& r, const std::string& label) {
    float y = r.y + (r.h - glyphH()) * 0.5f;
    text(r.x, y, label, theme::textDim);
    float lx = r.x + textWidth(label) + 4.0f * fs;
    if (lx < r.x + r.w) rect({lx, std::floor(r.y + r.h * 0.5f), r.x + r.w - lx, (float)std::max(1, fs / 2)}, theme::border);
}

// --- widgets -----------------------------------------------------------------

bool UI::button(uint32_t id, const Rect& r, const std::string& label, bool toggled) {
    bool hot = hovered(r);
    bool clicked = false;
    if (hot && in_->mousePressed[MOUSE_LEFT] && activeId_ == 0) activeId_ = id;
    bool held = activeId_ == id;
    if (held) {
        activeSeen_ = true;
        if (!in_->mouseDown[MOUSE_LEFT]) {
            clicked = hot;
            activeId_ = 0;
        }
    }
    Color bg = toggled ? theme::accent : theme::button;
    if (held && hot) bg = toggled ? lighten(theme::accent, -0.08f) : theme::buttonPress;
    else if (hot) bg = toggled ? lighten(theme::accent, 0.07f) : theme::buttonHover;
    rect(r, bg);
    textIn(r, label, toggled ? theme::white : theme::text, true);
    return clicked;
}

bool UI::selectable(uint32_t id, const Rect& r, const std::string& label, bool selected, bool active) {
    (void)id;
    bool hot = hovered(r);
    bool clicked = hot && in_->mousePressed[MOUSE_LEFT] && activeId_ == 0;
    if (active) rect(r, theme::accent);
    else if (selected) rect(r, withAlpha(theme::accent, 0.45f));
    else if (hot) rect(r, theme::buttonHover);
    textIn(r, label, (active || selected) ? theme::white : theme::text, false);
    return clicked;
}

bool UI::swatch(uint32_t id, const Rect& r, Color c, bool selected) {
    bool hot = hovered(r);
    bool clicked = false;
    if (hot && in_->mousePressed[MOUSE_LEFT] && activeId_ == 0) activeId_ = id;
    if (activeId_ == id) {
        activeSeen_ = true;
        if (!in_->mouseDown[MOUSE_LEFT]) {
            clicked = hot;
            activeId_ = 0;
        }
    }
    rect(r, c);
    if (selected) border(r, theme::white, (float)fs);
    else if (hot) border(r, withAlpha(theme::white, 0.5f), (float)fs);
    return clicked;
}

void UI::editKeys(std::string& buf, bool numeric) {
    keyboardUsed_ = true;
    for (char c : in_->text) {
        if (buf.size() >= 120) break;
        if (numeric && !(std::isdigit((unsigned char)c) || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E'))
            continue;
        buf.push_back(c);
    }
    if (in_->keyRepeat[KEY_BACKSPACE] && !buf.empty()) buf.pop_back();
}

bool UI::dragFloat(uint32_t id, const Rect& r, float& value, float speed, Color accentColor, float lo, float hi) {
    bool hot = hovered(r);
    bool changed = false;

    if (editId_ == id) {  // typing a value
        editSeen_ = true;
        editKeys(editBuf_, true);
        bool clickedAway = in_->mousePressed[MOUSE_LEFT] && !r.contains(mx(), my());
        if (in_->keyPressed[KEY_ESCAPE]) {
            editId_ = 0;
        } else if (in_->keyPressed[KEY_ENTER] || in_->keyPressed[KEY_TAB] || clickedAway) {
            char* end = nullptr;
            float v = std::strtof(editBuf_.c_str(), &end);
            if (end != editBuf_.c_str() && std::isfinite(v)) {
                v = clampf(v, lo, hi);
                if (v != value) {
                    value = v;
                    changed = true;
                }
            }
            editId_ = 0;
        }
        if (editId_ == id) {
            rect(r, theme::panelDark);
            border(r, accentColor, (float)fs);
            float pad = 3.0f * fs;
            std::string shown = editBuf_;
            while (!shown.empty() && textWidth(shown) > r.w - 3 * pad) shown.erase(shown.begin());
            float y = r.y + (r.h - glyphH()) * 0.5f;
            text(r.x + pad, y, shown, theme::white);
            rect({std::floor(r.x + pad + textWidth(shown) + fs), y, (float)fs, glyphH()}, theme::white);
            return changed;
        }
    }

    if (hot && in_->mousePressed[MOUSE_LEFT] && activeId_ == 0) {
        activeId_ = id;
        dragStartX_ = mx();
        dragStartValue_ = value;
        dragMoved_ = false;
    }
    if (activeId_ == id) {
        activeSeen_ = true;
        if (!dragMoved_ && std::fabs(mx() - dragStartX_) > 3.0f) {
            dragMoved_ = true;
            dragStartX_ = mx();
        }
        if (dragMoved_) {
            float s = speed * (in_->shift() ? 0.1f : 1.0f);
            float v = clampf(dragStartValue_ + (mx() - dragStartX_) * s, lo, hi);
            if (v != value) {
                value = v;
                changed = true;
            }
        }
        if (!in_->mouseDown[MOUSE_LEFT]) {
            if (!dragMoved_) {  // plain click -> start typing
                editId_ = id;
                editBuf_ = editableNumber(value);
            }
            activeId_ = 0;
        }
    }

    Color bg = activeId_ == id ? theme::buttonPress : hot ? theme::buttonHover : theme::field;
    rect(r, bg);
    rect({r.x, r.y, (float)std::max(2, fs), r.h}, accentColor);
    std::string s;
    for (int d = 3; d >= 0; --d) {
        s = formatNumber(value, d);
        if (textWidth(s) <= r.w - 6.0f * fs) break;
    }
    textIn(r, s, theme::text, true);
    return changed;
}

bool UI::textField(uint32_t id, const Rect& r, std::string& value) {
    bool hot = hovered(r);
    bool committed = false;
    if (editId_ != id && hot && in_->mousePressed[MOUSE_LEFT] && activeId_ == 0) {
        editId_ = id;
        editBuf_ = value;
    }
    bool editing = editId_ == id;
    if (editing) {
        editSeen_ = true;
        editKeys(editBuf_, false);
        bool clickedAway = in_->mousePressed[MOUSE_LEFT] && !r.contains(mx(), my());
        if (in_->keyPressed[KEY_ESCAPE]) {
            editId_ = 0;
        } else if (in_->keyPressed[KEY_ENTER] || in_->keyPressed[KEY_TAB] || clickedAway) {
            value = editBuf_;
            committed = true;
            editId_ = 0;
        }
    }
    rect(r, editing ? theme::panelDark : hot ? theme::buttonHover : theme::field);
    if (editing && editId_ == id) border(r, theme::accent, (float)fs);
    float pad = 3.0f * fs;
    std::string shown = (editing && editId_ == id) ? editBuf_ : value;
    while (!shown.empty() && textWidth(shown) > r.w - 3 * pad) shown.erase(shown.begin());
    float y = r.y + (r.h - glyphH()) * 0.5f;
    text(r.x + pad, y, shown, theme::text);
    if (editing && editId_ == id)
        rect({std::floor(r.x + pad + textWidth(shown) + fs), y, (float)fs, glyphH()}, theme::white);
    return committed;
}
