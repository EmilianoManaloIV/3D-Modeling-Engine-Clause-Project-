#pragma once
// Platform abstraction layer: window + OpenGL context + input.
// One implementation per OS (platform_win32.cpp, platform_x11.cpp), selected
// at build time - the "platform independence layer" of GEA Vol. I sec. 1.5.
// Input follows the HID model of GEA Vol. I ch. 9: raw OS events are folded
// into per-frame state (down / pressed-this-frame / released-this-frame).
#include <algorithm>
#include <iterator>
#include <string>

// Letters and digits use their uppercase ASCII codes ('A', '7', ...).
enum Key : int {
    KEY_SPACE = 32,
    KEY_ESCAPE = 256,
    KEY_ENTER,
    KEY_TAB,
    KEY_BACKSPACE,
    KEY_DELETE,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_UP,
    KEY_DOWN,
    KEY_HOME,
    KEY_END,
    KEY_SHIFT,
    KEY_CONTROL,
    KEY_ALT,
    KEY_F1,
    KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_COUNT = 512
};

enum MouseButton { MOUSE_LEFT = 0, MOUSE_RIGHT = 1, MOUSE_MIDDLE = 2 };

struct Input {
    float mouseX = 0, mouseY = 0;    // window pixels, origin top-left
    float mouseDX = 0, mouseDY = 0;  // motion this frame
    float wheel = 0;                 // notches this frame (+ = away from user)
    bool mouseDown[3] = {};
    bool mousePressed[3] = {};
    bool mouseReleased[3] = {};
    bool keyDown[KEY_COUNT] = {};
    bool keyPressed[KEY_COUNT] = {};  // first press only
    bool keyRepeat[KEY_COUNT] = {};   // first press + OS auto-repeat
    std::string text;                 // printable ASCII typed this frame
    bool hasMouse = false;

    bool shift() const { return keyDown[KEY_SHIFT]; }
    bool ctrl() const { return keyDown[KEY_CONTROL]; }
    bool alt() const { return keyDown[KEY_ALT]; }
    bool pressed(int k) const { return k > 0 && k < KEY_COUNT && keyPressed[k]; }

    void beginFrame() {
        mouseDX = mouseDY = wheel = 0;
        std::fill(std::begin(mousePressed), std::end(mousePressed), false);
        std::fill(std::begin(mouseReleased), std::end(mouseReleased), false);
        std::fill(std::begin(keyPressed), std::end(keyPressed), false);
        std::fill(std::begin(keyRepeat), std::end(keyRepeat), false);
        text.clear();
    }
    void onKey(int k, bool down) {
        if (k <= 0 || k >= KEY_COUNT) return;
        if (down) {
            if (!keyDown[k]) keyPressed[k] = true;
            keyRepeat[k] = true;
            keyDown[k] = true;
        } else {
            keyDown[k] = false;
        }
    }
    void onMouseButton(int b, bool down) {
        if (b < 0 || b > 2) return;
        if (down) {
            if (!mouseDown[b]) mousePressed[b] = true;
            mouseDown[b] = true;
        } else {
            if (mouseDown[b]) mouseReleased[b] = true;
            mouseDown[b] = false;
        }
    }
    void onMouseMove(float x, float y) {
        if (hasMouse) {
            mouseDX += x - mouseX;
            mouseDY += y - mouseY;
        }
        mouseX = x;
        mouseY = y;
        hasMouse = true;
    }
    // Called when the window loses focus so no key/button stays "stuck".
    void releaseAll() {
        std::fill(std::begin(keyDown), std::end(keyDown), false);
        for (int b = 0; b < 3; ++b) onMouseButton(b, false);
    }
};

namespace platform {
bool init(const char* title, int width, int height, std::string& error);
void shutdown();
void processEvents(Input& input);  // pumps the OS queue into `input`
bool quitRequested();              // window close button / Alt+F4
void clearQuitRequest();
void swapBuffers();
void setVSync(bool on);  // best effort; some drivers force their own setting
void getFramebufferSize(int& w, int& h);
float dpiScale();                  // 1.0 = 96 DPI
void* getProcAddress(const char* name);
void setTitle(const std::string& title);
void showError(const std::string& message);
void sleepMs(int ms);
}  // namespace platform
