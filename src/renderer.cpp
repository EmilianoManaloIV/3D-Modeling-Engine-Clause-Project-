#include "renderer.h"

#include "font.h"
#include "gl.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace {

// --- Shaders -------------------------------------------------------------------
// Mesh: Blinn-Phong with a key light, a fill light and hemispherical ambient
// (FoCG 5e sec. 5.2-5.3), lit in linear space and gamma-encoded at the end.
const char* kMeshVS = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
uniform mat4 uModel;
uniform mat4 uViewProj;
uniform mat4 uNormalMatrix;
out vec3 vWorld;
out vec3 vNormal;
void main() {
    vec4 world = uModel * vec4(aPos, 1.0);
    vWorld = world.xyz;
    vNormal = mat3(uNormalMatrix) * aNormal;
    gl_Position = uViewProj * world;
}
)";

const char* kMeshFS = R"(#version 330 core
in vec3 vWorld;
in vec3 vNormal;
uniform vec3 uColor;
uniform vec3 uCamPos;
uniform vec3 uKeyDir;
uniform vec3 uFillDir;
uniform float uHighlight;
out vec4 fragColor;
void main() {
    vec3 n = normalize(vNormal);
    if (!gl_FrontFacing) n = -n;               // two-sided lighting
    vec3 v = normalize(uCamPos - vWorld);
    vec3 base = pow(uColor, vec3(2.2));        // sRGB -> linear
    float key = max(dot(n, uKeyDir), 0.0);
    float fill = max(dot(n, uFillDir), 0.0);
    vec3 ambient = mix(vec3(0.07, 0.065, 0.06), vec3(0.17, 0.19, 0.23), n.y * 0.5 + 0.5);
    vec3 h = normalize(uKeyDir + v);
    float spec = key > 0.0 ? pow(max(dot(n, h), 0.0), 48.0) * 0.30 : 0.0;
    vec3 c = base * (ambient + key * vec3(1.0, 0.97, 0.92) * 0.95 + fill * vec3(0.55, 0.62, 0.75) * 0.30) + vec3(spec);
    float rim = pow(1.0 - max(dot(n, v), 0.0), 3.0);
    c += vec3(1.0, 0.45, 0.08) * rim * uHighlight * 0.30;   // selection glow
    if (!gl_FrontFacing) c *= 0.6;                          // reveal inside / flipped faces
    fragColor = vec4(pow(c, vec3(1.0 / 2.2)), 1.0);
}
)";

// Lines and points: per-vertex color * tint, optional distance fade (grid).
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

}  // namespace

bool Renderer::init(std::string& error) {
    meshProg_ = linkProgram(kMeshVS, kMeshFS, error);
    lineProg_ = meshProg_ ? linkProgram(kLineVS, kLineFS, error) : 0;
    uiProg_ = lineProg_ ? linkProgram(kUiVS, kUiFS, error) : 0;
    if (!meshProg_ || !lineProg_ || !uiProg_) return false;

    meshU_.model = gl::GetUniformLocation(meshProg_, "uModel");
    meshU_.viewProj = gl::GetUniformLocation(meshProg_, "uViewProj");
    meshU_.normalMatrix = gl::GetUniformLocation(meshProg_, "uNormalMatrix");
    meshU_.color = gl::GetUniformLocation(meshProg_, "uColor");
    meshU_.camPos = gl::GetUniformLocation(meshProg_, "uCamPos");
    meshU_.keyDir = gl::GetUniformLocation(meshProg_, "uKeyDir");
    meshU_.fillDir = gl::GetUniformLocation(meshProg_, "uFillDir");
    meshU_.highlight = gl::GetUniformLocation(meshProg_, "uHighlight");

    lineU_.model = gl::GetUniformLocation(lineProg_, "uModel");
    lineU_.viewProj = gl::GetUniformLocation(lineProg_, "uViewProj");
    lineU_.tint = gl::GetUniformLocation(lineProg_, "uTint");
    lineU_.fadeCenter = gl::GetUniformLocation(lineProg_, "uFadeCenter");
    lineU_.fadeRadius = gl::GetUniformLocation(lineProg_, "uFadeRadius");
    lineU_.pointSize = gl::GetUniformLocation(lineProg_, "uPointSize");
    lineU_.roundPoints = gl::GetUniformLocation(lineProg_, "uRoundPoints");

    uiU_.screen = gl::GetUniformLocation(uiProg_, "uScreen");
    uiU_.tex = gl::GetUniformLocation(uiProg_, "uTex");

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
    return true;
}

void Renderer::shutdown() {
    for (auto& kv : cache_) {
        GpuMesh& g = kv.second;
        gl::DeleteBuffers(1, &g.vbo);
        gl::DeleteBuffers(1, &g.edgeVbo);
        gl::DeleteVertexArrays(1, &g.vao);
        gl::DeleteVertexArrays(1, &g.edgeVao);
    }
    cache_.clear();
    gl::DeleteBuffers(1, &lineVbo_);
    gl::DeleteBuffers(1, &uiVbo_);
    gl::DeleteVertexArrays(1, &lineVao_);
    gl::DeleteVertexArrays(1, &uiVao_);
    gl::DeleteTextures(1, &fontTex_);
    gl::DeleteProgram(meshProg_);
    gl::DeleteProgram(lineProg_);
    gl::DeleteProgram(uiProg_);
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

Renderer::GpuMesh& Renderer::gpuMesh(const Object& o) {
    GpuMesh& g = cache_[o.id];
    if (!g.vao) {
        gl::GenVertexArrays(1, &g.vao);
        gl::GenBuffers(1, &g.vbo);
        gl::BindVertexArray(g.vao);
        gl::BindBuffer(GL_ARRAY_BUFFER, g.vbo);
        gl::EnableVertexAttribArray(0);
        gl::VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(RenderVertex), offsetPtr(0));
        gl::EnableVertexAttribArray(1);
        gl::VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(RenderVertex), offsetPtr(sizeof(Vec3)));
        gl::GenVertexArrays(1, &g.edgeVao);
        gl::GenBuffers(1, &g.edgeVbo);
        setupLineVao(g.edgeVao, g.edgeVbo);
    }
    if (g.version != o.mesh.version || g.smooth != o.smooth) {
        buildRenderData(o.mesh, o.smooth, 40.0f, scratch_);
        gl::BindBuffer(GL_ARRAY_BUFFER, g.vbo);
        gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(scratch_.size() * sizeof(RenderVertex)), scratch_.data(),
                       GL_STATIC_DRAW);
        g.triVerts = (int)scratch_.size();

        scratchLines_.clear();
        for (auto [a, b] : uniqueEdges(o.mesh)) {
            scratchLines_.push_back({o.mesh.verts[a], theme::white});
            scratchLines_.push_back({o.mesh.verts[b], theme::white});
        }
        gl::BindBuffer(GL_ARRAY_BUFFER, g.edgeVbo);
        gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(scratchLines_.size() * sizeof(LineVertex)), scratchLines_.data(),
                       GL_STATIC_DRAW);
        g.edgeVerts = (int)scratchLines_.size();
        g.version = o.mesh.version;
        g.smooth = o.smooth;
    }
    return g;
}

void Renderer::drawObject(const Object& o, const FrameParams& f, float highlight) {
    GpuMesh& g = gpuMesh(o);
    if (g.triVerts == 0) return;
    Mat4 model = o.matrix();
    Mat4 normalMatrix = transpose(inverse(model));  // normals transform by (M^-1)^T, FoCG 5e ch. 7
    gl::UseProgram(meshProg_);
    gl::UniformMatrix4fv(meshU_.model, 1, GL_FALSE, model.m);
    gl::UniformMatrix4fv(meshU_.viewProj, 1, GL_FALSE, f.viewProj.m);
    gl::UniformMatrix4fv(meshU_.normalMatrix, 1, GL_FALSE, normalMatrix.m);
    gl::Uniform3f(meshU_.color, o.color.x, o.color.y, o.color.z);
    gl::Uniform3f(meshU_.camPos, f.cameraPos.x, f.cameraPos.y, f.cameraPos.z);
    gl::Uniform3f(meshU_.keyDir, f.keyLightDir.x, f.keyLightDir.y, f.keyLightDir.z);
    gl::Uniform3f(meshU_.fillDir, f.fillLightDir.x, f.fillLightDir.y, f.fillLightDir.z);
    gl::Uniform1f(meshU_.highlight, highlight);
    // Push filled polygons slightly back so wireframe lines drawn on top of
    // them win the depth test.
    gl::Enable(GL_POLYGON_OFFSET_FILL);
    gl::PolygonOffset(1.0f, 1.0f);
    gl::BindVertexArray(g.vao);
    gl::DrawArrays(GL_TRIANGLES, 0, g.triVerts);
    gl::Disable(GL_POLYGON_OFFSET_FILL);
}

void Renderer::drawObjectEdges(const Object& o, const FrameParams& f, Color c) {
    GpuMesh& g = gpuMesh(o);
    if (g.edgeVerts == 0) return;
    Mat4 model = o.matrix();
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
    gl::DepthMask(GL_TRUE);
}

void Renderer::drawLineList(const std::vector<LineVertex>& v, unsigned mode, const FrameParams& f, const Mat4& model,
                            bool depthTest, float fadeRadius, Vec3 fadeCenter, float pointSize) {
    if (v.empty()) return;
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
    gl::BindVertexArray(lineVao_);
    gl::BindBuffer(GL_ARRAY_BUFFER, lineVbo_);
    gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(v.size() * sizeof(LineVertex)), v.data(), GL_STREAM_DRAW);
    gl::DrawArrays(mode, 0, (GLsizei)v.size());
    gl::DepthMask(GL_TRUE);
    gl::Enable(GL_DEPTH_TEST);
}

void Renderer::drawLines(const std::vector<LineVertex>& v, const FrameParams& f, const Mat4& model, bool depthTest,
                         float fadeRadius, Vec3 fadeCenter) {
    drawLineList(v, GL_LINES, f, model, depthTest, fadeRadius, fadeCenter, 1.0f);
}

void Renderer::drawPoints(const std::vector<LineVertex>& v, const FrameParams& f, const Mat4& model, float size,
                          bool depthTest) {
    drawLineList(v, GL_POINTS, f, model, depthTest, 0.0f, Vec3(), size);
}

void Renderer::drawUI(const UI& ui, int w, int h) {
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
    }
    gl::Disable(GL_SCISSOR_TEST);
    gl::Enable(GL_DEPTH_TEST);
}

void Renderer::purge(const Scene& scene) {
    if (cache_.size() <= scene.objects.size()) {
        bool allPresent = true;
        for (const auto& kv : cache_) {
            bool found = false;
            for (const auto& o : scene.objects)
                if (o.id == kv.first) { found = true; break; }
            if (!found) { allPresent = false; break; }
        }
        if (allPresent) return;
    }
    std::unordered_set<uint32_t> alive;
    for (const auto& o : scene.objects) alive.insert(o.id);
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
