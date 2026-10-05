#pragma once
// Constants and small helpers shared by the editor_*.cpp files.
#include "editor.h"

#include <cctype>
#include <cstring>
#include <string>

namespace ed {

inline const Color kAxisColor[3] = {{0.93f, 0.33f, 0.33f, 1}, {0.47f, 0.80f, 0.30f, 1}, {0.33f, 0.56f, 0.98f, 1}};
inline const Color kRgbColor[3] = {{1, 0.25f, 0.25f, 1}, {0.3f, 0.9f, 0.3f, 1}, {0.3f, 0.5f, 1, 1}};
inline const Color kCustomAxisColor{0.96f, 0.86f, 0.30f, 1};
inline const Color kViewportBg{0.215f, 0.225f, 0.245f, 1};
inline const Color kEdgeDark{0.04f, 0.04f, 0.06f, 0.85f};
inline const Color kBoneColor{0.55f, 0.85f, 0.95f, 1};
inline const Color kLightGizmo{1.0f, 0.88f, 0.45f, 1};
inline const Color kEmitterGizmo{1.0f, 0.55f, 0.85f, 1};

inline const Vec3 kPalette[10] = {{0.80f, 0.80f, 0.80f}, {0.90f, 0.42f, 0.33f}, {0.95f, 0.74f, 0.32f},
                                  {0.52f, 0.78f, 0.42f}, {0.36f, 0.66f, 0.90f}, {0.62f, 0.50f, 0.90f},
                                  {0.90f, 0.52f, 0.72f}, {0.40f, 0.78f, 0.74f}, {0.30f, 0.31f, 0.34f},
                                  {0.97f, 0.96f, 0.93f}};

constexpr size_t kMaxUndo = 64;
constexpr size_t kMaxSubdivCorners = 400000;

inline Color toColor(Vec3 v, float a = 1.0f) { return {v.x, v.y, v.z, a}; }

inline Vec3 axisVector(int a) {
    Vec3 v;
    v[a] = 1.0f;
    return v;
}

inline float wrapRadians(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}

inline std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

inline bool endsWithNoCase(const std::string& s, const char* suffix) {
    size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower((unsigned char)s[s.size() - n + i]) != std::tolower((unsigned char)suffix[i])) return false;
    return true;
}

// "chair" / "chair.m3d" / "chair.obj"  ->  "chair<ext>"
inline std::string withExtension(const std::string& in, const char* ext) {
    std::string p = trimmed(in);
    if (endsWithNoCase(p, ext)) return p;
    if (endsWithNoCase(p, ".m3d") || endsWithNoCase(p, ".obj")) p.resize(p.size() - 4);
    if (p.empty()) p = "scene";
    return p + ext;
}

inline std::string fileNameOf(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? path : path.substr(p + 1);
}

inline float distanceToSegment2D(Vec2 p, Vec2 a, Vec2 b) {
    Vec2 ab = b - a;
    float len2 = dot(ab, ab);
    float t = len2 > 1e-8f ? clampf(dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    return length(p - (a + ab * t));
}

}  // namespace ed
