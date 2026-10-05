// Path-traced rendering in the viewport (Render tab), and GPU utilisation
// reporting for both the raster preview and the path tracer.
#include "editor_internal.h"
#include "gl.h"
#include "image_io.h"
#include "jobs.h"
#include "profiler.h"

#include <cstdio>
#include <cstring>

using namespace ed;

namespace {
struct Hasher {
    uint64_t h = 1469598103934665603ull;
    void bytes(const void* p, size_t n) {
        const unsigned char* c = (const unsigned char*)p;
        for (size_t i = 0; i < n; ++i) h = (h ^ c[i]) * 1099511628211ull;
    }
    template <class T>
    void add(const T& v) {
        bytes(&v, sizeof v);
    }
};

bool isSoftwareRenderer(const std::string& r) {
    static const char* const kSoft[] = {"llvmpipe", "softpipe", "swrast", "GDI Generic", "Microsoft Basic Render",
                                        "SwiftShader"};
    for (const char* s : kSoft)
        if (r.find(s) != std::string::npos) return true;
    return false;
}
}  // namespace

void Editor::initGpuInfo() {
    auto str = [](GLenum e) {
        const GLubyte* s = gl::GetString(e);
        return s ? std::string((const char*)s) : std::string("?");
    };
    glVendor_ = str(GL_VENDOR);
    glRenderer_ = str(GL_RENDERER);
    glVersion_ = str(GL_VERSION);
    softwareGl_ = isSoftwareRenderer(glRenderer_);
    viewportTimer_.init();
    std::string err;
    gpuTracerOk_ = gpuTracer_.init(err);
    if (!gpuTracerOk_) {
        gpuTracerError_ = err;
        renderSet_.gpu = false;
    }
    // A software rasterizer (llvmpipe, Microsoft Basic Render, ...) runs the
    // "GPU" tracer on the CPU, far slower than the native multithreaded CPU
    // tracer: default to the CPU there (the user can still pick GPU).
    if (softwareGl_) {
        renderSet_.gpu = false;
        setStatus("Software OpenGL (" + glRenderer_.substr(0, 24) + "): no GPU acceleration - check graphics drivers",
                  true);
    }
}

void Editor::shutdownRender() {
    cpuRender_.stop();
    gpuTracer_.shutdown();
    viewportTimer_.shutdown();
}

uint64_t Editor::renderStamp() const {
    Hasher h;
    h.add(cam_.target);
    h.add(cam_.yaw);
    h.add(cam_.pitch);
    h.add(cam_.distance);
    h.add(cam_.fovY);
    h.add(cam_.ortho);
    h.add(viewport_.w);
    h.add(viewport_.h);
    h.add(renderSet_.gpu);
    h.add(renderSet_.bounces);
    h.add(renderSet_.resolution);
    h.add(renderSet_.clamp);
    h.add(renderSet_.envStrength);
    h.add(renderSet_.lightSize);
    h.add(renderSet_.studioLights);
    h.add(scene_.ambient);
    for (const Object& o : scene_.objects) {
        h.add(o.id);
        h.add(o.kind);
        h.add(o.parent);
        h.add(o.position);
        h.add(o.rotation);
        h.add(o.scale);
        if (o.isMesh()) {
            h.add(o.mesh.version);
            h.add(o.color);
            h.add(o.emission);
            h.add(o.emissionStrength);
            h.add(o.gloss);
            h.add(o.smooth);
        } else if (o.kind == ObjectKind::Light) {
            h.add(o.light);
        }
    }
    return h.h;
}

void Editor::renderSize(int& w, int& h) const {
    const float k = clampf(renderSet_.resolution, 5.0f, 200.0f) / 100.0f;
    w = std::max(8, (int)std::lround(viewport_.w * k));
    h = std::max(8, (int)std::lround(viewport_.h * k));
}

void Editor::startRender() {
    PROF_SCOPE("render start");
    cpuRender_.stop();
    gpuTracer_.stop();
    const int threads = renderSet_.cpuThreads >= 1 ? (int)renderSet_.cpuThreads : jobs::hardwareThreads();
    if (threads != jobs::threadCount()) jobs::setThreadCount(threads);

    rt::Settings s;
    s.maxBounces = (int)renderSet_.bounces;
    s.clampIndirect = renderSet_.clamp;
    s.envStrength = renderSet_.envStrength;
    s.lightSize = renderSet_.lightSize;
    s.studioLights = renderSet_.studioLights;
    const Vec3 r = camRight(), u = camUp(), back = -camForward();
    auto scene = std::make_shared<rt::SceneData>();
    rt::buildScene(scene_, s, *scene, normalize(r * -0.45f + u * 0.75f + back * 0.6f),
                   normalize(r * 0.7f - u * 0.15f + back * 0.4f));
    int w, h;
    renderSize(w, h);
    rt::View view = rt::makeView(cam_.eye(), cam_.target, cam_.fovY, cam_.ortho, cam_.orthoHalfHeight(),
                                 (float)w / (float)h);
    renderGpu_ = renderSet_.gpu && gpuTracerOk_;
    if (renderGpu_) gpuTracer_.start(*scene, view, s, w, h, (int)renderSet_.samples);
    else cpuRender_.start(scene, view, s, w, h, (int)renderSet_.samples);
    rtScene_ = scene;
    renderStamp_ = renderStamp();
}

void Editor::stopRender() {
    cpuRender_.stop();
    gpuTracer_.stop();
}

void Editor::toggleRenderView() {
    renderView_ = !renderView_;
    if (renderView_) {
        if (xf_ != Xform::None) endTransform(true);
        startRender();
        setStatus(strf("Rendering on the %s - %d samples (Render tab). Press again to return to the editor view.",
                       renderGpu_ ? "GPU" : "CPU", (int)renderSet_.samples));
    } else {
        stopRender();
        setStatus("Editor view");
    }
}

int Editor::renderSamples() const { return renderGpu_ ? gpuTracer_.samples() : cpuRender_.samples(); }
bool Editor::renderRunning() const { return renderGpu_ ? gpuTracer_.running() : cpuRender_.running(); }

void Editor::updateRender() {
    viewportTimer_.poll();
    if (!renderView_) return;
    // Interactive: any change to the camera / scene / settings restarts.
    if (renderAutoUpdate_ && renderStamp() != renderStamp_) startRender();
    gpuTracer_.setTargetSamples((int)renderSet_.samples);
    cpuRender_.setTargetSamples((int)renderSet_.samples);
    if (renderGpu_) {
        gpuTracer_.step(renderSet_.gpuBudgetMs);
    } else {
        // Resume a finished CPU render if the sample target was raised.
        cpuRender_.update();
        if (cpuRender_.takeImageDirty()) {
            PROF_SCOPE("render upload");
            gpuTracer_.uploadImage(cpuRender_.image(), cpuRender_.width(), cpuRender_.height());
        }
    }
    if (!renderOut_.empty() && !renderRunning()) {
        saveRender(renderOut_);
        exit_ = true;
    }
}

void Editor::presentRender(int vx, int vy, int vw, int vh) {
    if (renderGpu_) gpuTracer_.present(true, vx, vy, vw, vh);
    else if (gpuTracer_.hasImage()) gpuTracer_.present(false, vx, vy, vw, vh);
}

bool Editor::saveRender(const std::string& path) {
    std::vector<uint8_t> rgba;
    int w, h;
    if (renderGpu_) {
        gpuTracer_.readImage(rgba);
        w = gpuTracer_.width();
        h = gpuTracer_.height();
    } else {
        rgba = cpuRender_.image();
        w = cpuRender_.width();
        h = cpuRender_.height();
    }
    if (rgba.empty() || w <= 0 || h <= 0) {
        setStatus("Nothing rendered yet", true);
        return false;
    }
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                rgb[((size_t)(h - 1 - y) * w + x) * 3 + c] = rgba[((size_t)y * w + x) * 4 + c];
    bool ok = writePNG(path, w, h, rgb);
    // A small report next to the image (also used by --render runs).
    if (ok) {
        std::string report = path + ".txt";
        if (FILE* f = std::fopen(report.c_str(), "wb")) {
            std::fprintf(f, "device %s\nsize %dx%d\nsamples %d\nseconds %.3f\nsamples_per_second %.0f\n",
                         renderGpu_ ? "gpu" : "cpu", w, h, renderSamples(),
                         renderGpu_ ? gpuTracer_.elapsedSeconds() : cpuRender_.elapsedSeconds(),
                         renderGpu_ ? gpuTracer_.samplesPerSecond() : cpuRender_.samplesPerSecond());
            std::fprintf(f, "triangles %d\nbvh_nodes %d\nscene_build_ms %.2f\ncpu_threads %d\ngl_renderer %s\n",
                         rtScene_ ? (int)rtScene_->triangleCount() : 0, rtScene_ ? (int)rtScene_->bvh.nodes.size() : 0,
                         rtScene_ ? rtScene_->buildMs : 0.0, jobs::threadCount(), glRenderer_.c_str());
            if (renderGpu_) std::fprintf(f, "gpu_ms_per_frame %.2f\n", gpuTracer_.gpuMsPerFrame());
            std::fclose(f);
        }
    }
    setStatus(ok ? "Saved render to " + path : "Could not write " + path, !ok);
    return ok;
}

std::vector<std::string> Editor::gpuReport() const {
    std::vector<std::string> lines;
    lines.push_back("GPU " + glRenderer_.substr(0, 40));
    if (softwareGl_) lines.push_back("WARNING: software OpenGL!");
    const double frameMs = std::max(0.001, (double)(1000.0f / std::max(1.0f, fps_)));
    if (viewportTimer_.supported())
        lines.push_back(strf("GPU viewport %6.2f ms", viewportTimer_.avgMs()));
    if (renderView_) {
        if (renderGpu_) {
            double busy = gpuTracer_.gpuMsPerFrame() / frameMs * 100.0;
            lines.push_back(strf("GPU trace    %6.2f ms (%3.0f%%)", gpuTracer_.gpuMsPerFrame(), std::min(100.0, busy)));
            lines.push_back(strf("Trace %7.2f Msamples/s", gpuTracer_.samplesPerSecond() * 1e-6));
        } else {
            lines.push_back(strf("CPU trace %2d threads", jobs::threadCount()));
            lines.push_back(strf("Trace %7.2f Msamples/s", cpuRender_.samplesPerSecond() * 1e-6));
        }
    }
    return lines;
}
