#pragma once
// OpenGL 3.3 renderer. Follows the pipeline split of FoCG 5e ch. 9 / GEA
// Vol. II sec. 11.4: the CPU prepares vertex streams (cached per object and
// re-uploaded only when the mesh changes), vertex shaders transform them, and
// fragment shaders apply Blinn-Phong shading (FoCG 5e ch. 5).
#include "scene.h"
#include "ui.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct LineVertex {
    Vec3 pos;
    Color color;
};

struct FrameParams {
    Mat4 viewProj;
    Vec3 cameraPos;
    Vec3 keyLightDir;   // unit vector toward the light
    Vec3 fillLightDir;
};

class Renderer {
public:
    bool init(std::string& error);
    void shutdown();

    void clearWindow(int w, int h, Color c);
    void beginViewport(int x, int y, int w, int h, Color clear);

    void drawObject(const Object& o, const FrameParams& f, float highlight);
    void drawObjectEdges(const Object& o, const FrameParams& f, Color c);
    void drawLines(const std::vector<LineVertex>& v, const FrameParams& f, const Mat4& model, bool depthTest,
                   float fadeRadius = 0.0f, Vec3 fadeCenter = Vec3());
    void drawPoints(const std::vector<LineVertex>& v, const FrameParams& f, const Mat4& model, float size,
                    bool depthTest = true);
    void drawUI(const UI& ui, int screenW, int screenH);

    // Frees GPU buffers of objects that no longer exist.
    void purge(const Scene& scene);
    void readPixels(int w, int h, std::vector<uint8_t>& rgb);

private:
    struct GpuMesh {
        unsigned vao = 0, vbo = 0;
        int triVerts = 0;
        unsigned edgeVao = 0, edgeVbo = 0;
        int edgeVerts = 0;
        uint64_t version = ~0ull;
        bool smooth = false;
    };
    GpuMesh& gpuMesh(const Object& o);
    void drawLineList(const std::vector<LineVertex>& v, unsigned mode, const FrameParams& f, const Mat4& model,
                      bool depthTest, float fadeRadius, Vec3 fadeCenter, float pointSize);

    unsigned meshProg_ = 0, lineProg_ = 0, uiProg_ = 0;
    struct {
        int model, viewProj, normalMatrix, color, camPos, keyDir, fillDir, highlight;
    } meshU_{};
    struct {
        int model, viewProj, tint, fadeCenter, fadeRadius, pointSize, roundPoints;
    } lineU_{};
    struct {
        int screen, tex;
    } uiU_{};

    unsigned lineVao_ = 0, lineVbo_ = 0;
    unsigned uiVao_ = 0, uiVbo_ = 0;
    unsigned fontTex_ = 0;
    std::unordered_map<uint32_t, GpuMesh> cache_;
    std::vector<RenderVertex> scratch_;
    std::vector<LineVertex> scratchLines_;
};
