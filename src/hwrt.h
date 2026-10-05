#pragma once
// Hardware ray tracing ("RTX"): the path tracer on DirectX 12 with DXR 1.1
// inline ray tracing. Ray / scene intersection runs on the GPU's dedicated
// ray-tracing units (NVIDIA RTX, AMD RDNA 2+, Intel Arc) against an
// acceleration structure the driver builds; shading is the same algorithm
// as the CPU and GLSL tracers (shaders/hwrt.hlsl).
//
// Windows only (D3D12 is loaded at run time, so the program still starts on
// systems without it). The adapter is the first GPU reporting raytracing
// tier 1.1; Microsoft's software WARP adapter (also DXR 1.1, but on the CPU)
// can be allowed for testing.
//
// The renderer never blocks the UI thread: each frame it submits a GPU-time
// budget's worth of sample bands if the previous submission has finished,
// and a few times a second copies a tone-mapped RGBA8 image back for display.
#include "pathtracer.h"

#include <memory>
#include <string>
#include <vector>

struct HwRtInfo {
    bool available = false;   // a DXR 1.1 adapter was found (hardware, or WARP if allowed)
    bool software = false;    // it is WARP (CPU emulation)
    std::string adapter;      // adapter name
    std::string reason;       // why it is not available
    std::vector<std::string> adapters;  // every adapter with its raytracing tier, for the UI
};

class HwRayTracer {
public:
    HwRayTracer();
    ~HwRayTracer();
    // Finds an adapter (cached after the first call unless allowWarp changes).
    static HwRtInfo probe(bool allowWarp);

    bool init(bool allowWarp, std::string& error);
    void shutdown();
    bool ready() const;
    const HwRtInfo& info() const { return info_; }

    bool start(const rt::SceneData& scene, const rt::View& view, const rt::Settings& settings, int w, int h,
               int targetSamples, std::string& error);
    void stop();
    void step(double budgetMs);  // non-blocking; call once per frame
    void setTargetSamples(int n);
    bool running() const;
    int samples() const;
    int width() const;
    int height() const;
    double elapsedSeconds() const;
    double samplesPerSecond() const;
    double gpuMsPerSubmit() const;  // measured time of the last finished submission
    // New tone-mapped RGBA8 image (top row first) since the last call?
    bool takeImage(std::vector<uint8_t>& rgba);
    // Blocks until all submitted work is done and the image is current (tests, saving).
    void finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    HwRtInfo info_;
};
