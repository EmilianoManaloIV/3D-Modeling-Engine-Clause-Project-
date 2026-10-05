// Modeler3D - a small, dependency-free 3D modeling program (C++17 / OpenGL 3.3).
//
// Startup / shutdown order and the main loop follow GEA Vol. I sec. 6.1 and
// ch. 8: bring up the platform layer, then the graphics API, then the editor;
// each frame pumps OS events, updates, renders and swaps; tear down in reverse.
//
// Usage: Modeler3D [scene.m3d | model.obj]
// Testing flags: --demo <0-7>   build a sample scene (5 = booleans, 6 = materials, 7 = modeling tools)
//                --screenshot <file.png>   render a few frames, save, exit
//                --benchmark <report.md>   run the performance scenarios, write a report, exit
//                --frames <n>   frames to run before --screenshot (default 6)
//                --threads <n>  CPU worker threads (default: all hardware threads)
//                --only <n,m,...>  with --benchmark: run only these scenarios
//                --size <w>x<h> window size (default 1360x860)
//                --script <file>  replay scripted input (see script.h), exit when done;
//                               the exit code is the number of failed checks
//                --render <out.png> [--device cpu|gpu|rtx] [--samples n] [--resolution pct] [--rt-warp]
//                               path-trace the scene, save it (+ out.png.txt stats), exit
#include "editor.h"
#include "gl.h"
#include "image_io.h"
#include "jobs.h"
#include "platform.h"
#include "profiler.h"
#include "script.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    AppOptions options;
    std::string screenshotPath;
    int screenshotFrames = 6, threads = 0, winW = 1360, winH = 860;
    std::string scriptPath;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--screenshot" && i + 1 < argc) screenshotPath = argv[++i];
        else if (a == "--demo" && i + 1 < argc) options.demo = std::atoi(argv[++i]);
        else if (a == "--frames" && i + 1 < argc) screenshotFrames = std::max(1, std::atoi(argv[++i]));
        else if (a == "--threads" && i + 1 < argc) threads = std::atoi(argv[++i]);
        else if (a == "--render" && i + 1 < argc) options.renderOut = argv[++i];
        else if (a == "--device" && i + 1 < argc) options.renderDevice = argv[++i];
        else if (a == "--rt-warp") options.allowWarp = true;
        else if (a == "--samples" && i + 1 < argc) options.renderSamples = std::atoi(argv[++i]);
        else if (a == "--resolution" && i + 1 < argc) options.renderPercent = std::atoi(argv[++i]);
        else if (a == "--help-overlay") options.showHelp = true;
        else if (a == "--benchmark" && i + 1 < argc) options.benchmarkReport = argv[++i];
        else if (a == "--script" && i + 1 < argc) scriptPath = argv[++i];
        else if (a == "--only" && i + 1 < argc) {  // --benchmark r.md --only 5,13
            for (const char* p = argv[++i]; *p;) {
                options.benchmarkOnly.push_back(std::atoi(p));
                while (*p && *p != ',') ++p;
                if (*p == ',') ++p;
            }
        }
        else if (a == "--size" && i + 1 < argc) {
            int w = 0, h = 0;
            if (std::sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w >= 320 && h >= 240) winW = w, winH = h;
        }
        else if (!a.empty() && a[0] != '-') options.openPath = a;
    }

    // Settings live next to the executable (argv[0]).
    {
        std::string exe = argc > 0 ? argv[0] : "";
        size_t slash = exe.find_last_of("/\\");
        options.configPath = (slash == std::string::npos ? std::string() : exe.substr(0, slash + 1)) + "Modeler3D.cfg";
    }

    jobs::init(threads);  // CPU thread pool (GEA Vol. I sec. 8.6, job systems)

    std::string error;
    ScriptPlayer script;
    if (!scriptPath.empty() && !script.load(scriptPath, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    if (!platform::init("Modeler3D", winW, winH, error)) {
        platform::showError(error);
        return 1;
    }
    std::string missing;
    if (!gl::load(missing)) {
        platform::showError("Required OpenGL functions are missing:\n" + missing);
        platform::shutdown();
        return 1;
    }

    Editor editor;
    if (!editor.init(options, error)) {
        platform::showError(error);
        platform::shutdown();
        return 1;
    }

    Input input;
    auto last = std::chrono::steady_clock::now();
    int frameCount = 0;
    const bool benchmark = !options.benchmarkReport.empty();
    if (benchmark || !scriptPath.empty()) platform::setVSync(false);  // measure real frame cost, not the display rate
    while (!editor.shouldExit()) {
        prof::beginFrame();
        auto frameStart = std::chrono::steady_clock::now();
        platform::processEvents(input);
        int fw = 0, fh = 0;
        platform::getFramebufferSize(fw, fh);
        if (!scriptPath.empty() && !script.step(input, editor, std::max(1, fw), std::max(1, fh))) break;
        if (platform::quitRequested()) {
            platform::clearQuitRequest();
            editor.requestQuit();
        }
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;

        int w = 0, h = 0;
        platform::getFramebufferSize(w, h);
        if (w <= 0 || h <= 0) {  // minimized
            platform::sleepMs(30);
            continue;
        }
        editor.frame(input, w, h, scriptPath.empty() ? dt : 1.0f / 60.0f);  // scripts: fixed time step
        if (!scriptPath.empty()) script.afterFrame(editor, w, h);
        if (benchmark) {  // include the GPU's work in the measured frame
            PROF_SCOPE("gpu finish");
            gl::Finish();
        }

        if (!screenshotPath.empty() && ++frameCount == screenshotFrames) {
            std::vector<uint8_t> pixels;
            editor.readPixels(w, h, pixels);
            int rc = writePNG(screenshotPath, w, h, pixels) ? 0 : 2;
            editor.shutdown();
            platform::shutdown();
            jobs::shutdown();
            return rc;
        }
        {
            PROF_SCOPE("present (swap)");
            platform::swapBuffers();
        }
        prof::add("frame total", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count());
        prof::endFrame();
        if (benchmark || !scriptPath.empty()) continue;

        // Frame cap for systems where vsync is unavailable, so an idle editor
        // doesn't spin a CPU core at hundreds of frames per second.
        const float minFrame = 1.0f / 144.0f;
        float spent = std::chrono::duration<float>(std::chrono::steady_clock::now() - now).count();
        if (spent < minFrame) platform::sleepMs((int)((minFrame - spent) * 1000.0f) + 1);
    }

    editor.shutdown();
    platform::shutdown();
    jobs::shutdown();
    if (!scriptPath.empty()) {
        std::printf("script: %d frames, %d failure(s)\n", script.frame(), script.failures());
        return script.failures() ? 3 : 0;
    }
    return 0;
}
