#include "renderer.h"

#include "font.h"
#include "gl.h"
#include "profiler.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <unordered_set>

namespace {

// --- Shaders -------------------------------------------------------------------
const char* kMeshVS = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in float aWeight;
uniform mat4 uModel;
uniform mat4 uViewProj;
uniform mat4 uNormalMatrix;
out vec3 vWorld;
out vec3 vNormal;
out vec2 vUV;
out float vWeight;
void main() {
    vec4 world = uModel * vec4(aPos, 1.0);
    vWorld = world.xyz;
    vNormal = mat3(uNormalMatrix) * aNormal;
    vUV = aUV;
    vWeight = aWeight;
    gl_Position = uViewProj * world;
}
)";

// Blinn-Phong (FoCG 5e sec. 5.2) evaluated in linear space and gamma-encoded
// at the end. Point/spot lights use a windowed inverse-square falloff.
const char* kMeshFS = R"(#version 330 core
#define MAX_LIGHTS 8
in vec3 vWorld;
in vec3 vNormal;
in vec2 vUV;
in float vWeight;
uniform vec3 uColor;
uniform vec3 uEmission;
uniform float uGloss;
uniform vec3 uCamPos;
uniform vec3 uKeyDir;
uniform vec3 uFillDir;
uniform float uHighlight;
uniform int uMode;
uniform vec3 uAmbient;
uniform int uLightCount;
uniform vec3 uLightPos[MAX_LIGHTS];
uniform vec3 uLightDir[MAX_LIGHTS];
uniform vec3 uLightColor[MAX_LIGHTS];
uniform vec4 uLightParams[MAX_LIGHTS];  // type, range, cos(inner), cos(outer)
uniform sampler2D uChecker;
out vec4 fragColor;

vec3 heat(float w) {
    return clamp(vec3(1.5 - abs(4.0 * w - 3.0), 1.5 - abs(4.0 * w - 2.0), 1.5 - abs(4.0 * w - 1.0)), 0.0, 1.0);
}

void main() {
    vec3 n = normalize(vNormal);
    if (!gl_FrontFacing) n = -n;               // two-sided lighting
    vec3 v = normalize(uCamPos - vWorld);
    vec3 base = pow(uColor, vec3(2.2));        // sRGB -> linear
    if (uMode == 2) base = pow(texture(uChecker, vUV).rgb, vec3(2.2));
    if (uMode == 3) base = pow(mix(vec3(0.05, 0.05, 0.35), heat(vWeight), step(0.001, vWeight)), vec3(2.2));
    float shininess = mix(6.0, 160.0, uGloss * uGloss);
    float specK = mix(0.03, 0.6, uGloss);
    vec3 c;
    if (uMode == 1) {
        c = base * uAmbient;
        for (int i = 0; i < MAX_LIGHTS; ++i) {
            if (i >= uLightCount) break;
            vec4 prm = uLightParams[i];
            vec3 L;
            float att = 1.0;
            if (prm.x > 0.5 && prm.x < 1.5) {          // sun
                L = -uLightDir[i];
            } else {                                     // point / spot
                vec3 d = uLightPos[i] - vWorld;
                float dist = length(d);
                L = d / max(dist, 1e-4);
                float x = clamp(1.0 - pow(dist / prm.y, 4.0), 0.0, 1.0);
                att = x * x / (dist * dist + 1.0);
                if (prm.x > 1.5) att *= smoothstep(prm.w, prm.z, dot(-L, uLightDir[i]));
            }
            float ndl = max(dot(n, L), 0.0);
            vec3 h = normalize(L + v);
            float spec = ndl > 0.0 ? pow(max(dot(n, h), 0.0), shininess) * specK : 0.0;
            c += (base * ndl + vec3(spec)) * uLightColor[i] * att;
        }
    } else {
        float key = max(dot(n, uKeyDir), 0.0);
        float fill = max(dot(n, uFillDir), 0.0);
        vec3 ambient = mix(vec3(0.07, 0.065, 0.06), vec3(0.17, 0.19, 0.23), n.y * 0.5 + 0.5);
        vec3 h = normalize(uKeyDir + v);
        float spec = key > 0.0 ? pow(max(dot(n, h), 0.0), shininess) * specK : 0.0;
        c = base * (ambient + key * vec3(1.0, 0.97, 0.92) * 0.95 + fill * vec3(0.55, 0.62, 0.75) * 0.30) + vec3(spec);
    }
    if (uMode != 3) c += uEmission;
    float rim = pow(1.0 - max(dot(n, v), 0.0), 3.0);
    c += vec3(1.0, 0.45, 0.08) * rim * uHighlight * 0.30;   // selection glow
    if (!gl_FrontFacing) c *= 0.6;                          // reveal inside / flipped faces
    // Soft shoulder: values below 0.8 are untouched, brighter ones approach 1
    // smoothly instead of clipping (bright lights / strong emission).
    c = mix(c, 0.8 + 0.2 * (1.0 - exp(-(c - 0.8) / 0.2)), step(0.8, c));
    fragColor = vec4(pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0);
}
)";

const char* kLineVS = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
uniform mat4 uModel;
uniform mat4 uViewProj;
uniform float uPointSize;
out vec4 vColor;
out vec3 vWorld;
void main() {
    vec4 world = uModel * vec4(aPos, 1.0);
    vWorld = world.xyz;
    vColor = aColor;
    gl_Position = uViewProj * world;
    gl_PointSize = uPointSize;
}
)";

const char* kLineFS = R"(#version 330 core
in vec4 vColor;
in vec3 vWorld;
uniform vec4 uTint;
uniform vec3 uFadeCenter;
uniform float uFadeRadius;
uniform int uRoundPoints;
out vec4 fragColor;
void main() {
    if (uRoundPoints == 1) {
        vec2 p = gl_PointCoord * 2.0 - 1.0;
        if (dot(p, p) > 1.0) discard;
    }
    vec4 c = vColor * uTint;
    if (uFadeRadius > 0.0) {
        float d = length(vWorld.xz - uFadeCenter.xz);
        c.a *= 1.0 - smoothstep(uFadeRadius * 0.4, uFadeRadius, d);
    }
    if (c.a <= 0.002) discard;
    fragColor = c;
}
)";

// Particles: camera-facing round point sprites sized in world units.
const char* kParticleVS = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in float aSize;
uniform mat4 uViewProj;
uniform float uPointScale;
out vec4 vColor;
void main() {
    gl_Position = uViewProj * vec4(aPos, 1.0);
    gl_PointSize = clamp(aSize * uPointScale / max(gl_Position.w, 1e-4), 1.0, 256.0);
    vColor = aColor;
}
)";

const char* kParticleFS = R"(#version 330 core
in vec4 vColor;
out vec4 fragColor;
void main() {
    vec2 p = gl_PointCoord * 2.0 - 1.0;
    float r2 = dot(p, p);
    if (r2 > 1.0) discard;
    float a = (1.0 - r2);
    fragColor = vec4(vColor.rgb, vColor.a * a * a);
}
)";

// Selection mask + outline composite.
const char* kMaskVS = R"(#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uModel;
uniform mat4 uViewProj;
void main() { gl_Position = uViewProj * (uModel * vec4(aPos, 1.0)); }
)";

const char* kMaskFS = R"(#version 330 core
uniform float uValue;
out vec4 fragColor;
void main() { fragColor = vec4(uValue, 0.0, 0.0, 1.0); }
)";

const char* kOutlineVS = R"(#version 330 core
void main() {  // one full-screen triangle, no vertex buffer
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kOutlineFS = R"(#version 330 core
uniform sampler2D uMask;
uniform ivec2 uOffset;
uniform ivec2 uSize;
uniform float uRadius;
uniform vec4 uActive;
uniform vec4 uOther;
out vec4 fragColor;
float maskAt(ivec2 p) { return texelFetch(uMask, clamp(p, ivec2(0), uSize - 1), 0).r; }
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy) - uOffset;
    if (maskAt(p) > 0.0) discard;              // outline only outside the silhouette
    float m = 0.0;
    for (int k = 0; k < 8; ++k) {          // 8 taps on the outline radius...
        float a = 6.2831853 * float(k) / 8.0;
        m = max(m, maskAt(p + ivec2(round(vec2(cos(a), sin(a)) * uRadius))));
    }
    for (int k = 0; k < 4; ++k) {          // ...and 4 halfway, so thin parts are caught
        float a = 6.2831853 * (float(k) + 0.5) / 4.0;
        m = max(m, maskAt(p + ivec2(round(vec2(cos(a), sin(a)) * uRadius * 0.5))));
    }
    if (m <= 0.0) discard;
    fragColor = m > 0.75 ? uActive : uOther;
}
)";

const char* kUiVS = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
uniform vec2 uScreen;
out vec2 vUV;
out vec4 vColor;
void main() {
    vUV = aUV;
    vColor = aColor;
    gl_Position = vec4(aPos.x / uScreen.x * 2.0 - 1.0, 1.0 - aPos.y / uScreen.y * 2.0, 0.0, 1.0);
}
)";

const char* kUiFS = R"(#version 330 core
in vec2 vUV;
in vec4 vColor;
uniform sampler2D uTex;
out vec4 fragColor;
void main() {
    fragColor = vec4(vColor.rgb, vColor.a * texture(uTex, vUV).r);
}
)";

GLuint compileShader(GLenum type, const char* src, std::string& err) {
    GLuint s = gl::CreateShader(type);
    gl::ShaderSource(s, 1, &src, nullptr);
    gl::CompileShader(s);
    GLint ok = 0;
    gl::GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        gl::GetShaderInfoLog(s, sizeof log, nullptr, log);
        err = std::string("Shader compile error:\n") + log;
        gl::DeleteShader(s);
        return 0;
    }
    return s;
}

GLuint linkProgram(const char* vsSrc, const char* fsSrc, std::string& err) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, vsSrc, err);
    if (!vs) return 0;
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSrc, err);
    if (!fs) {
        gl::DeleteShader(vs);
        return 0;
    }
    GLuint p = gl::CreateProgram();
    gl::AttachShader(p, vs);
    gl::AttachShader(p, fs);
    gl::LinkProgram(p);
    gl::DeleteShader(vs);
    gl::DeleteShader(fs);
    GLint ok = 0;
    gl::GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        gl::GetProgramInfoLog(p, sizeof log, nullptr, log);
        err = std::string("Shader link error:\n") + log;
        gl::DeleteProgram(p);
        return 0;
    }
    return p;
}

const void* offsetPtr(size_t bytes) { return reinterpret_cast<const void*>(bytes); }

void setupLineVao(GLuint vao, GLuint vbo) {
    gl::BindVertexArray(vao);
    gl::BindBuffer(GL_ARRAY_BUFFER, vbo);
    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(LineVertex), offsetPtr(0));
    gl::EnableVertexAttribArray(1);
    gl::VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(LineVertex), offsetPtr(sizeof(Vec3)));
}

// UV checker: 8x8 tiles, alternating light/dark, each tile tinted with its
// own hue so stretching, rotation and mirroring are easy to spot.
void buildChecker(std::vector<uint8_t>& px, int size) {
    px.resize(size_t(size) * size * 3);
    const int cell = size / 8;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            int cx = x / cell, cy = y / cell;
            float hue = (cx + cy * 8) / 64.0f * 6.0f;
            float r = clampf(std::fabs(hue - 3.0f) - 1.0f, 0, 1);
            float g = clampf(2.0f - std::fabs(hue - 2.0f), 0, 1);
            float b = clampf(2.0f - std::fabs(hue - 4.0f), 0, 1);
            float lum = ((cx + cy) & 1) ? 0.92f : 0.42f;
            float t = 0.35f;
            bool line = (x % cell) == 0 || (y % cell) == 0;
            float k = line ? 0.15f : 1.0f;
            uint8_t* p = &px[(size_t(y) * size + x) * 3];
            p[0] = (uint8_t)(255 * k * (lum * (1 - t) + r * lum * t));
            p[1] = (uint8_t)(255 * k * (lum * (1 - t) + g * lum * t));
            p[2] = (uint8_t)(255 * k * (lum * (1 - t) + b * lum * t));
        }
}

}  // namespace

bool Renderer::init(std::string& error) {
    meshProg_ = linkProgram(kMeshVS, kMeshFS, error);
    lineProg_ = meshProg_ ? linkProgram(kLineVS, kLineFS, error) : 0;
    uiProg_ = lineProg_ ? linkProgram(kUiVS, kUiFS, error) : 0;
    particleProg_ = uiProg_ ? linkProgram(kParticleVS, kParticleFS, error) : 0;
    maskProg_ = particleProg_ ? linkProgram(kMaskVS, kMaskFS, error) : 0;
    outlineProg_ = maskProg_ ? linkProgram(kOutlineVS, kOutlineFS, error) : 0;
    if (!meshProg_ || !lineProg_ || !uiProg_ || !particleProg_ || !maskProg_ || !outlineProg_) return false;

    auto loc = [](GLuint p, const char* name) { return gl::GetUniformLocation(p, name); };
    meshU_.model = loc(meshProg_, "uModel");
    meshU_.viewProj = loc(meshProg_, "uViewProj");
    meshU_.normalMatrix = loc(meshProg_, "uNormalMatrix");
    meshU_.color = loc(meshProg_, "uColor");
    meshU_.emission = loc(meshProg_, "uEmission");
    meshU_.gloss = loc(meshProg_, "uGloss");
    meshU_.camPos = loc(meshProg_, "uCamPos");
    meshU_.keyDir = loc(meshProg_, "uKeyDir");
    meshU_.fillDir = loc(meshProg_, "uFillDir");
    meshU_.highlight = loc(meshProg_, "uHighlight");
    meshU_.mode = loc(meshProg_, "uMode");
    meshU_.ambient = loc(meshProg_, "uAmbient");
    meshU_.lightCount = loc(meshProg_, "uLightCount");
    meshU_.checker = loc(meshProg_, "uChecker");
    for (int i = 0; i < kMaxLights; ++i) {
        char name[64];
        std::snprintf(name, sizeof name, "uLightPos[%d]", i);
        meshU_.lightPos[i] = loc(meshProg_, name);
        std::snprintf(name, sizeof name, "uLightDir[%d]", i);
        meshU_.lightDir[i] = loc(meshProg_, name);
        std::snprintf(name, sizeof name, "uLightColor[%d]", i);
        meshU_.lightColor[i] = loc(meshProg_, name);
        std::snprintf(name, sizeof name, "uLightParams[%d]", i);
        meshU_.lightParams[i] = loc(meshProg_, name);
    }

    lineU_.model = loc(lineProg_, "uModel");
    lineU_.viewProj = loc(lineProg_, "uViewProj");
    lineU_.tint = loc(lineProg_, "uTint");
    lineU_.fadeCenter = loc(lineProg_, "uFadeCenter");
    lineU_.fadeRadius = loc(lineProg_, "uFadeRadius");
    lineU_.pointSize = loc(lineProg_, "uPointSize");
    lineU_.roundPoints = loc(lineProg_, "uRoundPoints");

    uiU_.screen = loc(uiProg_, "uScreen");
    uiU_.tex = loc(uiProg_, "uTex");

    particleU_.viewProj = loc(particleProg_, "uViewProj");
    maskU_.model = loc(maskProg_, "uModel");
    maskU_.viewProj = loc(maskProg_, "uViewProj");
    maskU_.value = loc(maskProg_, "uValue");
    outlineU_.mask = loc(outlineProg_, "uMask");
    outlineU_.offset = loc(outlineProg_, "uOffset");
    outlineU_.size = loc(outlineProg_, "uSize");
    outlineU_.radius = loc(outlineProg_, "uRadius");
    outlineU_.active = loc(outlineProg_, "uActive");
    outlineU_.other = loc(outlineProg_, "uOther");
    gl::GenVertexArrays(1, &emptyVao_);
    particleU_.pointScale = loc(particleProg_, "uPointScale");

    gl::GenVertexArrays(1, &lineVao_);
    gl::GenBuffers(1, &lineVbo_);
    setupLineVao(lineVao_, lineVbo_);

    gl::GenVertexArrays(1, &uiVao_);
    gl::GenBuffers(1, &uiVbo_);
    gl::BindVertexArray(uiVao_);
    gl::BindBuffer(GL_ARRAY_BUFFER, uiVbo_);
    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(UI::Vertex), offsetPtr(0));
    gl::EnableVertexAttribArray(1);
    gl::VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(UI::Vertex), offsetPtr(2 * sizeof(float)));
    gl::EnableVertexAttribArray(2);
    gl::VertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(UI::Vertex), offsetPtr(4 * sizeof(float)));

    gl::GenVertexArrays(1, &particleVao_);
    gl::GenBuffers(1, &particleVbo_);
    gl::BindVertexArray(particleVao_);
    gl::BindBuffer(GL_ARRAY_BUFFER, particleVbo_);
    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(ParticleVertex), offsetPtr(0));
    gl::EnableVertexAttribArray(1);
    gl::VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(ParticleVertex), offsetPtr(3 * sizeof(float)));
    gl::EnableVertexAttribArray(2);
    gl::VertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(ParticleVertex), offsetPtr(7 * sizeof(float)));
    gl::BindVertexArray(0);

    std::vector<uint8_t> atlas;
    font::buildAtlas(atlas);
    gl::GenTextures(1, &fontTex_);
    gl::BindTexture(GL_TEXTURE_2D, fontTex_);
    gl::PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl::TexImage2D(GL_TEXTURE_2D, 0, GL_R8, font::kAtlasW, font::kAtlasH, 0, GL_RED, GL_UNSIGNED_BYTE, atlas.data());
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    std::vector<uint8_t> checker;
    const int checkerSize = 512;
    buildChecker(checker, checkerSize);
    gl::GenTextures(1, &checkerTex_);
    gl::BindTexture(GL_TEXTURE_2D, checkerTex_);
    gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, checkerSize, checkerSize, 0, GL_RGB, GL_UNSIGNED_BYTE, checker.data());
    gl::GenerateMipmap(GL_TEXTURE_2D);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    return true;
}

void Renderer::shutdown() {
    for (auto& kv : cache_) {
        GpuMesh& g = kv.second;
        gl::DeleteBuffers(1, &g.vbo);
        gl::DeleteBuffers(1, &g.ebo);
        gl::DeleteBuffers(1, &g.edgeVbo);
        gl::DeleteVertexArrays(1, &g.vao);
        gl::DeleteVertexArrays(1, &g.edgeVao);
    }
    cache_.clear();
    for (auto& kv : lineBatches_) {
        gl::DeleteBuffers(1, &kv.second.vbo);
        gl::DeleteVertexArrays(1, &kv.second.vao);
    }
    lineBatches_.clear();
    gl::DeleteBuffers(1, &lineVbo_);
    gl::DeleteBuffers(1, &uiVbo_);
    gl::DeleteBuffers(1, &particleVbo_);
    gl::DeleteVertexArrays(1, &lineVao_);
    gl::DeleteVertexArrays(1, &uiVao_);
    gl::DeleteVertexArrays(1, &particleVao_);
    gl::DeleteTextures(1, &fontTex_);
    gl::DeleteTextures(1, &checkerTex_);
    gl::DeleteProgram(meshProg_);
    gl::DeleteProgram(lineProg_);
    gl::DeleteProgram(uiProg_);
    gl::DeleteProgram(particleProg_);
    gl::DeleteProgram(maskProg_);
    gl::DeleteProgram(outlineProg_);
    if (maskFbo_) gl::DeleteFramebuffers(1, &maskFbo_);
    if (maskTex_) gl::DeleteTextures(1, &maskTex_);
    gl::DeleteVertexArrays(1, &emptyVao_);
}

void Renderer::clearWindow(int w, int h, Color c) {
    gl::Viewport(0, 0, w, h);
    gl::Disable(GL_SCISSOR_TEST);
    gl::ClearColor(c.r, c.g, c.b, 1.0f);
    gl::Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Renderer::beginViewport(int x, int y, int w, int h, Color c) {
    gl::Viewport(x, y, w, h);
    gl::Enable(GL_SCISSOR_TEST);
    gl::Scissor(x, y, w, h);
    gl::ClearColor(c.r, c.g, c.b, 1.0f);
    gl::Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    gl::Disable(GL_SCISSOR_TEST);
    gl::Enable(GL_DEPTH_TEST);
    gl::DepthFunc(GL_LEQUAL);
    gl::DepthMask(GL_TRUE);
    gl::Enable(GL_BLEND);
    gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl::Enable(GL_PROGRAM_POINT_SIZE);
    gl::Enable(GL_MULTISAMPLE);
    gl::Disable(GL_CULL_FACE);
}

Renderer::GpuMesh& Renderer::gpuMesh(uint32_t id, uint64_t key, const Mesh& mesh, const std::vector<Vec3>& positions,
                                     bool smooth, int weightSlot) {
    GpuMesh& g = cache_[id];
    if (!g.vao) {
        gl::GenVertexArrays(1, &g.vao);
        gl::GenBuffers(1, &g.vbo);
        gl::GenBuffers(1, &g.ebo);
        gl::BindVertexArray(g.vao);
        gl::BindBuffer(GL_ARRAY_BUFFER, g.vbo);
        gl::BindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ebo);  // part of the VAO state
        gl::EnableVertexAttribArray(0);
        gl::VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(RenderVertex), offsetPtr(offsetof(RenderVertex, pos)));
        gl::EnableVertexAttribArray(1);
        gl::VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(RenderVertex),
                                offsetPtr(offsetof(RenderVertex, normal)));
        gl::EnableVertexAttribArray(2);
        gl::VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(RenderVertex), offsetPtr(offsetof(RenderVertex, uv)));
        gl::EnableVertexAttribArray(3);
        gl::VertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(RenderVertex),
                                offsetPtr(offsetof(RenderVertex, weight)));
    }
    if (g.key != key) {
        PROF_SCOPE("mesh rebuild+upload");
        prof::count("mesh uploads", 1);
        buildRenderMesh(mesh, positions, smooth, 40.0f, weightSlot, scratch_, scratchIndices_);
        gl::BindVertexArray(g.vao);
        gl::BindBuffer(GL_ARRAY_BUFFER, g.vbo);
        gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(scratch_.size() * sizeof(RenderVertex)), scratch_.data(),
                       GL_STATIC_DRAW);
        gl::BindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ebo);
        gl::BufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(scratchIndices_.size() * sizeof(uint32_t)),
                       scratchIndices_.data(), GL_STATIC_DRAW);
        g.triVerts = (int)scratchIndices_.size();
        prof::count("uploaded vertices", (double)scratch_.size());
        g.key = key;
    }
    return g;
}

void Renderer::drawMesh(uint32_t id, uint64_t key, const Mesh& mesh, const std::vector<Vec3>& positions, bool smooth,
                        int weightSlot, const Mat4& model, const MaterialParams& mat, const FrameParams& f) {
    GpuMesh& g = gpuMesh(id, key, mesh, positions, smooth, weightSlot);
    if (g.triVerts == 0) return;
    Mat4 normalMatrix = transpose(inverse(model));  // normals transform by (M^-1)^T, FoCG 5e ch. 7
    gl::UseProgram(meshProg_);
    gl::UniformMatrix4fv(meshU_.model, 1, GL_FALSE, model.m);
    gl::UniformMatrix4fv(meshU_.viewProj, 1, GL_FALSE, f.viewProj.m);
    gl::UniformMatrix4fv(meshU_.normalMatrix, 1, GL_FALSE, normalMatrix.m);
    gl::Uniform3f(meshU_.color, mat.color.x, mat.color.y, mat.color.z);
    gl::Uniform3f(meshU_.emission, mat.emission.x, mat.emission.y, mat.emission.z);
    gl::Uniform1f(meshU_.gloss, mat.gloss);
    gl::Uniform3f(meshU_.camPos, f.cameraPos.x, f.cameraPos.y, f.cameraPos.z);
    gl::Uniform3f(meshU_.keyDir, f.keyLightDir.x, f.keyLightDir.y, f.keyLightDir.z);
    gl::Uniform3f(meshU_.fillDir, f.fillLightDir.x, f.fillLightDir.y, f.fillLightDir.z);
    gl::Uniform1f(meshU_.highlight, mat.highlight);
    gl::Uniform1i(meshU_.mode, f.shading);
    gl::Uniform3f(meshU_.ambient, f.ambient.x, f.ambient.y, f.ambient.z);
    int count = std::min((int)f.lights.size(), kMaxLights);
    gl::Uniform1i(meshU_.lightCount, count);
    for (int i = 0; i < count; ++i) {
        const GpuLight& L = f.lights[i];
        gl::Uniform3f(meshU_.lightPos[i], L.pos.x, L.pos.y, L.pos.z);
        gl::Uniform3f(meshU_.lightDir[i], L.dir.x, L.dir.y, L.dir.z);
        gl::Uniform3f(meshU_.lightColor[i], L.color.x, L.color.y, L.color.z);
        gl::Uniform4f(meshU_.lightParams[i], (float)L.type, std::max(0.01f, L.range), L.cosInner, L.cosOuter);
    }
    gl::Uniform1i(meshU_.checker, 1);
    gl::ActiveTexture(GL_TEXTURE0 + 1);
    gl::BindTexture(GL_TEXTURE_2D, checkerTex_);
    gl::ActiveTexture(GL_TEXTURE0);
    // Push filled polygons slightly back so wireframe lines drawn on top of
    // them win the depth test.
    gl::Enable(GL_POLYGON_OFFSET_FILL);
    gl::PolygonOffset(1.0f, 1.0f);
    gl::BindVertexArray(g.vao);
    gl::DrawElements(GL_TRIANGLES, g.triVerts, GL_UNSIGNED_INT, nullptr);
    prof::count("draw calls", 1);
    prof::count("triangles drawn", g.triVerts / 3);
    gl::Disable(GL_POLYGON_OFFSET_FILL);
}

void Renderer::drawMeshEdges(uint32_t id, uint64_t key, const Mesh& mesh, const std::vector<Vec3>& positions,
                             bool smooth, int weightSlot, const Mat4& model, Color c, const FrameParams& f) {
    GpuMesh& g = cache_[id];
    if (!g.edgeVao) {
        gl::GenVertexArrays(1, &g.edgeVao);
        gl::GenBuffers(1, &g.edgeVbo);
        setupLineVao(g.edgeVao, g.edgeVbo);
    }
    if (g.edgeKey != key) {
        PROF_SCOPE("edge rebuild+upload");
        if (g.edgeTopology != mesh.topology || mesh.topology == 0) {
            g.edges = uniqueEdges(mesh);
            g.edgeTopology = mesh.topology;
        }
        scratchLines_.clear();
        scratchLines_.reserve(g.edges.size() * 2);
        for (auto [a, b] : g.edges) {
            scratchLines_.push_back({positions[a], theme::white});
            scratchLines_.push_back({positions[b], theme::white});
        }
        gl::BindBuffer(GL_ARRAY_BUFFER, g.edgeVbo);
        gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(scratchLines_.size() * sizeof(LineVertex)), scratchLines_.data(),
                       GL_STATIC_DRAW);
        g.edgeVerts = (int)scratchLines_.size();
        g.edgeKey = key;
    }
    (void)smooth;
    (void)weightSlot;
    if (g.edgeVerts == 0) return;
    gl::UseProgram(lineProg_);
    gl::UniformMatrix4fv(lineU_.model, 1, GL_FALSE, model.m);
    gl::UniformMatrix4fv(lineU_.viewProj, 1, GL_FALSE, f.viewProj.m);
    gl::Uniform4f(lineU_.tint, c.r, c.g, c.b, c.a);
    gl::Uniform1f(lineU_.fadeRadius, 0.0f);
    gl::Uniform1f(lineU_.pointSize, 1.0f);
    gl::Uniform1i(lineU_.roundPoints, 0);
    gl::Enable(GL_DEPTH_TEST);
    gl::DepthMask(GL_FALSE);
    gl::BindVertexArray(g.edgeVao);
    gl::DrawArrays(GL_LINES, 0, g.edgeVerts);
    prof::count("draw calls", 1);
    gl::DepthMask(GL_TRUE);
}

void Renderer::drawBound(unsigned vao, int count, unsigned mode, const FrameParams& f, const Mat4& model,
                         bool depthTest, float fadeRadius, Vec3 fadeCenter, float pointSize) {
    gl::UseProgram(lineProg_);
    gl::UniformMatrix4fv(lineU_.model, 1, GL_FALSE, model.m);
    gl::UniformMatrix4fv(lineU_.viewProj, 1, GL_FALSE, f.viewProj.m);
    gl::Uniform4f(lineU_.tint, 1, 1, 1, 1);
    gl::Uniform3f(lineU_.fadeCenter, fadeCenter.x, fadeCenter.y, fadeCenter.z);
    gl::Uniform1f(lineU_.fadeRadius, fadeRadius);
    gl::Uniform1f(lineU_.pointSize, pointSize);
    gl::Uniform1i(lineU_.roundPoints, mode == GL_POINTS ? 1 : 0);
    if (depthTest) gl::Enable(GL_DEPTH_TEST);
    else gl::Disable(GL_DEPTH_TEST);
    gl::DepthMask(GL_FALSE);
    gl::BindVertexArray(vao);
    gl::DrawArrays(mode, 0, (GLsizei)count);
    prof::count("draw calls", 1);
    gl::DepthMask(GL_TRUE);
    gl::Enable(GL_DEPTH_TEST);
}

void Renderer::drawLineList(const std::vector<LineVertex>& v, unsigned mode, const FrameParams& f, const Mat4& model,
                            bool depthTest, float fadeRadius, Vec3 fadeCenter, float pointSize) {
    if (v.empty()) return;
    gl::BindVertexArray(lineVao_);
    gl::BindBuffer(GL_ARRAY_BUFFER, lineVbo_);
    gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(v.size() * sizeof(LineVertex)), v.data(), GL_STREAM_DRAW);
    drawBound(lineVao_, (int)v.size(), mode, f, model, depthTest, fadeRadius, fadeCenter, pointSize);
}

void Renderer::drawLinesCached(int slot, uint64_t key, const std::vector<LineVertex>& v, const FrameParams& f,
                               const Mat4& model, bool depthTest, bool points, float pointSize) {
    LineBatch& b = lineBatches_[slot];
    if (!b.vao) {
        gl::GenVertexArrays(1, &b.vao);
        gl::GenBuffers(1, &b.vbo);
        setupLineVao(b.vao, b.vbo);
    }
    if (b.key != key) {
        PROF_SCOPE("overlay upload");
        gl::BindBuffer(GL_ARRAY_BUFFER, b.vbo);
        gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(v.size() * sizeof(LineVertex)), v.data(), GL_DYNAMIC_DRAW);
        b.count = (int)v.size();
        b.key = key;
    }
    if (b.count) drawBound(b.vao, b.count, points ? GL_POINTS : GL_LINES, f, model, depthTest, 0.0f, Vec3(), pointSize);
}

void Renderer::drawLines(const std::vector<LineVertex>& v, const FrameParams& f, const Mat4& model, bool depthTest,
                         float fadeRadius, Vec3 fadeCenter) {
    drawLineList(v, GL_LINES, f, model, depthTest, fadeRadius, fadeCenter, 1.0f);
}

void Renderer::drawPoints(const std::vector<LineVertex>& v, const FrameParams& f, const Mat4& model, float size,
                          bool depthTest) {
    drawLineList(v, GL_POINTS, f, model, depthTest, 0.0f, Vec3(), size);
}

void Renderer::drawParticles(const std::vector<ParticleVertex>& v, const FrameParams& f, bool additive) {
    if (v.empty()) return;
    gl::UseProgram(particleProg_);
    gl::UniformMatrix4fv(particleU_.viewProj, 1, GL_FALSE, f.viewProj.m);
    gl::Uniform1f(particleU_.pointScale, f.pointScale);
    gl::Enable(GL_DEPTH_TEST);
    gl::DepthMask(GL_FALSE);
    gl::BlendFunc(GL_SRC_ALPHA, additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
    gl::BindVertexArray(particleVao_);
    gl::BindBuffer(GL_ARRAY_BUFFER, particleVbo_);
    gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(v.size() * sizeof(ParticleVertex)), v.data(), GL_STREAM_DRAW);
    gl::DrawArrays(GL_POINTS, 0, (GLsizei)v.size());
    prof::count("draw calls", 1);
    gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl::DepthMask(GL_TRUE);
}

void Renderer::drawUI(const UI& ui, int w, int h) {
    PROF_SCOPE("ui draw");
    const auto& verts = ui.vertices();
    if (verts.empty()) return;
    gl::Viewport(0, 0, w, h);
    gl::Disable(GL_DEPTH_TEST);
    gl::Enable(GL_BLEND);
    gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl::Enable(GL_SCISSOR_TEST);
    gl::UseProgram(uiProg_);
    gl::Uniform2f(uiU_.screen, (float)w, (float)h);
    gl::Uniform1i(uiU_.tex, 0);
    gl::ActiveTexture(GL_TEXTURE0);
    gl::BindTexture(GL_TEXTURE_2D, fontTex_);
    gl::BindVertexArray(uiVao_);
    gl::BindBuffer(GL_ARRAY_BUFFER, uiVbo_);
    gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts.size() * sizeof(UI::Vertex)), verts.data(), GL_STREAM_DRAW);
    for (const auto& cmd : ui.commands()) {
        if (cmd.count <= 0) continue;
        int x0 = (int)std::floor(cmd.clip.x), y0 = (int)std::floor(cmd.clip.y);
        int x1 = (int)std::ceil(cmd.clip.x + cmd.clip.w), y1 = (int)std::ceil(cmd.clip.y + cmd.clip.h);
        gl::Scissor(x0, h - y1, std::max(0, x1 - x0), std::max(0, y1 - y0));  // GL origin is bottom-left
        gl::DrawArrays(GL_TRIANGLES, cmd.first, cmd.count);
        prof::count("draw calls", 1);
    }
    gl::Disable(GL_SCISSOR_TEST);
    gl::Enable(GL_DEPTH_TEST);
}

bool Renderer::isCached(uint32_t id, uint64_t key) const {
    auto it = cache_.find(id);
    return it != cache_.end() && it->second.key == key;
}

bool Renderer::edgesCached(uint32_t id, uint64_t key) const {
    auto it = cache_.find(id);
    return it != cache_.end() && it->second.edgeKey == key;
}

bool Renderer::beginOutlineMask(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (!maskFbo_ || w != maskW_ || h != maskH_) {
        if (!maskFbo_) {
            gl::GenFramebuffers(1, &maskFbo_);
            gl::GenTextures(1, &maskTex_);
        }
        gl::BindTexture(GL_TEXTURE_2D, maskTex_);
        gl::TexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl::BindFramebuffer(GL_FRAMEBUFFER, maskFbo_);
        gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, maskTex_, 0);
        maskOk_ = gl::CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        maskW_ = w;
        maskH_ = h;
    }
    if (!maskOk_) {
        gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }
    gl::BindFramebuffer(GL_FRAMEBUFFER, maskFbo_);
    gl::Viewport(0, 0, w, h);
    gl::Disable(GL_SCISSOR_TEST);
    gl::ClearColor(0, 0, 0, 0);
    gl::Clear(GL_COLOR_BUFFER_BIT);
    gl::Disable(GL_DEPTH_TEST);
    gl::Disable(GL_BLEND);
    gl::UseProgram(maskProg_);
    return true;
}

void Renderer::drawMeshMask(uint32_t id, uint64_t key, const Mesh& mesh, const std::vector<Vec3>& positions,
                            bool smooth, int weightSlot, const Mat4& model, float value, const FrameParams& f) {
    GpuMesh& g = gpuMesh(id, key, mesh, positions, smooth, weightSlot);
    if (!g.triVerts) return;
    gl::UseProgram(maskProg_);
    gl::UniformMatrix4fv(maskU_.model, 1, GL_FALSE, model.m);
    gl::UniformMatrix4fv(maskU_.viewProj, 1, GL_FALSE, f.viewProj.m);
    gl::Uniform1f(maskU_.value, value);
    gl::BindVertexArray(g.vao);
    gl::DrawElements(GL_TRIANGLES, g.triVerts, GL_UNSIGNED_INT, nullptr);
    prof::count("draw calls", 1);
}

void Renderer::endOutlineMask(int vx, int vy, int vw, int vh, int sx, int sy, int sw, int sh, Color active,
                              Color other, float radiusPx) {
    gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl::Viewport(vx, vy, vw, vh);
    gl::Enable(GL_SCISSOR_TEST);
    gl::Scissor(sx, sy, std::max(0, sw), std::max(0, sh));
    gl::Enable(GL_BLEND);
    gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl::Disable(GL_DEPTH_TEST);
    gl::UseProgram(outlineProg_);
    gl::ActiveTexture(GL_TEXTURE0);
    gl::BindTexture(GL_TEXTURE_2D, maskTex_);
    gl::Uniform1i(outlineU_.mask, 0);
    gl::Uniform2i(outlineU_.offset, vx, vy);
    gl::Uniform2i(outlineU_.size, maskW_, maskH_);
    gl::Uniform1f(outlineU_.radius, radiusPx);
    gl::Uniform4f(outlineU_.active, active.r, active.g, active.b, active.a);
    gl::Uniform4f(outlineU_.other, other.r, other.g, other.b, other.a);
    gl::BindVertexArray(emptyVao_);
    gl::DrawArrays(GL_TRIANGLES, 0, 3);
    prof::count("draw calls", 1);
    gl::Disable(GL_SCISSOR_TEST);
    gl::Enable(GL_DEPTH_TEST);
}

void Renderer::purge(const Scene& scene) {
    std::unordered_set<uint32_t> alive;
    for (const auto& o : scene.objects)
        if (o.isMesh()) alive.insert(o.id);
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (!alive.count(it->first)) {
            GpuMesh& g = it->second;
            gl::DeleteBuffers(1, &g.vbo);
            gl::DeleteBuffers(1, &g.edgeVbo);
            gl::DeleteVertexArrays(1, &g.vao);
            gl::DeleteVertexArrays(1, &g.edgeVao);
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
}

void Renderer::readPixels(int w, int h, std::vector<uint8_t>& rgb) {
    rgb.resize(size_t(w) * h * 3);
    gl::PixelStorei(GL_PACK_ALIGNMENT, 1);
    gl::ReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
}
