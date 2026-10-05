#pragma once
// Minimal 3D math for the modeler.
//
// Conventions follow "Fundamentals of Computer Graphics" (5th ed.):
//   - column vectors; matrices multiply on the left (p' = M p)      [ch. 6, 7]
//   - right-handed world, +Y up, the camera looks down its -Z axis  [ch. 8]
// Storage is column-major so a Mat4 can be handed straight to OpenGL.
// See also "Game Engine Architecture" Vol. I, ch. 5 (3D Math for Games).
#include <algorithm>
#include <cmath>

constexpr float kPi = 3.14159265358979323846f;
inline float toRadians(float deg) { return deg * (kPi / 180.0f); }
inline float toDegrees(float rad) { return rad * (180.0f / kPi); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------------------------------------------------------------------
struct Vec2 {
    float x = 0, y = 0;
    Vec2() = default;
    Vec2(float x_, float y_) : x(x_), y(y_) {}
    Vec2& operator+=(Vec2 b) { x += b.x; y += b.y; return *this; }
    Vec2& operator-=(Vec2 b) { x -= b.x; y -= b.y; return *this; }
};
inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(Vec2 a, float s) { return {a.x * s, a.y * s}; }
inline Vec2 operator/(Vec2 a, float s) { return {a.x / s, a.y / s}; }
inline float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline float length(Vec2 a) { return std::sqrt(dot(a, a)); }

// ---------------------------------------------------------------------------
struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    float& operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }
    Vec3& operator+=(Vec3 b) { x += b.x; y += b.y; z += b.z; return *this; }
    Vec3& operator-=(Vec3 b) { x -= b.x; y -= b.y; z -= b.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator-(Vec3 a) { return {-a.x, -a.y, -a.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator*(float s, Vec3 a) { return a * s; }
inline Vec3 operator/(Vec3 a, float s) { return a * (1.0f / s); }
inline Vec3 mul(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalize(Vec3 a) {
    float l = length(a);
    return l > 1e-20f ? a / l : Vec3();
}
inline Vec3 vmin(Vec3 a, Vec3 b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
inline Vec3 vmax(Vec3 a, Vec3 b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
inline Vec3 lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }

// ---------------------------------------------------------------------------
struct Vec4 {
    float x = 0, y = 0, z = 0, w = 0;
    Vec4() = default;
    Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    Vec4(Vec3 v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
    Vec3 xyz() const { return {x, y, z}; }
};

// ---------------------------------------------------------------------------
// 4x4 matrix, column-major. Element (row r, column c) lives at m[c * 4 + r].
struct Mat4 {
    float m[16];
    Mat4() {
        for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    float& operator()(int r, int c) { return m[c * 4 + r]; }
    float operator()(int r, int c) const { return m[c * 4 + r]; }
    Vec3 row3(int r) const { return {(*this)(r, 0), (*this)(r, 1), (*this)(r, 2)}; }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a(rr, k) * b(k, c);
            r(rr, c) = s;
        }
    return r;
}

inline Vec4 operator*(const Mat4& a, Vec4 v) {
    return {a(0, 0) * v.x + a(0, 1) * v.y + a(0, 2) * v.z + a(0, 3) * v.w,
            a(1, 0) * v.x + a(1, 1) * v.y + a(1, 2) * v.z + a(1, 3) * v.w,
            a(2, 0) * v.x + a(2, 1) * v.y + a(2, 2) * v.z + a(2, 3) * v.w,
            a(3, 0) * v.x + a(3, 1) * v.y + a(3, 2) * v.z + a(3, 3) * v.w};
}

inline Vec3 transformPoint(const Mat4& a, Vec3 p) { return (a * Vec4(p, 1.0f)).xyz(); }
inline Vec3 transformDir(const Mat4& a, Vec3 d) { return (a * Vec4(d, 0.0f)).xyz(); }

inline Mat4 transpose(const Mat4& a) {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r(i, j) = a(j, i);
    return r;
}

inline Mat4 translation(Vec3 t) {
    Mat4 r;
    r(0, 3) = t.x; r(1, 3) = t.y; r(2, 3) = t.z;
    return r;
}

inline Mat4 scaling(Vec3 s) {
    Mat4 r;
    r(0, 0) = s.x; r(1, 1) = s.y; r(2, 2) = s.z;
    return r;
}

inline Mat4 rotationX(float rad) {
    Mat4 r; float c = std::cos(rad), s = std::sin(rad);
    r(1, 1) = c; r(1, 2) = -s; r(2, 1) = s; r(2, 2) = c;
    return r;
}
inline Mat4 rotationY(float rad) {
    Mat4 r; float c = std::cos(rad), s = std::sin(rad);
    r(0, 0) = c; r(0, 2) = s; r(2, 0) = -s; r(2, 2) = c;
    return r;
}
inline Mat4 rotationZ(float rad) {
    Mat4 r; float c = std::cos(rad), s = std::sin(rad);
    r(0, 0) = c; r(0, 1) = -s; r(1, 0) = s; r(1, 1) = c;
    return r;
}

// Rotation about an arbitrary axis (Rodrigues' formula, FoCG 5e sec. 7.2).
inline Mat4 rotationAxis(Vec3 axis, float rad) {
    Vec3 a = normalize(axis);
    float c = std::cos(rad), s = std::sin(rad), t = 1.0f - c;
    Mat4 r;
    r(0, 0) = t * a.x * a.x + c;       r(0, 1) = t * a.x * a.y - s * a.z; r(0, 2) = t * a.x * a.z + s * a.y;
    r(1, 0) = t * a.x * a.y + s * a.z; r(1, 1) = t * a.y * a.y + c;       r(1, 2) = t * a.y * a.z - s * a.x;
    r(2, 0) = t * a.x * a.z - s * a.y; r(2, 1) = t * a.y * a.z + s * a.x; r(2, 2) = t * a.z * a.z + c;
    return r;
}

// Euler angles in degrees, applied X first, then Y, then Z: R = Rz * Ry * Rx.
inline Mat4 eulerToMatrix(Vec3 deg) {
    return rotationZ(toRadians(deg.z)) * rotationY(toRadians(deg.y)) * rotationX(toRadians(deg.x));
}

// Inverse of eulerToMatrix for a pure rotation matrix (handles gimbal lock).
inline Vec3 matrixToEuler(const Mat4& r) {
    float sy = clampf(-r(2, 0), -1.0f, 1.0f);
    float y = std::asin(sy);
    float x, z;
    if (std::fabs(sy) < 0.99999f) {
        x = std::atan2(r(2, 1), r(2, 2));
        z = std::atan2(r(1, 0), r(0, 0));
    } else {
        x = std::atan2(-r(1, 2), r(1, 1));
        z = 0.0f;
    }
    return {toDegrees(x), toDegrees(y), toDegrees(z)};
}

// Perspective projection (FoCG 5e sec. 8.3), OpenGL clip-space conventions.
inline Mat4 perspective(float fovyRad, float aspect, float n, float f) {
    Mat4 r;
    float t = 1.0f / std::tan(fovyRad * 0.5f);
    r(0, 0) = t / aspect;
    r(1, 1) = t;
    r(2, 2) = (f + n) / (n - f);
    r(2, 3) = 2.0f * f * n / (n - f);
    r(3, 2) = -1.0f;
    r(3, 3) = 0.0f;
    return r;
}

// Orthographic projection (FoCG 5e sec. 8.1).
inline Mat4 orthographic(float l, float rr, float b, float t, float n, float f) {
    Mat4 r;
    r(0, 0) = 2.0f / (rr - l);
    r(1, 1) = 2.0f / (t - b);
    r(2, 2) = -2.0f / (f - n);
    r(0, 3) = -(rr + l) / (rr - l);
    r(1, 3) = -(t + b) / (t - b);
    r(2, 3) = -(f + n) / (f - n);
    return r;
}

// Camera (view) transform, FoCG 5e sec. 8.1.
inline Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up) {
    Vec3 f = normalize(target - eye);
    Vec3 s = normalize(cross(f, up));
    Vec3 u = cross(s, f);
    Mat4 r;
    r(0, 0) = s.x;  r(0, 1) = s.y;  r(0, 2) = s.z;  r(0, 3) = -dot(s, eye);
    r(1, 0) = u.x;  r(1, 1) = u.y;  r(1, 2) = u.z;  r(1, 3) = -dot(u, eye);
    r(2, 0) = -f.x; r(2, 1) = -f.y; r(2, 2) = -f.z; r(2, 3) = dot(f, eye);
    return r;
}

// General 4x4 inverse by cofactor expansion. Returns identity if singular.
inline Mat4 inverse(const Mat4& mat) {
    const float* m = mat.m;
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    Mat4 r;
    if (std::fabs(det) < 1e-30f) return r;
    float invDet = 1.0f / det;
    for (int i = 0; i < 16; ++i) r.m[i] = inv[i] * invDet;
    return r;
}

// Splits an affine matrix into translation, Euler rotation (degrees) and
// scale. Shear (from non-uniform parent scale) cannot be represented and is
// approximated - the usual trade-off for TRS hierarchies (GEA Vol. I sec. 5.3).
inline void decomposeTRS(const Mat4& m, Vec3& pos, Vec3& rotDeg, Vec3& scl) {
    pos = {m(0, 3), m(1, 3), m(2, 3)};
    Vec3 c0{m(0, 0), m(1, 0), m(2, 0)}, c1{m(0, 1), m(1, 1), m(2, 1)}, c2{m(0, 2), m(1, 2), m(2, 2)};
    scl = {length(c0), length(c1), length(c2)};
    if (dot(cross(c0, c1), c2) < 0) scl.x = -scl.x;
    Mat4 r;
    for (int i = 0; i < 3; ++i) {
        r(i, 0) = std::fabs(scl.x) > 1e-12f ? c0[i] / scl.x : (i == 0 ? 1.0f : 0.0f);
        r(i, 1) = std::fabs(scl.y) > 1e-12f ? c1[i] / scl.y : (i == 1 ? 1.0f : 0.0f);
        r(i, 2) = std::fabs(scl.z) > 1e-12f ? c2[i] / scl.z : (i == 2 ? 1.0f : 0.0f);
    }
    rotDeg = matrixToEuler(r);
}
