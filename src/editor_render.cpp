#include "editor_internal.h"
#include "skin.h"
#include "profiler.h"

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
        L.color = o.light.finalColor() * o.light.intensity;
        L.axisU = transformDir(w, Vec3(o.light.width * 0.5f, 0, 0));
        L.axisV = transformDir(w, Vec3(0, 0, o.light.height * 0.5f));
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
                     : o.kind == ObjectKind::Camera ? Color{0.55f, 0.85f, 1.0f, 1}
                                                    : Color{0.9f, 0.9f, 0.9f, 1};
        Color c = isActive ? Color{1.0f, 0.75f, 0.35f, 1} : sel ? withAlpha(theme::selection, 0.95f) : withAlpha(base, 0.85f);
        const float k = worldPerPixel(p) * 14.0f * dpi_;  // ~constant screen size

        switch (o.kind) {
            case ObjectKind::Light: {
                Vec3 dir = normalize(transformDir(W, Vec3(0, -1, 0)));
                Vec3 lc = o.light.finalColor();
                glows.push_back({p, lc.x, lc.y, lc.z, 0.9f, k * 1.6f});
                if (o.light.type != LightType::Area) circle(p, right, up, k * 0.6f, c);
                if (o.light.type == LightType::Area) {
                    // The emitting rectangle and its facing direction.
                    Vec3 hu = transformDir(W, Vec3(o.light.width * 0.5f, 0, 0));
                    Vec3 hv = transformDir(W, Vec3(0, 0, o.light.height * 0.5f));
                    Vec3 q[4] = {p - hu - hv, p + hu - hv, p + hu + hv, p - hu + hv};
                    for (int a = 0; a < 4; ++a) seg(q[a], q[(a + 1) % 4], c);
                    seg(q[0], q[2], withAlpha(c, 0.4f));
                    seg(q[1], q[3], withAlpha(c, 0.4f));
                    seg(p, p + dir * (k * 3.0f), c);
                } else if (o.light.type == LightType::Point) {
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
            case ObjectKind::Camera: {
                // Frustum of the physical camera (to its focus distance, capped
                // to a readable size) plus an "up" triangle, like Blender's icon.
                const float aspect = viewport_.w / std::max(1.0f, viewport_.h);
                const float fov = toRadians(o.camera.verticalFovDeg(aspect));
                const float depth = clampf(o.camera.focusDistance, k * 3.0f, k * 12.0f);
                const float hh = std::tan(fov * 0.5f) * depth, hw = hh * aspect;
                Vec3 fwd = normalize(transformDir(W, Vec3(0, 0, -1)));
                Vec3 cu = normalize(transformDir(W, Vec3(0, 1, 0))), cr = normalize(cross(fwd, cu));
                cu = cross(cr, fwd);
                Vec3 centre = p + fwd * depth;
                Vec3 q[4] = {centre - cr * hw - cu * hh, centre + cr * hw - cu * hh, centre + cr * hw + cu * hh,
                             centre - cr * hw + cu * hh};
                for (int a = 0; a < 4; ++a) {
                    seg(p, q[a], c);
                    seg(q[a], q[(a + 1) % 4], c);
                }
                Vec3 t0 = centre + cu * (hh * 1.1f) - cr * (hw * 0.3f), t1 = centre + cu * (hh * 1.1f) + cr * (hw * 0.3f);
                Vec3 t2 = centre + cu * (hh * 1.45f);
                seg(t0, t1, c);
                seg(t1, t2, c);
                seg(t2, t0, c);
                if (scene_.renderCameraIndex() == i) circle(p, right, up, k * 0.5f, c);  // the render camera
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
    if (renderView_) {  // path-traced image instead of the raster preview
        presentRender(vx, vy, (int)viewport_.w, (int)viewport_.h);
        return;
    }

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
    // Positions are only needed when the GPU copy is out of date; otherwise
    // skip evaluation (and with it CPU skinning) entirely.
    static const std::vector<Vec3> kNoPositions;
    auto meshInputs = [&](int i, bool rest, uint64_t key, Mat4& model) -> const std::vector<Vec3>& {
        const Object& o = scene_.objects[i];
        if (renderer_.isCached(o.id, key)) {
            model = (!rest && isSkinned(o)) ? Mat4() : scene_.world(i);
            return kNoPositions;
        }
        return evaluateMesh(scene_, i, rest, scratch_, model);
    };
    // Opaque meshes first, then transparent ones back to front (blended
    // without depth writes), the usual order for alpha blending.
    std::vector<std::pair<float, int>> transparentOrder;
    const Vec3 eye = cam_.eye();
    auto drawOne = [&](int i) {
        const Object& o = scene_.objects[i];
        const bool rest = mode_ == Mode::Edit && i == scene_.active;
        const int slot = shading_ == SHADE_WEIGHTS ? displayWeightSlot(o) : -1;
        const uint64_t key = meshKey(i, rest, slot);
        Mat4 model;
        const std::vector<Vec3>& pos = meshInputs(i, rest, key, model);
        MaterialParams mat;
        mat.color = o.color;
        Vec3 e = o.emission;
        mat.emission = Vec3(std::pow(e.x, 2.2f), std::pow(e.y, 2.2f), std::pow(e.z, 2.2f)) * o.emissionStrength;
        mat.roughness = o.roughness;
        mat.metallic = o.metallic;
        mat.opacity = o.opacity;
        mat.transmission = o.transmission;
        mat.ior = o.ior;
        mat.normalStrength = o.normalStrength;
        mat.uvScale = o.uvScale;
        if (shading_ <= SHADE_LIT)
            for (int t = 0; t < TEX_COUNT; ++t) mat.textures[t] = renderer_.texture(o.textures[t]);
        mat.highlight = (mode_ == Mode::Object && o.selected) ? (i == scene_.active ? 1.0f : 0.6f) : 0.0f;
        renderer_.drawMesh(o.id, key, o.mesh, pos, o.smooth, slot, model, mat, f);
    };
    for (int i = 0; i < count; ++i) {
        const Object& o = scene_.objects[i];
        if (!o.isMesh()) continue;
        if (o.transparent() && shading_ <= SHADE_LIT) {
            Vec3 lo, hi;
            Vec3 c = o.mesh.bounds(lo, hi) ? (lo + hi) * 0.5f : Vec3();
            transparentOrder.push_back({-length(transformPoint(scene_.world(i), c) - eye), i});
            continue;
        }
        drawOne(i);
    }
    std::sort(transparentOrder.begin(), transparentOrder.end());
    for (const auto& t : transparentOrder) drawOne(t.second);

    prof::count("objects", count);
    if (showGrid_) renderer_.drawLines(grid_, f, Mat4(), true, std::max(20.0f, cam_.distance * 3.0f), cam_.target);

    // Wireframe overlay (only when asked for; edge buffers are built lazily).
    if (wireframe_) {
        PROF_SCOPE("wireframe");
        for (int i = 0; i < count; ++i) {
            const Object& o = scene_.objects[i];
            if (!o.isMesh() || (mode_ == Mode::Edit && i == scene_.active)) continue;
            const int slot = shading_ == SHADE_WEIGHTS ? displayWeightSlot(o) : -1;
            const uint64_t key = meshKey(i, false, slot);
            Mat4 model;
            const bool cached = renderer_.edgesCached(o.id, key);
            const std::vector<Vec3>& pos = cached ? kNoPositions : evaluateMesh(scene_, i, false, scratch_, model);
            if (cached) model = isSkinned(o) ? Mat4() : scene_.world(i);
            Color c = o.selected && mode_ == Mode::Object ? withAlpha(theme::selection, 0.8f) : Color{0.02f, 0.02f, 0.03f, 0.5f};
            renderer_.drawMeshEdges(o.id, key, o.mesh, pos, o.smooth, slot, model, c, f);
        }
    }

    // Selection outline (Unity style): mask of the selected meshes, then an
    // outline around it. Constant cost whatever the mesh density.
    if (mode_ == Mode::Object && scene_.selectedCount() > 0) {
        PROF_SCOPE("selection outline");
        const int vw = (int)viewport_.w, vh = (int)viewport_.h;
        // Screen rectangle of the selection: the outline pass only runs there.
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        bool offscreen = false;
        for (int i = 0; i < count && !offscreen; ++i) {
            const Object& o = scene_.objects[i];
            Vec3 lo, hi;
            if (!o.isMesh() || !o.selected || !o.mesh.bounds(lo, hi)) continue;
            if (isSkinned(o)) {
                offscreen = true;  // posed skinned mesh can leave its rest bounds
                break;
            }
            Mat4 w = scene_.world(i);
            for (int c = 0; c < 8; ++c) {
                Vec2 sp;
                if (!worldToScreen(transformPoint(w, Vec3((c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y,
                                                          (c & 4) ? hi.z : lo.z)),
                                   sp)) {
                    offscreen = true;
                    break;
                }
                x0 = std::min(x0, sp.x), y0 = std::min(y0, sp.y), x1 = std::max(x1, sp.x), y1 = std::max(y1, sp.y);
            }
        }
        const float pad = 4.0f * dpi_ + 4.0f;
        if (offscreen) x0 = y0 = -pad, x1 = (float)vw + pad, y1 = (float)vh + pad;
        x0 = std::max(0.0f, x0 - pad), y0 = std::max(0.0f, y0 - pad);
        x1 = std::min((float)vw, x1 + pad), y1 = std::min((float)vh, y1 + pad);
        if (x1 > x0 && y1 > y0 && renderer_.beginOutlineMask(vw, vh)) {
            for (int i = 0; i < count; ++i) {
                const Object& o = scene_.objects[i];
                if (!o.isMesh() || !o.selected) continue;
                const int slot = shading_ == SHADE_WEIGHTS ? displayWeightSlot(o) : -1;
                const uint64_t key = meshKey(i, false, slot);
                Mat4 model;
                const std::vector<Vec3>& pos = meshInputs(i, false, key, model);
                renderer_.drawMeshMask(o.id, key, o.mesh, pos, o.smooth, slot, model, i == scene_.active ? 1.0f : 0.5f, f);
            }
            renderer_.endOutlineMask(vx, vy, vw, vh, vx + (int)x0, vy + (int)(vh - y1), (int)(x1 - x0) + 1,
                                     (int)(y1 - y0) + 1, {1.0f, 0.62f, 0.12f, 1.0f}, {0.95f, 0.45f, 0.08f, 0.85f},
                                     std::max(2.0f, 2.0f * dpi_));
        }
    }

    if (mode_ == Mode::Edit && activeMesh()) {
        PROF_SCOPE("edit overlay");
        const Object& o = *activeMesh();
        const auto& sel = vertSel();
        // Edges depend on topology only; the line/point buffers on positions and
        // selection. Rebuild each only when its inputs change.
        if (editEdgesVersion_ != o.mesh.topology || o.mesh.topology == 0) {
            editEdges_ = uniqueEdges(o.mesh);
            editEdgesVersion_ = o.mesh.topology;
        }
        syncEditSelection();
        uint64_t selHash = 1469598103934665603ull;
        for (char c : sel) selHash = (selHash ^ (uint8_t)c) * 1099511628211ull;
        for (char c : fsel_) selHash = (selHash ^ (uint8_t)c) * 1099511628211ull;
        for (const auto& e : esel_) selHash = (selHash ^ (uint64_t)(e.first * 31 + e.second)) * 1099511628211ull;
        selHash ^= (uint64_t)selMode_ * 0x51ED27F1ull;
        const uint64_t key = (o.mesh.version * 0x9E3779B97F4A7C15ull) ^ selHash ^ ((uint64_t)o.id << 40);
        if (key != editOverlayKey_) {
            const Color dark{0.05f, 0.05f, 0.07f, 1};
            // Which edges are drawn as selected depends on the mode.
            std::unordered_set<uint64_t> selEdges;
            if (selMode_ == SelMode::Edge) {
                for (const auto& e : esel_) selEdges.insert((uint64_t(uint32_t(e.first)) << 32) | uint32_t(e.second));
            } else if (selMode_ == SelMode::Face) {
                for (size_t f = 0; f < fsel_.size(); ++f) {
                    if (!fsel_[f]) continue;
                    const auto& face = o.mesh.faces[f];
                    for (size_t i = 0; i < face.size(); ++i) {
                        auto e = meshedit::makeEdge(face[i], face[(i + 1) % face.size()]);
                        selEdges.insert((uint64_t(uint32_t(e.first)) << 32) | uint32_t(e.second));
                    }
                }
            }
            editLines_.clear();
            editLines_.reserve(editEdges_.size() * 2);
            for (auto [a, b] : editEdges_) {
                bool on = selMode_ == SelMode::Vertex ? (sel[a] && sel[b])
                                                      : selEdges.count((uint64_t(uint32_t(a)) << 32) | uint32_t(b)) > 0;
                Color c = on ? theme::selection : kEdgeDark;
                editLines_.push_back({o.mesh.verts[a], c});
                editLines_.push_back({o.mesh.verts[b], c});
            }
            editPoints_.clear();
            editFill_.clear();
            if (selMode_ == SelMode::Vertex) {
                editPoints_.reserve(o.mesh.verts.size());
                for (size_t v = 0; v < o.mesh.verts.size(); ++v)
                    editPoints_.push_back({o.mesh.verts[v], sel[v] ? theme::selection : dark});
            } else if (selMode_ == SelMode::Face) {
                const Color fill = withAlpha(theme::selection, 0.28f);
                for (size_t f = 0; f < o.mesh.faces.size(); ++f) {
                    const auto& face = o.mesh.faces[f];
                    const bool on = f < fsel_.size() && fsel_[f];
                    editPoints_.push_back({faceCenter(o.mesh, face), on ? theme::selection : dark});
                    if (!on) continue;
                    for (size_t i = 1; i + 1 < face.size(); ++i) {
                        editFill_.push_back({o.mesh.verts[face[0]], fill});
                        editFill_.push_back({o.mesh.verts[face[i]], fill});
                        editFill_.push_back({o.mesh.verts[face[i + 1]], fill});
                    }
                }
            }
            editOverlayKey_ = key;
        }
        Mat4 m = scene_.world(scene_.active);
        renderer_.drawTrianglesCached(2, key, editFill_, f, m, !wireframe_);
        renderer_.drawLinesCached(0, key, editLines_, f, m, !wireframe_, false, selMode_ == SelMode::Edge ? 2.0f : 1.0f);
        renderer_.drawLinesCached(1, key, editPoints_, f, m, !wireframe_, true,
                                  (selMode_ == SelMode::Face ? 2.5f : 3.5f) * fontScale_);
    }

    // Gizmos for lights, bones, empties and emitters (drawn on top).
    std::vector<LineVertex> gizmo;
    std::vector<ParticleVertex> glows;
    {
        PROF_SCOPE("gizmos");
        appendGizmos(gizmo, glows);
    }
    renderer_.drawLines(gizmo, f, Mat4(), false);

    // Particles: smoke (alpha blended) first, then glowing (additive) ones.
    PROF_SCOPE("particle draw");
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
