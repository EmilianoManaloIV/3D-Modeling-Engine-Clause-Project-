// Modeler3D - a small, dependency-free 3D modeling program (C++17 / OpenGL 3.3).
//
// Startup / shutdown order and the main loop follow GEA Vol. I sec. 6.1 and
// ch. 8: bring up the platform layer, then the graphics API, then the editor;
// each frame pumps OS events, updates, renders and swaps; tear down in reverse.
//
// Usage: Modeler3D [scene.m3d | model.obj]
// Testing flags: --demo <0-6>   build a sample scene (5 = booleans, 6 = materials)
//                --screenshot <file.png>   render a few frames, save, exit
//                --benchmark <report.md>   run the performance scenarios, write a report, exit
//                --frames <n>   frames to run before --screenshot (default 6)
//                --threads <n>  CPU worker threads (default: all hardware threads)
//                --render <out.png> [--device cpu|gpu|rtx] [--samples n] [--resolution pct] [--rt-warp]
//                               path-trace the scene, save it (+ out.png.txt stats), exit
#include "editor.h"
#include "gl.h"
#include "image_io.h"
#include "jobs.h"
#include "platform.h"
#include "profiler.h"

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
    int screenshotFrames = 6, threads = 0;
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
    if (!platform::init("Modeler3D", 1360, 860, error)) {
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
    if (benchmark) platform::setVSync(false);  // measure real frame cost, not the display rate
    while (!editor.shouldExit()) {
        prof::beginFrame();
        auto frameStart = std::chrono::steady_clock::now();
        platform::processEvents(input);
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
        editor.frame(input, w, h, dt);
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
        if (benchmark) continue;

        // Frame cap for systems where vsync is unavailable, so an idle editor
        // doesn't spin a CPU core at hundreds of frames per second.
        const float minFrame = 1.0f / 144.0f;
        float spent = std::chrono::duration<float>(std::chrono::steady_clock::now() - now).count();
        if (spent < minFrame) platform::sleepMs((int)((minFrame - spent) * 1000.0f) + 1);
    }

    editor.shutdown();
    platform::shutdown();
    jobs::shutdown();
    return 0;
}
