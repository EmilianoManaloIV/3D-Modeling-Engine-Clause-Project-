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
    Vec3 axisU, axisV;     // area light half extents
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
    float roughness = 0.5f, metallic = 0.0f;
    float opacity = 1.0f, transmission = 0.0f, ior = 1.45f, normalStrength = 1.0f;
    Vec2 uvScale{1, 1};
    unsigned textures[TEX_COUNT] = {};  // GL texture ids, 0 = none
    float highlight = 0;
    bool transparent() const { return opacity < 0.999f || transmission > 0.001f; }
};

// Compiles and links a vertex + fragment shader pair (0 on error, message in err).
unsigned linkGlProgram(const char* vsSrc, const char* fsSrc, std::string& err);

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

    // True when drawMesh / drawMeshEdges with this key would not need their
    // positions (GPU copy is current) - lets the caller skip CPU skinning.
    bool isCached(uint32_t id, uint64_t key) const;
    bool edgesCached(uint32_t id, uint64_t key) const;

    // Unity-style selection outline: render selected meshes into a mask
    // (value 1 = active, 0.5 = other selected), then draw an outline around
    // the mask in the viewport. Cost is independent of mesh density.
    bool beginOutlineMask(int viewportW, int viewportH);
    void drawMeshMask(uint32_t cacheId, uint64_t cacheKey, const Mesh& mesh, const std::vector<Vec3>& positions,
                      bool smooth, int weightSlot, const Mat4& model, float value, const FrameParams& f);
    // (sx, sy, sw, sh): window-space scissor rectangle limiting the composite pass.
    void endOutlineMask(int vx, int vy, int vw, int vh, int sx, int sy, int sw, int sh, Color active, Color other,
                        float radiusPx);
    // Lines / points kept in a persistent buffer per `slot`, re-uploaded only
    // when `key` changes (edit-mode overlay of dense meshes).
    void drawLinesCached(int slot, uint64_t key, const std::vector<LineVertex>& v, const FrameParams& f,
                         const Mat4& model, bool depthTest, bool points, float pointSize);
    // Translucent triangles (3 vertices each) pulled slightly towards the
    // camera so they sit on top of the surface they highlight.
    void drawTrianglesCached(int slot, uint64_t key, const std::vector<LineVertex>& v, const FrameParams& f,
                             const Mat4& model, bool depthTest);
    // Edit-mode overlay: one position buffer (re-uploaded only when `key`
    // changes, e.g. every frame of a vertex drag) shared by index buffers of
    // edges / points / faces (re-uploaded only when their own key changes, i.e.
    // on selection or topology changes). A dense mesh drag then uploads 12
    // bytes per vertex instead of a coloured copy of every edge.
    void setEditPositions(uint64_t key, const std::vector<Vec3>& positions);
    void drawEditElements(int slot, uint64_t key, const std::vector<uint32_t>& indices, unsigned mode, Color color,
                          const FrameParams& f, const Mat4& model, bool depthTest, float size);

    // GL texture for an image file (loaded through the texture cache, mipmapped).
    // 0 if it cannot be loaded. Base color / emission maps are sRGB-decoded in the shader.
    unsigned texture(const std::string& path);
    size_t textureCount() const { return textures_.size(); }

    // Frees GPU buffers of objects that no longer exist.
    void purge(const Scene& scene);
    void readPixels(int w, int h, std::vector<uint8_t>& rgb);

private:
    struct GpuMesh {
        unsigned vao = 0, vbo = 0, ebo = 0;
        int triVerts = 0;  // index count
        unsigned edgeVao = 0, edgeVbo = 0;
        int edgeVerts = 0;
        uint64_t key = ~0ull;
        uint64_t edgeKey = ~0ull;                 // edge buffer is built lazily (only when drawn)
        uint64_t edgeTopology = ~0ull;            // edge list depends on topology only
        std::vector<std::pair<int, int>> edges;
        // Small meshes live in the shared pool (vao/vbo/ebo stay 0): a range of
        // vertices and indices, drawn with a base vertex.
        bool pooled = false;
        uint32_t vOff = 0, vCap = 0, iOff = 0, iCap = 0;
        // Position-only refresh (same topology / smoothing / weight slot).
        RenderMap map;
        uint64_t topology = ~0ull;
        bool smooth = false;
        int weightSlot = -2;
        size_t vertexCount = 0, renderVertexCount = 0;
    };
    // Shared buffers for small meshes. One VAO + VBO + EBO per object cost the
    // driver ~100 KB each (50k small objects took 5 GB and paged); now small
    // meshes are ranges of a few big buffers.
    struct RangeAlloc {
        std::vector<std::pair<uint32_t, uint32_t>> free;  // (offset, size), sorted by offset
        uint32_t capacity = 0;
        bool alloc(uint32_t n, uint32_t& off);
        void release(uint32_t off, uint32_t n);
        void grow(uint32_t newCapacity);
    };
    struct MeshPool {
        unsigned vao = 0, vbo = 0, ebo = 0;
        RangeAlloc verts, indices;
    } pool_;
    static constexpr uint32_t kPoolMaxVerts = 16384;  // bigger meshes keep their own buffers
    void poolGrow(bool vertices, uint32_t need);
    void poolUpload(GpuMesh& g);
    void poolFree(GpuMesh& g);
    void releaseMesh(GpuMesh& g);
    void drawGpuMesh(const GpuMesh& g);
    void setupMeshAttribs();
    // Edit overlay buffers
    unsigned editVao_ = 0, editVbo_ = 0;
    uint64_t editPosKey_ = ~0ull;
    struct EditBatch {
        unsigned ebo = 0;
        uint64_t key = ~0ull;
        int count = 0;
    };
    std::unordered_map<int, EditBatch> editBatches_;
    // Per-frame state of the mesh program, set once per frame instead of per object.
    uint64_t frameSerial_ = 1, meshFrameSerial_ = 0;
    const FrameParams* meshFrameParams_ = nullptr;
    unsigned boundTex_[TEX_COUNT] = {};
    struct LineBatch {
        unsigned vao = 0, vbo = 0;
        uint64_t key = ~0ull;
        int count = 0;
    };
    std::unordered_map<int, LineBatch> lineBatches_;
    void drawBound(unsigned vao, int count, unsigned mode, const FrameParams& f, const Mat4& model, bool depthTest,
                   float fadeRadius, Vec3 fadeCenter, float pointSize);
    GpuMesh& gpuMesh(uint32_t id, uint64_t key, const Mesh& mesh, const std::vector<Vec3>& positions, bool smooth,
                     int weightSlot);
    void drawLineList(const std::vector<LineVertex>& v, unsigned mode, const FrameParams& f, const Mat4& model,
                      bool depthTest, float fadeRadius, Vec3 fadeCenter, float pointSize);

    unsigned meshProg_ = 0, lineProg_ = 0, uiProg_ = 0, particleProg_ = 0, maskProg_ = 0, outlineProg_ = 0;
    unsigned maskFbo_ = 0, maskTex_ = 0, emptyVao_ = 0;
    int maskW_ = 0, maskH_ = 0;
    bool maskOk_ = false;
    struct {
        int model, viewProj, value;
    } maskU_{};
    struct {
        int mask, offset, size, radius, active, other;
    } outlineU_{};
    struct {
        int model, viewProj, normalMatrix, color, emission, roughness, metallic, opacity, transmission, ior,
            normalStrength, uvScale, texMask, camPos, keyDir, fillDir, highlight, mode, ambient, lightCount, checker;
        int tex[TEX_COUNT];
        int lightPos[kMaxLights], lightDir[kMaxLights], lightColor[kMaxLights], lightParams[kMaxLights],
            lightU[kMaxLights], lightV[kMaxLights];
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
    struct GpuTexture {
        unsigned id = 0;
        const void* image = nullptr;  // identity of the cached image it was made from
    };
    std::unordered_map<std::string, GpuTexture> textures_;
    std::vector<RenderVertex> scratch_;
    std::vector<uint32_t> scratchIndices_;
    std::vector<LineVertex> scratchLines_;
};
