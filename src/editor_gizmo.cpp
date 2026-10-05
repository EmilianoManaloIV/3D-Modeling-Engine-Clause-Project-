// Unity-style transform handles (Move / Rotate / Scale / combined Transform).
//
// The handle sits at the selection's pivot (object origin, or the selection
// centre with "Center"), oriented to the world ("Global") or to the active
// object ("Local"; the Scale handle is always local, as in Unity). Handles
// keep a constant on-screen size. Dragging one starts a transform
// (beginTransform with fromGizmo = true) and each frame converts the mouse
// motion into an XfDelta:
//   arrows      - motion projected onto the axis' screen direction
//   planes      - mouse ray intersected with the handle plane (FoCG 5e sec. 4.4)
//   rings       - motion along the ring's screen tangent (arc length -> angle)
//   outer ring  - angle around the pivot on screen (view axis)
//   inside      - trackball rotation
//   scale cubes - motion along the axis relative to the handle length
// Ctrl snaps (0.25 units / 15 degrees / 0.1 scale), Shift+drag on a Move
// handle in Edit mode extrudes first (like ProBuilder).
#include "editor_internal.h"
#include "skin.h"
#include "transform.h"

#include <cmath>
#include <cstdio>

using namespace ed;

namespace {
const Color kHandleHot{1.0f, 0.86f, 0.18f, 1};
constexpr int kRingSegments = 64;

bool isMoveHandle(int h) { return h >= GH_MoveX && h <= GH_MoveFree; }
bool isRotateHandle(int h) { return h >= GH_RotX && h <= GH_RotFree; }

bool insideQuad(Vec2 p, const Vec2 q[4]) {
    // Convex quad: p must be on the same side of all four edges.
    float sign = 0;
    for (int i = 0; i < 4; ++i) {
        Vec2 a = q[i], b = q[(i + 1) % 4];
        float c = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        if (std::fabs(c) < 1e-6f) continue;
        if (sign == 0) sign = c;
        else if ((c > 0) != (sign > 0)) return false;
    }
    return true;
}
}  // namespace

bool Editor::localBounds(const Object& o, Vec3& lo, Vec3& hi) {
    if (!o.isMesh() || o.mesh.verts.empty()) return false;
    BoundsCache& c = boundsCache_[o.id];
    if (c.version != o.mesh.version) {
        c.lo = c.hi = o.mesh.verts[0];
        for (const Vec3& v : o.mesh.verts) {
            c.lo = vmin(c.lo, v);
            c.hi = vmax(c.hi, v);
        }
        c.version = o.mesh.version;
    }
    lo = c.lo;
    hi = c.hi;
    return true;
}

void Editor::computeGizmo() {
    if (gizmoDrag_ != GH_None) return;  // frozen while a handle is dragged
    gizmoVisible_ = false;
    if (tool_ == Tool::Hand || xf_ != Xform::None) return;

    Vec3 pivot;
    Mat4 frame;
    const bool wantLocal = localSpace_ || tool_ == Tool::Scale;
    if (mode_ == Mode::Object) {
        std::vector<int> roots = transformRoots();
        if (roots.empty()) return;
        int ref = (scene_.active >= 0 && scene_.objects[scene_.active].selected) ? scene_.active : roots[0];
        if (!pivotCenter_) {
            pivot = tf::position(scene_, ref);
        } else {  // centre of the selection's bounds
            Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
            for (int i : roots) {
                Mat4 w = scene_.world(i);
                Vec3 blo, bhi;
                if (localBounds(scene_.objects[i], blo, bhi)) {
                    for (int c = 0; c < 8; ++c) {
                        Vec3 corner((c & 1) ? bhi.x : blo.x, (c & 2) ? bhi.y : blo.y, (c & 4) ? bhi.z : blo.z);
                        Vec3 p = transformPoint(w, corner);
                        lo = vmin(lo, p);
                        hi = vmax(hi, p);
                    }
                } else {
                    Vec3 p = transformPoint(w, Vec3());
                    lo = vmin(lo, p);
                    hi = vmax(hi, p);
                }
            }
            pivot = (lo + hi) * 0.5f;
        }
        if (wantLocal) frame = tf::rotation(scene_, ref);
    } else {
        Object* o = activeMesh();
        if (!o) return;
        const auto& sel = vertSel();
        const Mat4 w = scene_.world(scene_.active);
        Vec3 sum, lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
        int n = 0;
        for (size_t v = 0; v < sel.size(); ++v)
            if (sel[v]) {
                Vec3 p = transformPoint(w, o->mesh.verts[v]);
                sum += p;
                lo = vmin(lo, p);
                hi = vmax(hi, p);
                ++n;
            }
        if (!n) return;
        pivot = pivotCenter_ ? (lo + hi) * 0.5f : sum / float(n);
        if (wantLocal) frame = tf::orthonormalized(w);
    }
    gizmoPivot_ = pivot;
    gizmoAxes_ = frame;
    gizmoSize_ = worldPerPixel(pivot) * 80.0f * dpi_;
    gizmoVisible_ = true;
}

int Editor::pickHandle(Vec2 p) {
    if (!gizmoVisible_) return GH_None;
    Vec2 c;
    if (!worldToScreen(gizmoPivot_, c)) return GH_None;
    const float tol = 7.0f * dpi_, S = gizmoSize_;
    const bool move = tool_ == Tool::Move || tool_ == Tool::Universal;
    const bool rotate = tool_ == Tool::Rotate || tool_ == Tool::Universal;
    const bool scale = tool_ == Tool::Scale;
    const float arrowLen = tool_ == Tool::Universal ? 0.75f * S : S;
    const Vec3 eye = cam_.eye();
    const Vec3 toViewer = cam_.ortho ? -camForward() : normalize(eye - gizmoPivot_);

    // 1. centre squares
    if ((scale || tool_ == Tool::Universal) && length(p - c) < 9.0f * dpi_) return GH_ScaleUniform;
    if (tool_ == Tool::Move && length(p - c) < 7.0f * dpi_) return GH_MoveFree;

    // 2. plane squares (on the camera-facing side, like Unity)
    if (move)
        for (int n = 0; n < 3; ++n) {
            Vec3 a = gizmoAxis((n + 1) % 3), b = gizmoAxis((n + 2) % 3);
            float sa = dot(a, toViewer) >= 0 ? 1.0f : -1.0f, sb = dot(b, toViewer) >= 0 ? 1.0f : -1.0f;
            Vec2 q[4];
            const float t0 = 0.12f * arrowLen, t1 = 0.38f * arrowLen;
            Vec3 corners[4] = {gizmoPivot_ + a * (sa * t0) + b * (sb * t0), gizmoPivot_ + a * (sa * t1) + b * (sb * t0),
                               gizmoPivot_ + a * (sa * t1) + b * (sb * t1), gizmoPivot_ + a * (sa * t0) + b * (sb * t1)};
            bool ok = true;
            for (int k = 0; k < 4; ++k) ok &= worldToScreen(corners[k], q[k]);
            if (ok && insideQuad(p, q)) return GH_MoveYZ + n;
        }

    // 3. axes
    int best = GH_None;
    float bestD = tol;
    if (move || scale)
        for (int i = 0; i < 3; ++i) {
            Vec2 b;
            if (!worldToScreen(gizmoPivot_ + gizmoAxis(i) * arrowLen, b) || length(b - c) < 6.0f) continue;
            float d = distanceToSegment2D(p, c, b);
            if (scale) d = std::min(d, length(p - b) - 3.0f * dpi_);
            if (d < bestD) {
                bestD = d;
                best = (move ? GH_MoveX : GH_ScaleX) + i;
            }
        }
    if (best != GH_None) return best;

    // 4. rings
    if (rotate) {
        for (int i = 0; i < 3; ++i) {
            Vec3 u = gizmoAxis((i + 1) % 3), v = gizmoAxis((i + 2) % 3);
            Vec2 prev;
            bool prevOk = false;
            for (int k = 0; k <= kRingSegments; ++k) {
                float t = 2 * kPi * k / kRingSegments;
                Vec3 dir = u * std::cos(t) + v * std::sin(t);
                Vec2 s;
                bool ok = worldToScreen(gizmoPivot_ + dir * S, s) && dot(dir, toViewer) > -0.1f;
                if (ok && prevOk) {
                    float d = distanceToSegment2D(p, prev, s);
                    if (d < bestD) {
                        bestD = d;
                        best = GH_RotX + i;
                    }
                }
                prev = s;
                prevOk = ok;
            }
        }
        if (best != GH_None) return best;
        Vec2 edge;
        worldToScreen(gizmoPivot_ + camRight() * S, edge);
        float ringPx = length(edge - c);
        if (std::fabs(length(p - c) - ringPx * 1.18f) < tol) return GH_RotView;
        if (tool_ == Tool::Rotate && length(p - c) < ringPx * 0.92f) return GH_RotFree;
    }
    return GH_None;
}

bool Editor::beginGizmoDrag(int h, const Input& in) {
    Xform type = isMoveHandle(h) ? Xform::Grab : isRotateHandle(h) ? Xform::Rotate : Xform::Scale;
    bool extruded = false;
    if (mode_ == Mode::Edit && isMoveHandle(h) && in.shift()) {  // ProBuilder-style Shift+drag extrude
        Object* o = activeMesh();
        if (o && !selectedFaces(o->mesh, vertSel()).empty()) {
            pushUndo();
            Vec3 n;
            extrudeSelectedFaces(o->mesh, vertSel(), &n);
            markDirty();
            extruded = true;
        }
    }
    if (!beginTransform(type, !extruded, true)) return false;
    gizmoDrag_ = h;
    gizmoMouseStart_ = mouse_;
    if (h >= GH_MoveYZ && h <= GH_MoveXY) {
        Vec3 n = gizmoAxis(h - GH_MoveYZ), o, d;
        viewRay(mouse_, o, d);
        float denom = dot(d, n);
        gizmoPlaneStart_ = std::fabs(denom) > 1e-6f ? o + d * (dot(gizmoPivot_ - o, n) / denom) : gizmoPivot_;
    } else if (h >= GH_RotX && h <= GH_RotZ) {
        // Grab point on the ring nearest to the mouse; drag along its screen tangent.
        const int i = h - GH_RotX;
        Vec3 axis = gizmoAxis(i), u = gizmoAxis((i + 1) % 3), v = gizmoAxis((i + 2) % 3), best = u;
        float bestD = 1e30f;
        for (int k = 0; k < kRingSegments; ++k) {
            float t = 2 * kPi * k / kRingSegments;
            Vec3 dir = u * std::cos(t) + v * std::sin(t);
            Vec2 s;
            if (worldToScreen(gizmoPivot_ + dir * gizmoSize_, s) && length(s - mouse_) < bestD) {
                bestD = length(s - mouse_);
                best = dir;
            }
        }
        Vec3 P = gizmoPivot_ + best * gizmoSize_, T = cross(axis, best);
        Vec2 s0, s1, c;
        worldToScreen(P, s0);
        worldToScreen(P + T * (gizmoSize_ * 0.1f), s1);
        worldToScreen(gizmoPivot_, c);
        gizmoTangent_ = length(s1 - s0) > 1e-4f ? (s1 - s0) / length(s1 - s0) : Vec2(1, 0);
        gizmoRadiusPx_ = std::max(10.0f, length(s0 - c));
    }
    return true;
}

void Editor::updateGizmoDrag(const Input& in) {
    const int h = gizmoDrag_;
    const bool snap = in.ctrl();
    const Vec2 delta = mouse_ - gizmoMouseStart_;
    XfDelta d;
    d.basis = gizmoAxes_;
    static const char* axisNames[3] = {"X", "Y", "Z"};

    if (h >= GH_MoveX && h <= GH_MoveZ) {
        const int i = h - GH_MoveX;
        float t = axisDrag(xfPivot_, gizmoAxis(i), delta);
        if (snap) t = std::round(t * 4.0f) / 4.0f;
        d.move = gizmoAxis(i) * t;
        xfInfo_ = strf("Move %s %.3f", axisNames[i], t);
    } else if (h >= GH_MoveYZ && h <= GH_MoveXY) {
        const int n = h - GH_MoveYZ;
        Vec3 normal = gizmoAxis(n), o, dir;
        viewRay(mouse_, o, dir);
        float denom = dot(dir, normal);
        if (std::fabs(denom) > 1e-6f) {
            Vec3 hit = o + dir * (dot(xfPivot_ - o, normal) / denom);
            Vec3 diff = hit - gizmoPlaneStart_;
            if (snap) {
                Vec3 snapped;
                for (int k = 1; k <= 2; ++k) {
                    Vec3 a = gizmoAxis((n + k) % 3);
                    snapped += a * (std::round(dot(diff, a) * 4.0f) / 4.0f);
                }
                diff = snapped;
            }
            d.move = diff;
        }
        xfInfo_ = strf("Move (%.3f, %.3f, %.3f)", d.move.x, d.move.y, d.move.z);
    } else if (h == GH_MoveFree) {
        float k = worldPerPixel(xfPivot_);
        d.move = camRight() * (delta.x * k) - camUp() * (delta.y * k);
        if (snap)
            for (int i = 0; i < 3; ++i) d.move[i] = std::round(d.move[i] * 4.0f) / 4.0f;
        xfInfo_ = strf("Move (%.3f, %.3f, %.3f)", d.move.x, d.move.y, d.move.z);
    } else if (h >= GH_RotX && h <= GH_RotZ) {
        float deg = toDegrees(dot(delta, gizmoTangent_) / gizmoRadiusPx_);
        if (snap) deg = std::round(deg / 15.0f) * 15.0f;
        d.rotAxis = gizmoAxis(h - GH_RotX);
        d.rotDeg = deg;
        xfInfo_ = strf("Rotate %s %.1f deg", axisNames[h - GH_RotX], deg);
    } else if (h == GH_RotView) {
        Vec2 c;
        worldToScreen(xfPivot_, c);
        float a = std::atan2(-(mouse_.y - c.y), mouse_.x - c.x);
        xfAngle_ += wrapRadians(a - xfPrevAngle_);
        xfPrevAngle_ = a;
        float deg = toDegrees(xfAngle_);
        if (snap) deg = std::round(deg / 15.0f) * 15.0f;
        d.rotAxis = -camForward();
        d.rotDeg = deg;
        xfInfo_ = strf("Rotate (view) %.1f deg", deg);
    } else if (h == GH_RotFree) {
        if (length(delta) > 1e-3f) {
            d.rotAxis = normalize(camUp() * delta.x + camRight() * delta.y);
            d.rotDeg = length(delta) * 0.4f;
        }
        xfInfo_ = strf("Rotate (free) %.1f deg", d.rotDeg);
    } else if (h >= GH_ScaleX && h <= GH_ScaleZ) {
        const int i = h - GH_ScaleX;
        float f = 1.0f + axisDrag(xfPivot_, gizmoAxis(i), delta) / std::max(1e-6f, gizmoSize_);
        if (snap) f = std::round(f * 10.0f) / 10.0f;
        d.scale[i] = f;
        xfInfo_ = strf("Scale %s x%.3f", axisNames[i], f);
    } else if (h == GH_ScaleUniform) {
        float f = std::max(0.01f, 1.0f + (delta.x - delta.y) / (100.0f * dpi_));
        if (snap) f = std::max(0.1f, std::round(f * 10.0f) / 10.0f);
        d.scale = {f, f, f};
        xfInfo_ = strf("Scale x%.3f", f);
    }
    applyTransform(d);
}

// Drawn in the 2D overlay: thick lines and filled shapes regardless of the
// GL line-width limits of core profiles.
void Editor::drawGizmo() {
    if (gizmoDrag_ == GH_None) computeGizmo();
    if (!gizmoVisible_) return;
    Vec2 c;
    if (!worldToScreen(gizmoPivot_, c)) return;
    const Vec2 origin(viewport_.x, viewport_.y);
    auto scr = [&](Vec3 w, Vec2& out) {
        Vec2 s;
        if (!worldToScreen(w, s)) return false;
        out = origin + s;
        return true;
    };
    const Vec2 cw = origin + c;
    const float S = gizmoSize_, th = std::max(2.0f, 2.2f * dpi_);
    const int focus = gizmoDrag_ != GH_None ? gizmoDrag_ : gizmoHover_;
    auto colorFor = [&](int handle, Color base) {
        if (handle == focus) return kHandleHot;
        if (gizmoDrag_ != GH_None) return withAlpha(base, 0.3f);
        return base;
    };
    const bool move = tool_ == Tool::Move || tool_ == Tool::Universal;
    const bool rotate = tool_ == Tool::Rotate || tool_ == Tool::Universal;
    const bool scale = tool_ == Tool::Scale;
    const float arrowLen = tool_ == Tool::Universal ? 0.75f * S : S;
    const Vec3 toViewer = cam_.ortho ? -camForward() : normalize(cam_.eye() - gizmoPivot_);

    if (rotate) {
        for (int i = 0; i < 3; ++i) {
            Vec3 u = gizmoAxis((i + 1) % 3), v = gizmoAxis((i + 2) % 3);
            Color col = colorFor(GH_RotX + i, kAxisColor[i]);
            Vec2 prev;
            bool prevOk = false;
            for (int k = 0; k <= kRingSegments; ++k) {
                float t = 2 * kPi * k / kRingSegments;
                Vec3 dir = u * std::cos(t) + v * std::sin(t);
                Vec2 s;
                bool ok = scr(gizmoPivot_ + dir * S, s);
                if (ok && prevOk) {
                    bool front = dot(dir, toViewer) > -0.1f;
                    ui_.line(prev.x, prev.y, s.x, s.y, front ? th : std::max(1.0f, th * 0.5f),
                             front ? col : withAlpha(col, col.a * 0.25f));
                }
                prev = s;
                prevOk = ok;
            }
        }
        Vec2 edge;
        scr(gizmoPivot_ + camRight() * S, edge);
        float r = length(edge - cw) * 1.18f;
        Color vc = colorFor(GH_RotView, {0.85f, 0.87f, 0.92f, 0.8f});
        for (int k = 0; k < kRingSegments; ++k) {
            float a0 = 2 * kPi * k / kRingSegments, a1 = 2 * kPi * (k + 1) / kRingSegments;
            ui_.line(cw.x + r * std::cos(a0), cw.y + r * std::sin(a0), cw.x + r * std::cos(a1), cw.y + r * std::sin(a1),
                     th * 0.8f, vc);
        }
        if (focus == GH_RotFree) {
            float rr = length(edge - cw);
            for (int k = 0; k < kRingSegments; ++k) {
                float a0 = 2 * kPi * k / kRingSegments, a1 = 2 * kPi * (k + 1) / kRingSegments;
                ui_.line(cw.x + rr * std::cos(a0), cw.y + rr * std::sin(a0), cw.x + rr * std::cos(a1),
                         cw.y + rr * std::sin(a1), 1.0f, withAlpha(kHandleHot, 0.6f));
            }
        }
    }

    if (move) {
        for (int n = 0; n < 3; ++n) {  // plane squares
            Vec3 a = gizmoAxis((n + 1) % 3), b = gizmoAxis((n + 2) % 3);
            float sa = dot(a, toViewer) >= 0 ? 1.0f : -1.0f, sb = dot(b, toViewer) >= 0 ? 1.0f : -1.0f;
            const float t0 = 0.12f * arrowLen, t1 = 0.38f * arrowLen;
            Vec3 corners[4] = {gizmoPivot_ + a * (sa * t0) + b * (sb * t0), gizmoPivot_ + a * (sa * t1) + b * (sb * t0),
                               gizmoPivot_ + a * (sa * t1) + b * (sb * t1), gizmoPivot_ + a * (sa * t0) + b * (sb * t1)};
            Vec2 q[4];
            bool ok = true;
            for (int k = 0; k < 4; ++k) ok &= scr(corners[k], q[k]);
            if (!ok) continue;
            Color col = colorFor(GH_MoveYZ + n, kAxisColor[n]);
            ui_.triangle(q[0], q[1], q[2], withAlpha(col, col.a * (GH_MoveYZ + n == focus ? 0.55f : 0.28f)));
            ui_.triangle(q[0], q[2], q[3], withAlpha(col, col.a * (GH_MoveYZ + n == focus ? 0.55f : 0.28f)));
            for (int k = 0; k < 4; ++k) ui_.line(q[k].x, q[k].y, q[(k + 1) % 4].x, q[(k + 1) % 4].y, 1.0f, col);
        }
    }

    if (move || scale) {
        for (int i = 0; i < 3; ++i) {
            Vec2 tip;
            if (!scr(gizmoPivot_ + gizmoAxis(i) * arrowLen, tip) || length(tip - cw) < 6.0f) continue;
            Color col = colorFor((move ? GH_MoveX : GH_ScaleX) + i, kAxisColor[i]);
            ui_.line(cw.x, cw.y, tip.x, tip.y, th, col);
            Vec2 dir = (tip - cw) / length(tip - cw), perp(-dir.y, dir.x);
            if (move) {
                float len = 13.0f * dpi_, half = 5.0f * dpi_;
                ui_.triangle(tip + dir * len, tip + perp * half, tip - perp * half, col);
            } else {
                float half = 5.0f * dpi_;
                ui_.rect({tip.x - half, tip.y - half, 2 * half, 2 * half}, col);
            }
        }
    }
    if (tool_ == Tool::Move) {
        float half = 6.0f * dpi_;
        ui_.border({cw.x - half, cw.y - half, 2 * half, 2 * half}, colorFor(GH_MoveFree, {0.9f, 0.9f, 0.95f, 0.9f}),
                   std::max(1.0f, dpi_));
    }
    if (scale || tool_ == Tool::Universal) {
        float half = 6.5f * dpi_;
        ui_.rect({cw.x - half, cw.y - half, 2 * half, 2 * half}, colorFor(GH_ScaleUniform, {0.9f, 0.9f, 0.95f, 0.95f}));
    }
}

// ----------------------------------------------------------------------------
// Scene gizmo (Unity's view cube in the top-right corner of the Scene view):
// six axis arms; clicking one looks along that axis (and switches to Iso,
// like Unity), clicking the centre or the Persp/Iso label toggles projection.
// ----------------------------------------------------------------------------
void Editor::sceneGizmoLayout(Vec2& center, float& len) const {
    const float fs = (float)fontScale_;
    len = 20.0f * fs;
    center = {viewport_.x + viewport_.w - len - 14.0f * fs, headerRect_.y + headerRect_.h + len + 12.0f * fs};
}

int Editor::pickSceneGizmo(Vec2 p) {
    const float fs = (float)fontScale_;
    Vec2 c;
    float len;
    sceneGizmoLayout(c, len);
    sceneGizmoRect_ = {c.x - len - 8 * fs, c.y - len - 8 * fs, 2 * len + 16 * fs, 2 * len + 26 * fs};
    if (!sceneGizmoRect_.contains(p.x, p.y)) return -1;
    int best = -1;
    float bestZ = -1e9f;
    for (int k = 0; k < 6; ++k) {
        const int i = k / 2;
        const float sgn = (k % 2) ? -1.0f : 1.0f;
        Vec2 e(c.x + view_(0, i) * sgn * len, c.y - view_(1, i) * sgn * len);
        float z = view_(2, i) * sgn;  // toward the viewer = in front
        float r = (k % 2 ? 5.0f : 7.0f) * fs;
        if (length(p - e) <= r && z > bestZ) {
            bestZ = z;
            best = k;
        }
    }
    if (best >= 0) return best;
    if (length(p - c) < 6.0f * fs) return 6;
    if (p.y > c.y + len + 2 * fs) return 6;  // the Persp / Iso label
    return -1;
}

void Editor::clickSceneGizmo(int part) {
    if (part == 6) {
        cam_.ortho = !cam_.ortho;
        if (camAnimating_) camTo_.ortho = cam_.ortho;
        setStatus(cam_.ortho ? "Iso (orthographic)" : "Persp (perspective)");
        return;
    }
    static const float views[6][2] = {{90, 0}, {-90, 0}, {0, 89.9f}, {0, -89.9f}, {0, 0}, {180, 0}};
    static const char* names[6] = {"Right", "Left", "Top", "Bottom", "Front", "Back"};
    Camera goal = camAnimating_ ? camTo_ : cam_;
    goal.yaw = views[part][0];
    goal.pitch = views[part][1];
    goal.ortho = true;  // Unity switches to Iso when snapping to an axis
    animateCameraTo(goal);
    setStatus(std::string(names[part]) + " view (Iso) - click the label under the gizmo for Persp");
}

// ----------------------------------------------------------------------------
// Keymap preference (persisted next to the executable)
// ----------------------------------------------------------------------------
void Editor::setKeymap(Keymap k) {
    keymap_ = k;
    keys_.setPreset(k == Keymap::Unity ? input::Preset::Unity : input::Preset::Blender);
    if (k == Keymap::Blender && tool_ == Tool::Hand) tool_ = Tool::Move;
    saveConfig();
    setStatus(k == Keymap::Unity ? "Unity keymap: Q W E R Y tools, Alt+LMB orbit, RMB+WASD fly"
                                 : "Blender keymap: G R S modal transforms, RMB/MMB orbit");
}

void Editor::loadConfig() {
    keys_.setPreset(input::Preset::Unity);
    if (configPath_.empty()) return;
    FILE* f = std::fopen(configPath_.c_str(), "rb");
    if (!f) return;
    char line[256];
    bool sawBindings = false;
    std::vector<std::string> bindLines;
    while (std::fgets(line, sizeof line, f)) {
        std::string s = trimmed(line);
        float v = 0;
        int b = 0;
        char name[64];
        if (s == "keymap blender") keymap_ = Keymap::Blender;
        else if (s == "keymap unity") keymap_ = Keymap::Unity;
        else if (s.compare(0, 5, "bind ") == 0) bindLines.push_back(s);
        else if (std::sscanf(s.c_str(), "camera %63s %f", name, &v) == 2) {
            std::string n = name;
            b = v != 0.0f;
            if (n == "orbit") camSet_.orbitSensitivity = clampf(v, 0.01f, 5.0f);
            else if (n == "look") camSet_.lookSensitivity = clampf(v, 0.01f, 5.0f);
            else if (n == "pan") camSet_.panSpeed = clampf(v, 0.05f, 20.0f);
            else if (n == "zoom") camSet_.zoomSpeed = clampf(v, 0.05f, 20.0f);
            else if (n == "arrows") camSet_.arrowSpeed = clampf(v, 0.05f, 20.0f);
            else if (n == "fast") camSet_.fastMultiplier = clampf(v, 1.0f, 20.0f);
            else if (n == "transition") camSet_.transition = clampf(v, 0.0f, 3.0f);
            else if (n == "invert_x") camSet_.invertX = b;
            else if (n == "invert_y") camSet_.invertY = b;
            else if (n == "fly_accel") camSet_.flyAcceleration = b;
            else if (n == "fly_speed") flySpeed_ = clampf(v, 0.01f, 100.0f);
            else if (n == "fov") cam_.fovY = clampf(v, 10.0f, 120.0f);
        }
    }
    std::fclose(f);
    keys_.setPreset(keymap_ == Keymap::Unity ? input::Preset::Unity : input::Preset::Blender);
    for (const std::string& l : bindLines) sawBindings |= keys_.parseLine(l);
    (void)sawBindings;
}

void Editor::saveConfig() {
    if (configPath_.empty()) return;
    if (FILE* f = std::fopen(configPath_.c_str(), "wb")) {
        std::fprintf(f, "# Modeler3D settings (edit in the Keys and Camera tabs)\nkeymap %s\n",
                     keymap_ == Keymap::Unity ? "unity" : "blender");
        const CameraSettings& c = camSet_;
        std::fprintf(f,
                     "camera orbit %g\ncamera look %g\ncamera pan %g\ncamera zoom %g\ncamera arrows %g\n"
                     "camera fast %g\ncamera transition %g\ncamera invert_x %d\ncamera invert_y %d\n"
                     "camera fly_accel %d\ncamera fly_speed %g\ncamera fov %g\n",
                     c.orbitSensitivity, c.lookSensitivity, c.panSpeed, c.zoomSpeed, c.arrowSpeed, c.fastMultiplier,
                     c.transition, (int)c.invertX, (int)c.invertY, (int)c.flyAcceleration, flySpeed_, cam_.fovY);
        std::fputs(keys_.serialize().c_str(), f);
        std::fclose(f);
    }
}
