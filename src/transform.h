#pragma once
// Unity-style Transform API on top of the scene hierarchy.
//
// Objects store a *local* TRS (like Unity's localPosition / localRotation /
// localScale); these helpers read and write the *world* values and provide
// the familiar Translate / Rotate / RotateAround / LookAt /
// TransformPoint family. Coordinate frames follow FoCG 5e sec. 7.5 and the
// hierarchy rules of GEA Vol. I sec. 5.3: world = parentWorld * local.
//
// Conventions: right-handed world, Y up; an object's forward() is its local
// +Z axis, right() is +X, up() is +Y (Unity's axis names). Non-uniform scale
// on a *parent* can't be represented exactly by a child's TRS (shear); like
// Unity, rotations are taken from the orthonormalized matrices.
#include "scene.h"

enum class Space { Self, World };

namespace tf {

// World-space position / rotation (pure rotation matrix) / scale.
Vec3 position(const Scene& s, int i);
void setPosition(Scene& s, int i, Vec3 worldPosition);
Mat4 rotation(const Scene& s, int i);
void setRotation(Scene& s, int i, const Mat4& worldRotation);
Vec3 lossyScale(const Scene& s, int i);

// The object's axes in world space.
Vec3 right(const Scene& s, int i);
Vec3 up(const Scene& s, int i);
Vec3 forward(const Scene& s, int i);

// Movement. Space::Self interprets vectors / axes in the object's own frame.
void translate(Scene& s, int i, Vec3 delta, Space relativeTo = Space::Self);
void rotate(Scene& s, int i, Vec3 eulerDegrees, Space relativeTo = Space::Self);
void rotate(Scene& s, int i, Vec3 axis, float degrees, Space relativeTo = Space::Self);
void rotateAround(Scene& s, int i, Vec3 worldPoint, Vec3 worldAxis, float degrees);
// Points forward() at the target, keeping up() as close to worldUp as possible.
void lookAt(Scene& s, int i, Vec3 worldTarget, Vec3 worldUp = Vec3(0, 1, 0));

// Space conversions.
Vec3 transformPoint(const Scene& s, int i, Vec3 localPoint);         // local -> world (full matrix)
Vec3 inverseTransformPoint(const Scene& s, int i, Vec3 worldPoint);  // world -> local
Vec3 transformDirection(const Scene& s, int i, Vec3 localDir);       // rotation only
Vec3 inverseTransformDirection(const Scene& s, int i, Vec3 worldDir);
Vec3 transformVector(const Scene& s, int i, Vec3 localVec);          // rotation + scale
Vec3 inverseTransformVector(const Scene& s, int i, Vec3 worldVec);

// Local TRS back to identity (Unity's Transform > Reset).
void reset(Object& o);

// Rotation part of a matrix with scale (and shear) removed (Gram-Schmidt).
Mat4 orthonormalized(const Mat4& m);

}  // namespace tf
