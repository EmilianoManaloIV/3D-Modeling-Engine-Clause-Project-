// X11 + GLX implementation of the platform layer (Linux / BSD).
#ifndef _WIN32

#include "platform.h"

#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <GL/glx.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#ifndef GLX_CONTEXT_MAJOR_VERSION_ARB
#define GLX_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB 0x2092
#endif
#ifndef GLX_CONTEXT_PROFILE_MASK_ARB
#define GLX_CONTEXT_PROFILE_MASK_ARB 0x9126
#define GLX_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#endif

namespace {

typedef GLXContext (*CreateContextAttribsFn)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
typedef void (*SwapIntervalEXTFn)(Display*, GLXDrawable, int);
typedef int (*SwapIntervalMESAFn)(unsigned int);

Display* g_display = nullptr;
Window g_window = 0;
Colormap g_colormap = 0;
GLXContext g_context = nullptr;
Atom g_wmDelete = 0;
bool g_quit = false;
int g_width = 0, g_height = 0;
float g_dpi = 1.0f;
bool g_contextError = false;

int contextErrorHandler(Display*, XErrorEvent*) {
    g_contextError = true;
    return 0;
}

bool hasExtension(const char* list, const char* ext) {
    if (!list) return false;
    const size_t n = std::strlen(ext);
    for (const char* p = list; (p = std::strstr(p, ext)) != nullptr; p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) return true;
    return false;
}

int translateKeysym(KeySym ks) {
    if (ks >= XK_a && ks <= XK_z) return 'A' + (int)(ks - XK_a);
    if (ks >= XK_A && ks <= XK_Z) return 'A' + (int)(ks - XK_A);
    if (ks >= XK_0 && ks <= XK_9) return '0' + (int)(ks - XK_0);
    if (ks >= XK_KP_0 && ks <= XK_KP_9) return '0' + (int)(ks - XK_KP_0);
    if (ks >= XK_F1 && ks <= XK_F12) return KEY_F1 + (int)(ks - XK_F1);
    switch (ks) {
        // Keypad with NumLock off still maps to digits (view shortcuts).
        case XK_KP_Insert: return '0';
        case XK_KP_End: return '1';
        case XK_KP_Down: return '2';
        case XK_KP_Next: return '3';
        case XK_KP_Left: return '4';
        case XK_KP_Begin: return '5';
        case XK_KP_Right: return '6';
        case XK_KP_Home: return '7';
        case XK_KP_Up: return '8';
        case XK_KP_Prior: return '9';
        case XK_space: return KEY_SPACE;
        case XK_Escape: return KEY_ESCAPE;
        case XK_Return: case XK_KP_Enter: return KEY_ENTER;
        case XK_Tab: case XK_ISO_Left_Tab: return KEY_TAB;
        case XK_BackSpace: return KEY_BACKSPACE;
        case XK_Delete: case XK_KP_Delete: return KEY_DELETE;
        case XK_Left: return KEY_LEFT;
        case XK_Right: return KEY_RIGHT;
        case XK_Up: return KEY_UP;
        case XK_Down: return KEY_DOWN;
        case XK_Home: return KEY_HOME;
        case XK_End: return KEY_END;
        case XK_Shift_L: case XK_Shift_R: return KEY_SHIFT;
        case XK_Control_L: case XK_Control_R: return KEY_CONTROL;
        case XK_Alt_L: case XK_Alt_R: case XK_Meta_L: case XK_Meta_R: return KEY_ALT;
        default: return 0;
    }
}

int translateButton(unsigned int b) {
    switch (b) {
        case Button1: return MOUSE_LEFT;
        case Button2: return MOUSE_MIDDLE;
        case Button3: return MOUSE_RIGHT;
        default: return -1;
    }
}

}  // namespace

namespace platform {

bool init(const char* title, int width, int height, std::string& error) {
    g_display = XOpenDisplay(nullptr);
    if (!g_display) {
        error = "Cannot connect to the X server. Is a graphical session running (DISPLAY set)?";
        return false;
    }
    const int screen = DefaultScreen(g_display);

    if (const char* rm = XResourceManagerString(g_display)) {
        if (const char* p = std::strstr(rm, "Xft.dpi:")) {
            float dpi = std::strtof(p + 8, nullptr);
            if (dpi > 0) g_dpi = std::max(1.0f, dpi / 96.0f);
        }
    }

    int glxMajor = 0, glxMinor = 0;
    if (!glXQueryVersion(g_display, &glxMajor, &glxMinor) || (glxMajor == 1 && glxMinor < 3)) {
        error = "GLX 1.3 or newer is required.";
        return false;
    }

    // Framebuffer config: try 4x MSAA first, then without.
    GLXFBConfig config = nullptr;
    for (int samples : {4, 0}) {
        const int attribs[] = {GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, GLX_RENDER_TYPE, GLX_RGBA_BIT,
                               GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
                               GLX_ALPHA_SIZE, 8, GLX_DEPTH_SIZE, 24, GLX_STENCIL_SIZE, 8, GLX_DOUBLEBUFFER, True,
                               GLX_SAMPLE_BUFFERS, samples ? 1 : 0, GLX_SAMPLES, samples, None};
        int count = 0;
        GLXFBConfig* configs = glXChooseFBConfig(g_display, screen, attribs, &count);
        if (configs && count > 0) config = configs[0];
        if (configs) XFree(configs);
        if (config) break;
    }
    if (!config) {
        error = "No suitable OpenGL framebuffer configuration found.";
        return false;
    }

    XVisualInfo* vi = glXGetVisualFromFBConfig(g_display, config);
    if (!vi) {
        error = "glXGetVisualFromFBConfig failed.";
        return false;
    }
    Window root = RootWindow(g_display, vi->screen);
    g_colormap = XCreateColormap(g_display, root, vi->visual, AllocNone);

    XSetWindowAttributes swa{};
    swa.colormap = g_colormap;
    swa.border_pixel = 0;
    swa.background_pixmap = None;
    swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | StructureNotifyMask | FocusChangeMask;

    width = std::min((int)(width * g_dpi), DisplayWidth(g_display, screen) * 9 / 10);
    height = std::min((int)(height * g_dpi), DisplayHeight(g_display, screen) * 85 / 100);
    g_window = XCreateWindow(g_display, root, 0, 0, (unsigned)width, (unsigned)height, 0, vi->depth, InputOutput,
                             vi->visual, CWBorderPixel | CWColormap | CWEventMask, &swa);
    XFree(vi);
    if (!g_window) {
        error = "XCreateWindow failed.";
        return false;
    }
    XStoreName(g_display, g_window, title);
    XClassHint* hint = XAllocClassHint();
    if (hint) {
        hint->res_name = const_cast<char*>("modeler3d");
        hint->res_class = const_cast<char*>("Modeler3D");
        XSetClassHint(g_display, g_window, hint);
        XFree(hint);
    }
    g_wmDelete = XInternAtom(g_display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g_display, g_window, &g_wmDelete, 1);
    XMapWindow(g_display, g_window);

    // OpenGL 3.3 core profile context.
    const char* extensions = glXQueryExtensionsString(g_display, screen);
    auto createContext =
        (CreateContextAttribsFn)glXGetProcAddressARB((const GLubyte*)"glXCreateContextAttribsARB");
    if (!hasExtension(extensions, "GLX_ARB_create_context") || !createContext) {
        error = "GLX_ARB_create_context is not supported; OpenGL 3.3 is required.";
        return false;
    }
    const int ctxAttribs[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 3, GLX_CONTEXT_MINOR_VERSION_ARB, 3,
                              GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB, None};
    g_contextError = false;
    int (*oldHandler)(Display*, XErrorEvent*) = XSetErrorHandler(contextErrorHandler);
    g_context = createContext(g_display, config, nullptr, True, ctxAttribs);
    XSync(g_display, False);
    XSetErrorHandler(oldHandler);
    if (!g_context || g_contextError) {
        error = "Could not create an OpenGL 3.3 core context. Please update your graphics driver.";
        return false;
    }
    glXMakeCurrent(g_display, g_window, g_context);

    if (hasExtension(extensions, "GLX_EXT_swap_control")) {
        if (auto f = (SwapIntervalEXTFn)glXGetProcAddressARB((const GLubyte*)"glXSwapIntervalEXT"))
            f(g_display, g_window, 1);
    } else if (hasExtension(extensions, "GLX_MESA_swap_control")) {
        if (auto f = (SwapIntervalMESAFn)glXGetProcAddressARB((const GLubyte*)"glXSwapIntervalMESA")) f(1);
    }

    // Report key auto-repeat as repeated presses instead of release/press pairs.
    XkbSetDetectableAutoRepeat(g_display, True, nullptr);

    g_width = width;
    g_height = height;
    return true;
}

void shutdown() {
    if (!g_display) return;
    if (g_context) {
        glXMakeCurrent(g_display, None, nullptr);
        glXDestroyContext(g_display, g_context);
    }
    if (g_window) XDestroyWindow(g_display, g_window);
    if (g_colormap) XFreeColormap(g_display, g_colormap);
    XCloseDisplay(g_display);
    g_display = nullptr;
}

void processEvents(Input& input) {
    input.beginFrame();
    while (XPending(g_display)) {
        XEvent ev;
        XNextEvent(g_display, &ev);
        switch (ev.type) {
            case KeyPress: {
                int k = translateKeysym(XLookupKeysym(&ev.xkey, 0));
                if (k) input.onKey(k, true);
                char buf[32];
                KeySym sym;
                int n = XLookupString(&ev.xkey, buf, sizeof buf, &sym, nullptr);
                for (int i = 0; i < n; ++i)
                    if (buf[i] >= 32 && buf[i] < 127) input.text.push_back(buf[i]);
                break;
            }
            case KeyRelease: {
                int k = translateKeysym(XLookupKeysym(&ev.xkey, 0));
                if (k) input.onKey(k, false);
                break;
            }
            case ButtonPress:
                input.onMouseMove((float)ev.xbutton.x, (float)ev.xbutton.y);
                if (ev.xbutton.button == Button4) input.wheel += 1.0f;
                else if (ev.xbutton.button == Button5) input.wheel -= 1.0f;
                else input.onMouseButton(translateButton(ev.xbutton.button), true);
                break;
            case ButtonRelease:
                input.onMouseMove((float)ev.xbutton.x, (float)ev.xbutton.y);
                input.onMouseButton(translateButton(ev.xbutton.button), false);
                break;
            case MotionNotify:
                input.onMouseMove((float)ev.xmotion.x, (float)ev.xmotion.y);
                break;
            case ConfigureNotify:
                g_width = ev.xconfigure.width;
                g_height = ev.xconfigure.height;
                break;
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == g_wmDelete) g_quit = true;
                break;
            case FocusOut:
                input.releaseAll();
                break;
            default:
                break;
        }
    }
}

bool quitRequested() { return g_quit; }
void clearQuitRequest() { g_quit = false; }
void swapBuffers() { glXSwapBuffers(g_display, g_window); }

void getFramebufferSize(int& w, int& h) {
    w = g_width;
    h = g_height;
}

float dpiScale() { return g_dpi; }

void* getProcAddress(const char* name) { return (void*)glXGetProcAddressARB((const GLubyte*)name); }

void setTitle(const std::string& title) {
    if (g_display && g_window) XStoreName(g_display, g_window, title.c_str());
}

void showError(const std::string& message) { std::fprintf(stderr, "Modeler3D error: %s\n", message.c_str()); }

void sleepMs(int ms) {
    timespec ts{ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

}  // namespace platform

#endif  // !_WIN32
