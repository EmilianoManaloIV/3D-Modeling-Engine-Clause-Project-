// Win32 + WGL implementation of the platform layer.
#ifdef _WIN32

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>

#include <cstdint>

#include "platform.h"

// Laptops with hybrid graphics (NVIDIA Optimus / AMD PowerXpress) run
// programs on the low-power integrated GPU unless the executable exports
// these symbols - ask for the discrete GPU so the viewport and the path
// tracer get the fast one.
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

namespace {

// WGL_ARB_pixel_format / WGL_ARB_multisample / WGL_ARB_create_context tokens.
constexpr int WGL_DRAW_TO_WINDOW_ARB = 0x2001;
constexpr int WGL_ACCELERATION_ARB = 0x2003;
constexpr int WGL_SUPPORT_OPENGL_ARB = 0x2010;
constexpr int WGL_DOUBLE_BUFFER_ARB = 0x2011;
constexpr int WGL_PIXEL_TYPE_ARB = 0x2013;
constexpr int WGL_COLOR_BITS_ARB = 0x2014;
constexpr int WGL_ALPHA_BITS_ARB = 0x201B;
constexpr int WGL_DEPTH_BITS_ARB = 0x2022;
constexpr int WGL_STENCIL_BITS_ARB = 0x2023;
constexpr int WGL_FULL_ACCELERATION_ARB = 0x2027;
constexpr int WGL_TYPE_RGBA_ARB = 0x202B;
constexpr int WGL_SAMPLE_BUFFERS_ARB = 0x2041;
constexpr int WGL_SAMPLES_ARB = 0x2042;
constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
constexpr int WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092;
constexpr int WGL_CONTEXT_PROFILE_MASK_ARB = 0x9126;
constexpr int WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x0001;

typedef BOOL(WINAPI* ChoosePixelFormatARBFn)(HDC, const int*, const FLOAT*, UINT, int*, UINT*);
typedef HGLRC(WINAPI* CreateContextAttribsARBFn)(HDC, HGLRC, const int*);
typedef BOOL(WINAPI* SwapIntervalEXTFn)(int);

const char* kClassName = "Modeler3DWindowClass";
HINSTANCE g_instance = nullptr;
HWND g_hwnd = nullptr;
HDC g_hdc = nullptr;
HGLRC g_glrc = nullptr;
HMODULE g_opengl32 = nullptr;
Input* g_input = nullptr;
bool g_quit = false;
int g_width = 0, g_height = 0;
float g_dpi = 1.0f;
SwapIntervalEXTFn g_swapInterval = nullptr;

int translateKey(WPARAM vk) {
    if (vk >= 'A' && vk <= 'Z') return (int)vk;
    if (vk >= '0' && vk <= '9') return (int)vk;
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return '0' + (int)(vk - VK_NUMPAD0);
    if (vk >= VK_F1 && vk <= VK_F12) return KEY_F1 + (int)(vk - VK_F1);
    switch (vk) {
        case VK_SPACE: return KEY_SPACE;
        case VK_ESCAPE: return KEY_ESCAPE;
        case VK_RETURN: return KEY_ENTER;
        case VK_TAB: return KEY_TAB;
        case VK_BACK: return KEY_BACKSPACE;
        case VK_DELETE: return KEY_DELETE;
        case VK_LEFT: return KEY_LEFT;
        case VK_RIGHT: return KEY_RIGHT;
        case VK_UP: return KEY_UP;
        case VK_DOWN: return KEY_DOWN;
        case VK_HOME: return KEY_HOME;
        case VK_END: return KEY_END;
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return KEY_SHIFT;
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return KEY_CONTROL;
        case VK_MENU: case VK_LMENU: case VK_RMENU: return KEY_ALT;
        default: return 0;
    }
}

void mouseButton(int button, bool down, LPARAM lp) {
    if (g_input) {
        g_input->onMouseMove((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp));
        g_input->onMouseButton(button, down);
    }
    if (down) {
        SetCapture(g_hwnd);
    } else if (!(GetKeyState(VK_LBUTTON) & 0x8000) && !(GetKeyState(VK_RBUTTON) & 0x8000) &&
               !(GetKeyState(VK_MBUTTON) & 0x8000)) {
        ReleaseCapture();
    }
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CLOSE:
            g_quit = true;
            return 0;
        case WM_SIZE:
            g_width = LOWORD(lp);
            g_height = HIWORD(lp);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_KILLFOCUS:
            if (g_input) g_input->releaseAll();
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            int k = translateKey(wp);
            if (g_input && k) g_input->onKey(k, true);
            // Let Windows see Alt+F4 & co.; swallow lone Alt / F10 so the
            // (non-existent) menu bar doesn't steal keyboard focus.
            if (msg == WM_SYSKEYDOWN && wp != VK_MENU && wp != VK_F10) break;
            return 0;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            int k = translateKey(wp);
            if (g_input && k) g_input->onKey(k, false);
            if (msg == WM_SYSKEYUP && wp != VK_MENU && wp != VK_F10) break;
            return 0;
        }
        case WM_SYSCHAR:
            return 0;  // no menu mnemonics; avoids the error beep on Alt+key
        case WM_CHAR:
            if (g_input && wp >= 32 && wp < 127) g_input->text.push_back((char)wp);
            return 0;
        case WM_MOUSEMOVE:
            if (g_input) g_input->onMouseMove((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp));
            return 0;
        case WM_LBUTTONDOWN: mouseButton(MOUSE_LEFT, true, lp); return 0;
        case WM_LBUTTONUP: mouseButton(MOUSE_LEFT, false, lp); return 0;
        case WM_RBUTTONDOWN: mouseButton(MOUSE_RIGHT, true, lp); return 0;
        case WM_RBUTTONUP: mouseButton(MOUSE_RIGHT, false, lp); return 0;
        case WM_MBUTTONDOWN: mouseButton(MOUSE_MIDDLE, true, lp); return 0;
        case WM_MBUTTONUP: mouseButton(MOUSE_MIDDLE, false, lp); return 0;
        case WM_MOUSEWHEEL:
            if (g_input) g_input->wheel += (float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA;
            return 0;
        default:
            break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

}  // namespace

namespace platform {

bool init(const char* title, int width, int height, std::string& error) {
    SetProcessDPIAware();
    g_instance = GetModuleHandleA(nullptr);

    WNDCLASSA wc{};
    wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = g_instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kClassName;
    if (!RegisterClassA(&wc)) {
        error = "RegisterClass failed";
        return false;
    }

    HDC screen = GetDC(nullptr);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSX) / 96.0f;
    ReleaseDC(nullptr, screen);
    if (g_dpi < 1.0f) g_dpi = 1.0f;

    // 1) A throwaway window + legacy context, needed only to fetch the WGL
    //    extension entry points that create a modern core-profile context.
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;

    HWND dummy = CreateWindowExA(0, kClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 32, 32, nullptr, nullptr, g_instance, nullptr);
    HDC dummyDC = GetDC(dummy);
    SetPixelFormat(dummyDC, ChoosePixelFormat(dummyDC, &pfd), &pfd);
    HGLRC dummyRC = wglCreateContext(dummyDC);
    wglMakeCurrent(dummyDC, dummyRC);
    auto choosePixelFormatARB = (ChoosePixelFormatARBFn)(void*)wglGetProcAddress("wglChoosePixelFormatARB");
    auto createContextAttribsARB = (CreateContextAttribsARBFn)(void*)wglGetProcAddress("wglCreateContextAttribsARB");
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(dummyRC);
    ReleaseDC(dummy, dummyDC);
    DestroyWindow(dummy);

    if (!createContextAttribsARB) {
        error = "Your graphics driver does not support OpenGL 3.3.\nPlease update your graphics driver.";
        return false;
    }

    // 2) The real window, sized for the monitor and centered.
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    width = std::min((int)(width * g_dpi), sw * 9 / 10);
    height = std::min((int)(height * g_dpi), sh * 85 / 100);
    DWORD style = WS_OVERLAPPEDWINDOW;
    RECT rc{0, 0, width, height};
    AdjustWindowRect(&rc, style, FALSE);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    g_hwnd = CreateWindowExA(0, kClassName, title, style, std::max(0, (sw - ww) / 2), std::max(0, (sh - wh) / 2), ww, wh,
                             nullptr, nullptr, g_instance, nullptr);
    if (!g_hwnd) {
        error = "CreateWindow failed";
        return false;
    }
    g_hdc = GetDC(g_hwnd);

    // 3) Pixel format: try 4x MSAA first, then without.
    int format = 0;
    if (choosePixelFormatARB) {
        for (int samples : {4, 0}) {
            const int attribs[] = {WGL_DRAW_TO_WINDOW_ARB, 1, WGL_SUPPORT_OPENGL_ARB, 1, WGL_DOUBLE_BUFFER_ARB, 1,
                                   WGL_ACCELERATION_ARB, WGL_FULL_ACCELERATION_ARB, WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
                                   WGL_COLOR_BITS_ARB, 32, WGL_ALPHA_BITS_ARB, 8, WGL_DEPTH_BITS_ARB, 24,
                                   WGL_STENCIL_BITS_ARB, 8, WGL_SAMPLE_BUFFERS_ARB, samples ? 1 : 0, WGL_SAMPLES_ARB,
                                   samples, 0};
            UINT count = 0;
            if (choosePixelFormatARB(g_hdc, attribs, nullptr, 1, &format, &count) && count > 0) break;
            format = 0;
        }
    }
    if (!format) format = ChoosePixelFormat(g_hdc, &pfd);
    PIXELFORMATDESCRIPTOR chosen{};
    DescribePixelFormat(g_hdc, format, sizeof(chosen), &chosen);
    if (!SetPixelFormat(g_hdc, format, &chosen)) {
        error = "SetPixelFormat failed";
        return false;
    }

    // 4) OpenGL 3.3 core profile context.
    const int ctxAttribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                              WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
    g_glrc = createContextAttribsARB(g_hdc, nullptr, ctxAttribs);
    if (!g_glrc) {
        error = "Could not create an OpenGL 3.3 context.\nPlease update your graphics driver.";
        return false;
    }
    wglMakeCurrent(g_hdc, g_glrc);
    g_swapInterval = (SwapIntervalEXTFn)(void*)wglGetProcAddress("wglSwapIntervalEXT");
    if (g_swapInterval) g_swapInterval(1);

    g_opengl32 = LoadLibraryA("opengl32.dll");

    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
    RECT client;
    GetClientRect(g_hwnd, &client);
    g_width = client.right - client.left;
    g_height = client.bottom - client.top;
    return true;
}

void shutdown() {
    if (g_glrc) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(g_glrc);
    }
    if (g_hwnd) {
        ReleaseDC(g_hwnd, g_hdc);
        DestroyWindow(g_hwnd);
    }
    if (g_opengl32) FreeLibrary(g_opengl32);
    g_glrc = nullptr;
    g_hwnd = nullptr;
}

void processEvents(Input& input) {
    g_input = &input;
    input.beginFrame();
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

bool quitRequested() { return g_quit; }
void clearQuitRequest() { g_quit = false; }
void swapBuffers() { SwapBuffers(g_hdc); }

void setVSync(bool on) {
    if (g_swapInterval) g_swapInterval(on ? 1 : 0);
}

void getFramebufferSize(int& w, int& h) {
    w = g_width;
    h = g_height;
}

float dpiScale() { return g_dpi; }

void* getProcAddress(const char* name) {
    PROC p = wglGetProcAddress(name);
    intptr_t v = (intptr_t)p;
    // wglGetProcAddress only knows extension/1.2+ functions; GL 1.1 entry
    // points come straight from opengl32.dll.
    if (v == 0 || v == 1 || v == 2 || v == 3 || v == -1) p = GetProcAddress(g_opengl32, name);
    return (void*)p;
}

void setTitle(const std::string& title) { SetWindowTextA(g_hwnd, title.c_str()); }

void showError(const std::string& message) { MessageBoxA(g_hwnd, message.c_str(), "Modeler3D", MB_ICONERROR | MB_OK); }

void sleepMs(int ms) { Sleep((DWORD)ms); }

}  // namespace platform

#endif  // _WIN32
