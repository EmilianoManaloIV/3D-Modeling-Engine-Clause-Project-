#pragma once
// GPU side of the renderer:
//   GpuTimer  - asynchronous GL_TIME_ELAPSED queries, so the editor can show
//               how long the GPU actually spends on the viewport and on path
//               tracing (and therefore whether it is being kept busy) without
//               stalling the pipeline (GEA Vol. I sec. 10.8, profiling).
//   GpuTracer - the path tracer of pathtracer.h running in a fragment shader.
//               Triangles, BVH nodes, materials and lights are uploaded as
//               RGBA32F "data textures" (GLSL 3.30 has no storage buffers),
//               textures as one RGBA8 texture array; every draw adds one
//               sample to a band of rows of a float accumulation target
//               (additive blending, sample count in alpha). The band height
//               adapts to the measured GPU time so each frame keeps the GPU
//               ~busy for a set budget while the UI stays smooth, and no
//               single draw runs long enough to trip the OS GPU watchdog.
#include "pathtracer.h"

#include <cstdint>
#include <string>
#include <vector>

class GpuTimer {
public:
    void init();
    void shutdown();
    void begin();
    void end(double work = 0);  // `work` = units done inside (e.g. rows traced)
    void poll();                // collects finished queries
    bool supported() const { return ok_; }
    double lastMs() const { return lastMs_; }
    double avgMs() const { return avgMs_; }
    double lastWork() const { return lastWork_; }

private:
    static constexpr int kRing = 8;
    unsigned q_[kRing] = {};
    bool pending_[kRing] = {};
    double work_[kRing] = {};
    int next_ = 0, open_ = -1;
    bool ok_ = false;
    double lastMs_ = 0, avgMs_ = 0, lastWork_ = 0;
};

class GpuTracer {
public:
    bool init(std::string& error);
    void shutdown();
    bool ready() const { return prog_ != 0; }

    void start(const rt::SceneData& scene, const rt::View& view, const rt::Settings& settings, int w, int h,
               int targetSamples);
    void stop() {
        running_ = false;
        resumable_ = false;
    }
    // Traces for about `budgetMs` of GPU time (adaptive). Call once per frame.
    void step(double budgetMs);
    void setTargetSamples(int n) {  // raising it resumes a finished render
        target_ = n;
        if (!running_ && resumable_ && samples_ < target_) running_ = true;
    }

    bool running() const { return running_; }
    int samples() const { return samples_; }
    int width() const { return w_; }
    int height() const { return h_; }
    float exposure() const { return view_.exposure; }
    double elapsedSeconds() const;
    double samplesPerSecond() const;  // pixel samples per second
    double gpuMsPerFrame() const { return timer_.avgMs(); }
    unsigned accumTexture() const { return accumTex_; }
    // Reads back the accumulation buffer and tone-maps it (RGBA8, top row first).
    void readImage(std::vector<uint8_t>& rgba);
    // Linear (unexposed) average of a pixel (x, y from the top), for tests.
    Vec3 readPixel(int x, int y);
    size_t uploadedBytes() const { return uploadedBytes_; }

    // Display helpers (also used for CPU renders): upload an RGBA8 image, and
    // draw either it or the GPU accumulation buffer (with `exposure`) into a
    // GL viewport rect.
    void uploadImage(const std::vector<uint8_t>& rgba, int w, int h);
    void present(bool accumulated, int x, int y, int w, int h, float exposure = 1.0f);
    bool hasImage() const { return imageTex_ != 0; }

private:
    unsigned presentProg_ = 0, imageTex_ = 0;
    int imageW_ = 0, imageH_ = 0;
    struct {
        int image, accum, exposure;
    } presentU_{};
    unsigned uploadData(unsigned tex, const std::vector<float>& texels);
    unsigned prog_ = 0, vao_ = 0, fbo_ = 0, accumTex_ = 0;
    unsigned triTex_ = 0, nodeTex_ = 0, matTex_ = 0, lightTex_ = 0, texArray_ = 0;
    struct {
        int tris, nodes, mats, lights, textures, lightCount, anyPass, width, seed, size, eye, forward, right, up, tanHalf, orthoHalf,
            ortho, aspect, lensRadius, focusDist, blades, maxBounces, clamp, skyLow, skyHigh;
    } u_{};
    rt::View view_;
    rt::Settings settings_;
    int lightCount_ = 0;
    bool anyPass_ = false;
    Vec3 skyLow_, skyHigh_;
    int w_ = 0, h_ = 0, target_ = 0, samples_ = 0, row_ = 0;
    bool running_ = false, resumable_ = false;
    double startTime_ = 0, endTime_ = 0;
    double msPerRow_ = 0;  // measured GPU cost of one row-sample
    double drawnRowsTotal_ = 0;
    GpuTimer timer_;
    size_t uploadedBytes_ = 0;
};
