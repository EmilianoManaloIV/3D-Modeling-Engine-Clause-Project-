#include "editor_internal.h"
#include "skin.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

using namespace ed;

void Editor::buildGrid() {
    grid_.clear();
    const int n = 50;
    const Color minor{0.55f, 0.57f, 0.62f, 0.20f}, major{0.62f, 0.64f, 0.70f, 0.40f};
    for (int i = -n; i <= n; ++i) {
        float f = (float)i, e = (float)n;
        Color alongZ = i == 0 ? withAlpha(kAxisColor[2], 0.9f) : (i % 5 == 0 ? major : minor);
        Color alongX = i == 0 ? withAlpha(kAxisColor[0], 0.9f) : (i % 5 == 0 ? major : minor);
        grid_.push_back({{f, 0, -e}, alongZ});
        grid_.push_back({{f, 0, e}, alongZ});
        grid_.push_back({{-e, 0, f}, alongX});
        grid_.push_back({{e, 0, f}, alongX});
    }
}

uint64_t Editor::meshKey(int i, bool restPose, int weightSlot) const {
    const Object& o = scene_.objects[i];
    uint64_t h = o.mesh.version * 0x9E3779B97F4A7C15ull;
    h ^= (uint64_t)(o.smooth ? 1 : 2) * 0xC2B2AE3D27D4EB4Full;
    h ^= (uint64_t)(weightSlot + 2) * 0x165667B19E3779F9ull;
    if (!restPose) h ^= poseHash(scene_, i);
    else h ^= 0x27D4EB2F165667C5ull;
    return h;
}

void Editor::collectLights(FrameParams& f) {
    f.lights.clear();
    for (int i = 0; i < (int)scene_.objects.size() && (int)f.lights.size() < kMaxLights; ++i) {
        const Object& o = scene_.objects[i];
        if (o.kind != ObjectKind::Light) continue;
        Mat4 w = scene_.world(i);
        GpuLight L;
        L.type = (int)o.light.type;
        L.pos = transformPoint(w, Vec3());
        L.dir = normalize(transformDir(w, Vec3(0, -1, 0)));
        L.color = o.light.color * o.light.intensity;
        L.range = o.light.range;
        float half = toRadians(clampf(o.light.spotAngle, 1.0f, 179.0f) * 0.5f);
        L.cosOuter = std::cos(half);
        L.cosInner = std::cos(half * (1.0f - clampf(o.light.spotBlend, 0.0f, 1.0f)));
        if (L.cosInner <= L.cosOuter) L.cosInner = L.cosOuter + 1e-4f;
        f.lights.push_back(L);
    }
}

void Editor::updateParticles(float dt) {
    std::unordered_set<uint32_t> emitters;
    for (int i = 0; i < (int)scene_.objects.size(); ++i) {
        const Object& o = scene_.objects[i];
        if (o.kind != ObjectKind::Emitter) continue;
        emitters.insert(o.id);
        if (playing_) stepParticles(particles_[o.id], o.particles, scene_.world(i), dt);
    }
    for (auto it = particles_.begin(); it != particles_.end();)
        it = emitters.count(it->first) ? std::next(it) : particles_.erase(it);
}

void Editor::appendGizmos(std::vector<LineVertex>& lines, std::vector<ParticleVertex>& glows) {
    const Vec3 right = camRight(), up = camUp();
    auto seg = [&](Vec3 a, Vec3 b, Color c) {
        lines.push_back({a, c});
        lines.push_back({b, c});
    };
    auto circle = [&](Vec3 center, Vec3 u, Vec3 v, float r, Color c, int n = 16) {
        for (int k = 0; k < n; ++k) {
            float a0 = 2 * kPi * k / n, a1 = 2 * kPi * (k + 1) / n;
            seg(center + (u * std::cos(a0) + v * std::sin(a0)) * r, center + (u * std::cos(a1) + v * std::sin(a1)) * r,
                c);
        }
    };
    for (int i = 0; i < (int)scene_.objects.size(); ++i) {
        const Object& o = scene_.objects[i];
        const Mat4 W = scene_.world(i);
        const Vec3 p = transformPoint(W, Vec3());
        const bool isActive = i == scene_.active, sel = o.selected;

        // Relationship line to the parent (like Blender's dashed lines).
        int pi = scene_.parentIndex(i);
        if (pi >= 0) seg(p, transformPoint(scene_.world(pi), Vec3()), {0.7f, 0.72f, 0.78f, 0.35f});

        if (o.isMesh()) continue;
        Color base = o.kind == ObjectKind::Light ? kLightGizmo
                     : o.kind == ObjectKind::Bone  ? kBoneColor
                     : o.kind == ObjectKind::Emitter ? kEmitterGizmo
                                                    : Color{0.9f, 0.9f, 0.9f, 1};
        Color c = isActive ? Color{1.0f, 0.75f, 0.35f, 1} : sel ? withAlpha(theme::selection, 0.95f) : withAlpha(base, 0.85f);
        const float k = worldPerPixel(p) * 14.0f * dpi_;  // ~constant screen size

        switch (o.kind) {
            case ObjectKind::Light: {
                Vec3 dir = normalize(transformDir(W, Vec3(0, -1, 0)));
                Vec3 lc = o.light.color;
                glows.push_back({p, lc.x, lc.y, lc.z, 0.9f, k * 1.6f});
                circle(p, right, up, k * 0.6f, c);
                if (o.light.type == LightType::Point) {
                    for (int a = 0; a < 4; ++a) {
                        float ang = kPi * 0.25f + a * kPi * 0.5f;
                        Vec3 d = right * std::cos(ang) + up * std::sin(ang);
                        seg(p + d * (k * 0.8f), p + d * (k * 1.4f), c);
                    }
                } else if (o.light.type == LightType::Sun) {
                    seg(p, p + dir * (k * 5.0f), c);
                    for (int a = 0; a < 8; ++a) {
                        float ang = a * kPi * 0.25f;
                        Vec3 d = right * std::cos(ang) + up * std::sin(ang);
                        seg(p + d * (k * 0.8f), p + d * (k * 1.2f), c);
                    }
                } else {
                    float len = k * 5.0f;
                    float rad = std::tan(toRadians(clampf(o.light.spotAngle, 1, 179) * 0.5f)) * len;
                    Vec3 u = normalize(cross(dir, std::fabs(dir.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0)));
                    Vec3 v = cross(dir, u);
                    Vec3 end = p + dir * len;
                    circle(end, u, v, rad, c, 20);
                    for (int a = 0; a < 4; ++a) {
                        float ang = a * kPi * 0.5f;
                        seg(p, end + (u * std::cos(ang) + v * std::sin(ang)) * rad, c);
                    }
                }
                break;
            }
            case ObjectKind::Bone: {
                // Octahedral bone: head -> ring at 15% -> tail.
                const float L = o.boneLength, r = 0.1f * L, y = 0.15f * L;
                Vec3 head = p, tail = transformPoint(W, Vec3(0, L, 0));
                Vec3 ring[4] = {transformPoint(W, {r, y, 0}), transformPoint(W, {0, y, r}),
                                transformPoint(W, {-r, y, 0}), transformPoint(W, {0, y, -r})};
                for (int a = 0; a < 4; ++a) {
                    seg(head, ring[a], c);
                    seg(ring[a], tail, c);
                    seg(ring[a], ring[(a + 1) % 4], c);
                }
                break;
            }
            case ObjectKind::Empty: {
                const float s = o.boneLength * 0.5f;
                for (int a = 0; a < 3; ++a) {
                    Vec3 d = transformDir(W, axisVector(a)) * s;
                    seg(p - d, p + d, withAlpha(isActive || sel ? c : kAxisColor[a], 0.9f));
                }
                break;
            }
            case ObjectKind::Emitter: {
                Vec3 dir = normalize(transformDir(W, Vec3(0, 1, 0)));
                float rad = std::max(o.particles.radius * length(transformDir(W, Vec3(1, 0, 0))), k * 0.6f);
                circle(p, right, up, rad, c);
                Vec3 tip = p + dir * (k * 4.0f);
                seg(p, tip, c);
                Vec3 side = normalize(cross(dir, std::fabs(dir.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0)));
                seg(tip, tip - dir * k + side * (k * 0.6f), c);
                seg(tip, tip - dir * k - side * (k * 0.6f), c);
                break;
            }
            default:
                break;
        }
    }
}

void Editor::renderViewport() {
    const int vx = (int)viewport_.x, vy = (int)(screenH_ - (viewport_.y + viewport_.h));
    renderer_.beginViewport(vx, vy, (int)viewport_.w, (int)viewport_.h, kViewportBg);

    FrameParams f;
    f.viewProj = viewProj_;
    f.cameraPos = cam_.eye();
    const Vec3 r = camRight(), u = camUp(), back = -camForward();
    f.keyLightDir = normalize(r * -0.45f + u * 0.75f + back * 0.6f);  // over the left shoulder
    f.fillLightDir = normalize(r * 0.7f - u * 0.15f + back * 0.4f);
    f.shading = shading_;
    f.ambient = scene_.ambient;
    f.pointScale = viewport_.h * proj_(1, 1) * 0.5f;
    collectLights(f);

    const int count = (int)scene_.objects.size();
    for (int i = 0; i < count; ++i) {
        const Object& o = scene_.objects[i];
        if (!o.isMesh()) continue;
        const bool rest = mode_ == Mode::Edit && i == scene_.active;
        const int slot = shading_ == SHADE_WEIGHTS ? displayWeightSlot(o) : -1;
        Mat4 model;
        const std::vector<Vec3>& pos = evaluateMesh(scene_, i, rest, scratch_, model);
        MaterialParams mat;
        mat.color = o.color;
        Vec3 e = o.emission;
        mat.emission = Vec3(std::pow(e.x, 2.2f), std::pow(e.y, 2.2f), std::pow(e.z, 2.2f)) * o.emissionStrength;
        mat.gloss = o.gloss;
        mat.highlight = (mode_ == Mode::Object && o.selected) ? (i == scene_.active ? 1.0f : 0.6f) : 0.0f;
        renderer_.drawMesh(o.id, meshKey(i, rest, slot), o.mesh, pos, o.smooth, slot, model, mat, f);
    }

    if (showGrid_) renderer_.drawLines(grid_, f, Mat4(), true, std::max(20.0f, cam_.distance * 3.0f), cam_.target);

    for (int i = 0; i < count; ++i) {
        const Object& o = scene_.objects[i];
        if (!o.isMesh() || (mode_ == Mode::Edit && i == scene_.active)) continue;
        bool selectedOutline = mode_ == Mode::Object && o.selected;
        if (!selectedOutline && !wireframe_) continue;
        const int slot = shading_ == SHADE_WEIGHTS ? displayWeightSlot(o) : -1;
        Mat4 model;
        const std::vector<Vec3>& pos = evaluateMesh(scene_, i, false, scratch_, model);
        Color c = selectedOutline ? withAlpha(theme::selection, i == scene_.active ? 0.9f : 0.55f)
                                  : Color{0.02f, 0.02f, 0.03f, 0.5f};
        renderer_.drawMeshEdges(o.id, meshKey(i, false, slot), o.mesh, pos, o.smooth, slot, model, c, f);
    }

    if (mode_ == Mode::Edit && activeMesh()) {
        const Object& o = *activeMesh();
        const auto& sel = vertSel();
        if (editEdgesVersion_ != o.mesh.version) {
            editEdges_ = uniqueEdges(o.mesh);
            editEdgesVersion_ = o.mesh.version;
        }
        std::vector<LineVertex> lines;
        lines.reserve(editEdges_.size() * 2);
        for (auto [a, b] : editEdges_) {
            Color c = (sel[a] && sel[b]) ? theme::selection : kEdgeDark;
            lines.push_back({o.mesh.verts[a], c});
            lines.push_back({o.mesh.verts[b], c});
        }
        Mat4 m = scene_.world(scene_.active);
        renderer_.drawLines(lines, f, m, !wireframe_);
        std::vector<LineVertex> points;
        points.reserve(o.mesh.verts.size());
        for (size_t v = 0; v < o.mesh.verts.size(); ++v)
            points.push_back({o.mesh.verts[v], sel[v] ? theme::selection : Color{0.05f, 0.05f, 0.07f, 1}});
        renderer_.drawPoints(points, f, m, 3.5f * fontScale_, !wireframe_);
    }

    // Gizmos for lights, bones, empties and emitters (drawn on top).
    std::vector<LineVertex> gizmo;
    std::vector<ParticleVertex> glows;
    appendGizmos(gizmo, glows);
    renderer_.drawLines(gizmo, f, Mat4(), false);

    // Particles: smoke (alpha blended) first, then glowing (additive) ones.
    std::vector<ParticleVertex> smoke, glow;
    for (int i = 0; i < count; ++i) {
        const Object& o = scene_.objects[i];
        if (o.kind != ObjectKind::Emitter) continue;
        auto it = particles_.find(o.id);
        if (it != particles_.end()) appendParticleVertices(it->second, o.particles, o.particles.additive ? glow : smoke);
    }
    renderer_.drawParticles(smoke, f, false);
    if (shading_ == SHADE_LIT) glow.insert(glow.end(), glows.begin(), glows.end());
    renderer_.drawParticles(glow, f, true);

    if (xf_ != Xform::None && xfAxis_ >= 0) {
        Vec3 a = xfAxis_ < 3 ? axisVector(xfAxis_) : xfCustomAxis_;
        Color c = xfAxis_ < 3 ? kAxisColor[xfAxis_] : kCustomAxisColor;
        std::vector<LineVertex> guide = {{xfPivot_ - a * 1000.0f, c}, {xfPivot_ + a * 1000.0f, c}};
        renderer_.drawLines(guide, f, Mat4(), false);
    }
}
