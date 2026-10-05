#include "transform.h"

namespace tf {
namespace {
Vec3 column(const Mat4& m, int c) { return {m(0, c), m(1, c), m(2, c)}; }

Mat4 fromColumns(Vec3 x, Vec3 y, Vec3 z) {
    Mat4 r;
    for (int i = 0; i < 3; ++i) {
        r(i, 0) = x[i];
        r(i, 1) = y[i];
        r(i, 2) = z[i];
    }
    return r;
}

// Local rotation matrix of the object (Euler -> matrix).
Mat4 localRotation(const Object& o) { return eulerToMatrix(o.rotation); }
}  // namespace

Mat4 orthonormalized(const Mat4& m) {
    Vec3 x = normalize(column(m, 0));
    Vec3 y = column(m, 1);
    y = normalize(y - x * dot(x, y));
    if (length(x) < 0.5f || length(y) < 0.5f) return Mat4();
    Vec3 z = cross(x, y);
    if (dot(z, column(m, 2)) < 0) {  // mirrored (negative scale): keep a proper rotation
        x = -x;
        z = cross(x, y);
    }
    return fromColumns(x, y, z);
}

Vec3 position(const Scene& s, int i) {
    Mat4 w = s.world(i);
    return {w(0, 3), w(1, 3), w(2, 3)};
}

void setPosition(Scene& s, int i, Vec3 p) { s.objects[i].position = ::transformPoint(inverse(s.parentWorld(i)), p); }

Mat4 rotation(const Scene& s, int i) { return orthonormalized(s.world(i)); }

void setRotation(Scene& s, int i, const Mat4& worldRotation) {
    Mat4 parentRot = orthonormalized(s.parentWorld(i));
    s.objects[i].rotation = matrixToEuler(transpose(parentRot) * worldRotation);
}

Vec3 lossyScale(const Scene& s, int i) {
    Mat4 w = s.world(i);
    return {length(column(w, 0)), length(column(w, 1)), length(column(w, 2))};
}

Vec3 right(const Scene& s, int i) { return column(rotation(s, i), 0); }
Vec3 up(const Scene& s, int i) { return column(rotation(s, i), 1); }
Vec3 forward(const Scene& s, int i) { return column(rotation(s, i), 2); }

void translate(Scene& s, int i, Vec3 delta, Space relativeTo) {
    Vec3 worldDelta = relativeTo == Space::Self ? transformDir(rotation(s, i), delta) : delta;
    setPosition(s, i, position(s, i) + worldDelta);
}

void rotate(Scene& s, int i, Vec3 eulerDegrees, Space relativeTo) {
    Mat4 r = eulerToMatrix(eulerDegrees);
    if (relativeTo == Space::Self) s.objects[i].rotation = matrixToEuler(localRotation(s.objects[i]) * r);
    else setRotation(s, i, r * rotation(s, i));
}

void rotate(Scene& s, int i, Vec3 axis, float degrees, Space relativeTo) {
    Mat4 r = rotationAxis(axis, toRadians(degrees));
    if (relativeTo == Space::Self) s.objects[i].rotation = matrixToEuler(localRotation(s.objects[i]) * r);
    else setRotation(s, i, r * rotation(s, i));
}

void rotateAround(Scene& s, int i, Vec3 point, Vec3 axis, float degrees) {
    Mat4 r = rotationAxis(axis, toRadians(degrees));
    Vec3 p = position(s, i);
    Mat4 worldRot = rotation(s, i);
    setPosition(s, i, point + transformDir(r, p - point));
    setRotation(s, i, r * worldRot);
}

void lookAt(Scene& s, int i, Vec3 target, Vec3 worldUp) {
    Vec3 f = target - position(s, i);
    if (length(f) < 1e-8f) return;
    f = normalize(f);
    Vec3 x = cross(worldUp, f);
    if (length(x) < 1e-6f) x = cross(std::fabs(f.x) < 0.9f ? Vec3(1, 0, 0) : Vec3(0, 0, 1), f);
    x = normalize(x);
    Vec3 y = cross(f, x);
    setRotation(s, i, fromColumns(x, y, f));
}

Vec3 transformPoint(const Scene& s, int i, Vec3 p) { return ::transformPoint(s.world(i), p); }
Vec3 inverseTransformPoint(const Scene& s, int i, Vec3 p) { return ::transformPoint(inverse(s.world(i)), p); }
Vec3 transformDirection(const Scene& s, int i, Vec3 d) { return transformDir(rotation(s, i), d); }
Vec3 inverseTransformDirection(const Scene& s, int i, Vec3 d) { return transformDir(transpose(rotation(s, i)), d); }
Vec3 transformVector(const Scene& s, int i, Vec3 v) { return transformDir(s.world(i), v); }
Vec3 inverseTransformVector(const Scene& s, int i, Vec3 v) { return transformDir(inverse(s.world(i)), v); }

void reset(Object& o) {
    o.position = Vec3();
    o.rotation = Vec3();
    o.scale = Vec3(1, 1, 1);
}

}  // namespace tf
