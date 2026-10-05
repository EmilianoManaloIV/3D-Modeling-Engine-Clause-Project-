#pragma once
// Parametric (procedural) modeling: an object remembers the recipe that
// built it - shape type, its parameters, plus a small non-destructive
// modifier stack (subdivision, twist, taper) - and is regenerated whenever a
// parameter changes. "Bake" turns it into a plain editable mesh.
// (Procedural modeling, FoCG 5e sec. 16.6; the data-driven approach of
// GEA Vol. II sec. 16.3 applied to geometry.)
#include "mesh.h"

enum ParamShape {
    PS_None = -1,
    PS_Cube = 0,
    PS_Sphere,
    PS_Cylinder,
    PS_Cone,
    PS_Plane,
    PS_Torus,
    PS_Stairs,
    PS_Gear,
    PS_Pipe,
    PS_Spring,
    PS_Count
};

struct ParamDef {
    const char* name;
    float def, lo, hi, speed;
    bool integer;
};

constexpr int kMaxShapeParams = 6;

struct ShapeDef {
    const char* name;
    int count;
    ParamDef params[kMaxShapeParams];
};

struct ParametricSpec {
    int shape = PS_None;
    float p[kMaxShapeParams] = {};
    int subdivisions = 0;  // Catmull-Clark levels (0-3)
    float twist = 0;       // degrees around Y from bottom to top
    float taper = 1;       // XZ scale at the top relative to the bottom
    bool active() const { return shape >= 0 && shape < PS_Count; }
};

const ShapeDef& shapeDef(int shape);
ParametricSpec defaultSpec(int shape);
void clampSpec(ParametricSpec& spec);
Mesh generateParametric(const ParametricSpec& spec);
