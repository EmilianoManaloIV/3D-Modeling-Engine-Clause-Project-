#pragma once
// OpenGL 3.3 renderer. Follows the pipeline split of FoCG 5e ch. 9 / GEA
// Vol. II sec. 11.4: the CPU prepares vertex streams (cached per object and
// re-uploaded only when the mesh, pose or display mode changes), vertex
// shaders transform them, and fragment shaders shade them.
//
// Shading modes:
//   Studio  - fixed key/fill lights that follow the camera (modeling view)
//   Lit     - the scene's own lights + ambient (FoCG 5e ch. 5, GEA Vol. II ch. 12)
//   Checker - UV checker texture, to judge an unwrap (FoCG 5e ch. 11)
//   Weights - heat map of one bone's skin weights
// Emission is added in every mode except Weights.
#include "particles.h"
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

enum Shading { SHADE_STUDIO = 0, SHADE_LIT, SHADE_CHECKER, SHADE_WEIGHTS, SHADE_COUNT };
constexpr int kMaxLights = 8;

struct GpuLight {
    int type = 0;  // LightType
    Vec3 pos, dir, color;  // color already multiplied by intensity
    float range = 10, cosInner = 1, cosOuter = 0;
};

struct FrameParams {
    Mat4 viewProj;
    Vec3 cameraPos;
    Vec3 keyLightDir, fillLightDir;  // studio lights (unit vectors toward the light)
    int shading = SHADE_STUDIO;
    Vec3 ambient;
    std::vector<GpuLight> lights;
    float pointScale = 1;  // pixels per world unit at distance 1 (particle sizing)
};

struct MaterialParams {
    Vec3 color;
    Vec3 emission;  // color * strength
    float gloss = 0.5f;
    float highlight = 0;
};

class Renderer {
public:
    bool init(std::string& error);
    void shutdown();

    void clearWindow(int w, int h, Color c);
    void beginViewport(int x, int y, int w, int h, Color clear);

    // `cacheKey` must change whenever the drawn geometry would change.
    void drawMesh(uint32_t cacheId, uint64_t cacheKey, const Mesh& mesh, const std::vector<Vec3>& positions,
                  bool smooth, int weightSlot, const Mat4& model, const MaterialParams& mat, const FrameParams& f);
    void drawMeshEdges(uint32_t cacheId, uint64_t cacheKey, const Mesh& mesh, const std::vector<Vec3>& positions,
                       bool smooth, int weightSlot, const Mat4& model, Color c, const FrameParams& f);
    void drawLines(const std::vector<LineVertex>& v, const FrameParams& f, const Mat4& model, bool depthTest,
                   float fadeRadius = 0.0f, Vec3 fadeCenter = Vec3());
    void drawPoints(const std::vector<LineVertex>& v, const FrameParams& f, const Mat4& model, float size,
                    bool depthTest = true);
    void drawParticles(const std::vector<ParticleVertex>& v, const FrameParams& f, bool additive);
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
        uint64_t key = ~0ull;
    };
    GpuMesh& gpuMesh(uint32_t id, uint64_t key, const Mesh& mesh, const std::vector<Vec3>& positions, bool smooth,
                     int weightSlot);
    void drawLineList(const std::vector<LineVertex>& v, unsigned mode, const FrameParams& f, const Mat4& model,
                      bool depthTest, float fadeRadius, Vec3 fadeCenter, float pointSize);

    unsigned meshProg_ = 0, lineProg_ = 0, uiProg_ = 0, particleProg_ = 0;
    struct {
        int model, viewProj, normalMatrix, color, emission, gloss, camPos, keyDir, fillDir, highlight, mode, ambient,
            lightCount, checker;
        int lightPos[kMaxLights], lightDir[kMaxLights], lightColor[kMaxLights], lightParams[kMaxLights];
    } meshU_{};
    struct {
        int model, viewProj, tint, fadeCenter, fadeRadius, pointSize, roundPoints;
    } lineU_{};
    struct {
        int screen, tex;
    } uiU_{};
    struct {
        int viewProj, pointScale;
    } particleU_{};

    unsigned lineVao_ = 0, lineVbo_ = 0;
    unsigned uiVao_ = 0, uiVbo_ = 0;
    unsigned particleVao_ = 0, particleVbo_ = 0;
    unsigned fontTex_ = 0, checkerTex_ = 0;
    std::unordered_map<uint32_t, GpuMesh> cache_;
    std::vector<RenderVertex> scratch_;
    std::vector<LineVertex> scratchLines_;
};
