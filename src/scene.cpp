#include "scene.h"

#include "skin.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <unordered_map>

namespace {
std::string baseName(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
    return name;
}

std::string sanitize(const std::string& s) {
    std::string r = s;
    for (char& c : r)
        if (std::isspace((unsigned char)c)) c = '_';
    return r.empty() ? std::string("object") : r;
}

const char* kKindNames[] = {"mesh", "light", "empty", "bone", "emitter"};
}  // namespace

const char* kindName(ObjectKind k) {
    static const char* names[] = {"Mesh", "Light", "Empty", "Bone", "Emitter"};
    return names[(int)k];
}

// ---------------------------------------------------------------------------
// Scene
// ---------------------------------------------------------------------------
int Scene::add(Mesh mesh, const std::string& base, Vec3 color) {
    Object o;
    o.name = base;
    o.mesh = std::move(mesh);
    o.color = color;
    return addObject(std::move(o));
}

int Scene::addObject(Object o) {
    o.id = nextId++;
    o.name = uniqueName(o.name);
    if (o.mesh.version == 0) o.mesh.touch();
    objects.push_back(std::move(o));
    return (int)objects.size() - 1;
}

std::string Scene::uniqueName(const std::string& baseIn, int ignore) const {
    std::string base = baseIn.empty() ? std::string("Object") : baseIn;
    auto exists = [&](const std::string& n) {
        for (int i = 0; i < (int)objects.size(); ++i)
            if (i != ignore && objects[i].name == n) return true;
        return false;
    };
    if (!exists(base)) return base;
    // Strip an existing ".NNN" suffix so duplicates of "Cube.001" become "Cube.002".
    std::string stem = base;
    size_t dot = stem.rfind('.');
    if (dot != std::string::npos && dot + 4 == stem.size() && std::isdigit((unsigned char)stem[dot + 1]) &&
        std::isdigit((unsigned char)stem[dot + 2]) && std::isdigit((unsigned char)stem[dot + 3]))
        stem = stem.substr(0, dot);
    for (int i = 1;; ++i) {
        char buf[16];
        std::snprintf(buf, sizeof buf, ".%03d", i);
        if (!exists(stem + buf)) return stem + buf;
    }
}

int Scene::selectedCount() const {
    int n = 0;
    for (const auto& o : objects) n += o.selected ? 1 : 0;
    return n;
}

size_t Scene::triangleCount() const {
    size_t n = 0;
    for (const auto& o : objects)
        if (o.isMesh()) n += o.mesh.triangleCount();
    return n;
}

void Scene::rebuildIdCache() const {
    idCache_.clear();
    idCache_.reserve(objects.size() * 2);
    for (int i = 0; i < (int)objects.size(); ++i) idCache_[objects[i].id] = i;
    cacheSize_ = objects.size();
    cacheData_ = objects.data();
    cacheFirst_ = objects.empty() ? 0 : objects.front().id;
    cacheLast_ = objects.empty() ? 0 : objects.back().id;
}

bool Scene::idCacheCurrent() const {
    return cacheSize_ == objects.size() && cacheData_ == objects.data() &&
           cacheFirst_ == (objects.empty() ? 0 : objects.front().id) &&
           cacheLast_ == (objects.empty() ? 0 : objects.back().id);
}

int Scene::indexOf(uint32_t id) const {
    if (id == 0) return -1;
    auto it = idCache_.find(id);
    if (it != idCache_.end() && it->second < (int)objects.size() && objects[it->second].id == id) return it->second;
    if (it == idCache_.end() && idCacheCurrent()) return -1;  // genuinely absent
    rebuildIdCache();
    it = idCache_.find(id);
    return it == idCache_.end() ? -1 : it->second;
}

int Scene::parentIndex(int i) const { return indexOf(objects[i].parent); }

Mat4 Scene::world(int i) const {
    Mat4 m = objects[i].matrix();
    // Walk up the chain (bounded, in case a file contains a cycle).
    int p = parentIndex(i);
    for (int guard = 0; p >= 0 && guard < 256; ++guard) {
        m = objects[p].matrix() * m;
        p = parentIndex(p);
    }
    return m;
}

void Scene::computeWorlds(std::vector<Mat4>& out) const {
    const int n = (int)objects.size();
    out.assign(n, Mat4());
    std::vector<char> state(n, 0);  // 0 = todo, 1 = in progress, 2 = done
    std::vector<int> stack;
    for (int start = 0; start < n; ++start) {
        if (state[start] == 2) continue;
        stack.push_back(start);
        while (!stack.empty()) {
            int i = stack.back();
            int p = parentIndex(i);
            if (p >= 0 && state[p] == 0) {  // parent first
                state[i] = 1;
                stack.push_back(p);
                continue;
            }
            Mat4 local = objects[i].matrix();
            out[i] = (p >= 0 && state[p] == 2) ? out[p] * local : local;
            state[i] = 2;
            stack.pop_back();
        }
    }
}

Mat4 Scene::parentWorld(int i) const {
    int p = parentIndex(i);
    return p >= 0 ? world(p) : Mat4();
}

bool Scene::isAncestor(int ancestor, int i) const {
    int p = parentIndex(i);
    for (int guard = 0; p >= 0 && guard < 256; ++guard) {
        if (p == ancestor) return true;
        p = parentIndex(p);
    }
    return false;
}

int Scene::depth(int i) const {
    int d = 0;
    for (int p = parentIndex(i); p >= 0 && d < 64; p = parentIndex(p)) ++d;
    return d;
}

bool Scene::setParent(int child, int parent, bool keepWorld) {
    if (child == parent) return false;
    if (parent >= 0 && isAncestor(child, parent)) return false;
    Mat4 w = world(child);
    objects[child].parent = parent >= 0 ? objects[parent].id : 0;
    if (keepWorld) {
        Mat4 local = inverse(parentWorld(child)) * w;
        decomposeTRS(local, objects[child].position, objects[child].rotation, objects[child].scale);
    }
    return true;
}

std::vector<int> Scene::descendants(int i) const {
    std::vector<int> out;
    for (int j = 0; j < (int)objects.size(); ++j)
        if (isAncestor(i, j)) out.push_back(j);
    return out;
}

int pickObject(const Scene& scene, Vec3 origin, Vec3 dir, float* tOut) {
    int best = -1;
    float bestT = 1e30f;
    std::vector<Vec3> scratch;
    std::vector<Mat4> worlds;
    scene.computeWorlds(worlds);
    for (int i = 0; i < (int)scene.objects.size(); ++i) {
        const Object& o = scene.objects[i];
        if (!o.isMesh()) continue;
        // Cheap rejection first: ray vs the mesh's local bounding box (posed
        // skinned meshes skip it - their pose can leave the rest bounds).
        Vec3 lo, hi;
        if (!isSkinned(o)) {
            if (!o.mesh.bounds(lo, hi)) continue;
            Mat4 inv = inverse(worlds[i]);
            Vec3 lo2 = transformPoint(inv, origin), ld = transformDir(inv, dir);
            if (!rayHitsBox(lo2, ld, lo, hi, bestT)) continue;
            float t;
            if (raycastMesh(o.mesh, o.mesh.verts, lo2, ld, t) && t < bestT) {
                bestT = t;
                best = i;
            }
            continue;
        }
        Mat4 model;
        const std::vector<Vec3>& pos = evaluateMesh(scene, i, false, scratch, model);
        // Transform the ray into the mesh's space instead of the mesh into
        // world space (the "instancing" trick from FoCG). The ray parameter t
        // is preserved by the affine map, so hits stay comparable.
        Mat4 inv = inverse(model);
        float t;
        if (raycastMesh(o.mesh, pos, transformPoint(inv, origin), transformDir(inv, dir), t) && t < bestT) {
            bestT = t;
            best = i;
        }
    }
    if (tOut) *tOut = bestT;
    return best;
}

// ---------------------------------------------------------------------------
// Fast text I/O helpers. The original version used fprintf per number and an
// istringstream per line; on a 131k-quad mesh that took 1.3 s to save and
// 4.5 s to load (see docs/performance.md). Now: one growing buffer, numbers
// via std::to_chars / std::from_chars (locale-free, shortest round-trip
// floats), one fwrite / one fread.
// ---------------------------------------------------------------------------
namespace {

struct TextOut {
    std::string buf;
    void raw(const char* s) { buf += s; }
    void str(const std::string& s) { buf += s; }
    void ch(char c) { buf.push_back(c); }
    void num(float v) {
        char tmp[32];
        auto r = std::to_chars(tmp, tmp + sizeof tmp, v);
        buf.append(tmp, r.ptr);
    }
    void num(int v) {
        char tmp[16];
        auto r = std::to_chars(tmp, tmp + sizeof tmp, v);
        buf.append(tmp, r.ptr);
    }
    void nums(std::initializer_list<float> vs) {
        for (float v : vs) {
            ch(' ');
            num(v);
        }
    }
    bool writeTo(const std::string& path) const {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        bool ok = std::fwrite(buf.data(), 1, buf.size(), f) == buf.size();
        ok = (std::fclose(f) == 0) && ok;
        return ok;
    }
};

bool readFile(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(size > 0 ? (size_t)size : 0);
    size_t got = out.empty() ? 0 : std::fread(&out[0], 1, out.size(), f);
    std::fclose(f);
    out.resize(got);
    return true;
}

// Tokenizer over one line.
struct Cursor {
    const char* p;
    const char* end;
    void skip() {
        while (p < end && (*p == ' ' || *p == '\t')) ++p;
    }
    bool atEnd() {
        skip();
        return p >= end;
    }
    std::string_view word() {
        skip();
        const char* s = p;
        while (p < end && *p != ' ' && *p != '\t') ++p;
        return {s, size_t(p - s)};
    }
    bool f(float& v) {
        skip();
        if (p < end && *p == '+') ++p;
        auto r = std::from_chars(p, end, v);
        if (r.ec != std::errc()) return false;
        p = r.ptr;
        return true;
    }
    bool i(int& v) {
        skip();
        if (p < end && *p == '+') ++p;
        auto r = std::from_chars(p, end, v);
        if (r.ec != std::errc()) return false;
        p = r.ptr;
        return true;
    }
    bool v3(Vec3& v) { return f(v.x) && f(v.y) && f(v.z); }
    std::string rest() {
        skip();
        const char* e = end;
        while (e > p && std::isspace((unsigned char)e[-1])) --e;
        return std::string(p, e);
    }
};

// Calls fn(Cursor&) for every non-empty, non-comment line.
template <class Fn>
void forEachLine(const std::string& text, Fn fn) {
    const char* p = text.data();
    const char* end = p + text.size();
    while (p < end) {
        const char* e = static_cast<const char*>(std::memchr(p, '\n', size_t(end - p)));
        if (!e) e = end;
        const char* le = e;
        if (le > p && le[-1] == '\r') --le;
        Cursor c{p, le};
        c.skip();
        if (c.p < c.end && *c.p != '#') {
            if (!fn(c)) return;
        }
        p = e + 1;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Native format
// ---------------------------------------------------------------------------
bool saveScene(const Scene& scene, const std::string& path, std::string& err) {
    std::unordered_map<uint32_t, int> fileIndex;
    for (int i = 0; i < (int)scene.objects.size(); ++i) fileIndex[scene.objects[i].id] = i;
    auto idx = [&](uint32_t id) {
        auto it = fileIndex.find(id);
        return it == fileIndex.end() ? -1 : it->second;
    };

    TextOut out;
    size_t estimate = 256;
    for (const Object& o : scene.objects) estimate += o.mesh.verts.size() * 40 + o.mesh.faces.size() * 60;
    out.buf.reserve(estimate);
    out.raw("# Modeler3D scene\nmodeler3d 2\nambient");
    out.nums({scene.ambient.x, scene.ambient.y, scene.ambient.z});
    out.ch('\n');
    for (const Object& o : scene.objects) {
        out.raw("object ");
        out.str(o.name);
        out.raw("\nkind ");
        out.raw(kKindNames[(int)o.kind]);
        out.raw("\nparent ");
        out.num(idx(o.parent));
        out.raw("\nposition");
        out.nums({o.position.x, o.position.y, o.position.z});
        out.raw("\nrotation");
        out.nums({o.rotation.x, o.rotation.y, o.rotation.z});
        out.raw("\nscale");
        out.nums({o.scale.x, o.scale.y, o.scale.z});
        out.raw("\ncolor");
        out.nums({o.color.x, o.color.y, o.color.z});
        out.raw("\nemission");
        out.nums({o.emission.x, o.emission.y, o.emission.z, o.emissionStrength});
        out.raw("\ngloss");
        out.nums({o.gloss});
        out.raw("\nsmooth ");
        out.num(o.smooth ? 1 : 0);
        const LightSettings& L = o.light;
        out.raw("\nlight ");
        out.num((int)L.type);
        out.nums({L.color.x, L.color.y, L.color.z, L.intensity, L.range, L.spotAngle, L.spotBlend});
        out.raw("\nbone");
        out.nums({o.boneLength});
        out.ch('\n');
        if (o.hasRest) {
            out.raw("rest");
            out.nums({o.restPosition.x, o.restPosition.y, o.restPosition.z, o.restRotation.x, o.restRotation.y,
                      o.restRotation.z, o.restScale.x, o.restScale.y, o.restScale.z});
            out.ch('\n');
        }
        const ParticleSettings& P = o.particles;
        out.raw("particles");
        out.nums({P.rate, P.lifetime, P.speed, P.spread, P.gravity, P.drag, P.startSize, P.endSize, P.radius,
                  P.startColor.x, P.startColor.y, P.startColor.z, P.endColor.x, P.endColor.y, P.endColor.z});
        out.raw(P.additive ? " 1\n" : " 0\n");
        if (o.param.active()) {
            out.raw("param ");
            out.num(o.param.shape);
            for (float v : o.param.p) out.nums({v});
            out.ch(' ');
            out.num(o.param.subdivisions);
            out.nums({o.param.twist, o.param.taper});
            out.ch('\n');
        }
        if (!o.skinBones.empty()) {
            out.raw("skin ");
            out.num((int)o.skinBones.size());
            for (uint32_t b : o.skinBones) {
                out.ch(' ');
                out.num(idx(b));
            }
            out.ch('\n');
            for (size_t s = 0; s < o.bindInverse.size(); ++s) {
                out.raw("bind ");
                out.num((int)s);
                for (float v : o.bindInverse[s].m) out.nums({v});
                out.ch('\n');
            }
        }
        for (const Vec3& v : o.mesh.verts) {
            out.ch('v');
            out.nums({v.x, v.y, v.z});
            out.ch('\n');
        }
        for (const auto& face : o.mesh.faces) {
            out.ch('f');
            for (int i : face) {
                out.ch(' ');
                out.num(i);
            }
            out.ch('\n');
        }
        if (o.mesh.hasUVs())
            for (const auto& uvs : o.mesh.uvs) {
                out.ch('t');
                for (const Vec2& t : uvs) out.nums({t.x, t.y});
                out.ch('\n');
            }
        if (o.mesh.hasWeights())
            for (size_t v = 0; v < o.mesh.weights.size(); ++v) {
                const BoneWeights& w = o.mesh.weights[v];
                if (w.total() <= 0) continue;
                out.raw("w ");
                out.num((int)v);
                for (int k = 0; k < 4; ++k) {
                    out.ch(' ');
                    out.num(w.bone[k]);
                    out.nums({w.w[k]});
                }
                out.ch('\n');
            }
        out.raw("end\n");
    }
    if (!out.writeTo(path)) {
        err = "Cannot write '" + path + "'";
        return false;
    }
    return true;
}

bool loadScene(Scene& scene, const std::string& path, std::string& err) {
    std::string text;
    if (!readFile(path, text)) {
        err = "Cannot open '" + path + "'";
        return false;
    }
    Scene result;
    result.nextId = scene.nextId;
    std::vector<int> parentIdx;             // per object: file index of the parent
    std::vector<std::vector<int>> skinIdx;  // per object: file indices of skin bones
    Object* cur = nullptr;
    bool header = false;
    int lineNo = 0;
    bool failed = false;
    auto fail = [&](const std::string& what) {
        err = path + ":" + std::to_string(lineNo) + ": " + what;
        failed = true;
        return false;
    };
    forEachLine(text, [&](Cursor& c) -> bool {
        ++lineNo;
        std::string_view key = c.word();
        if (key == "modeler3d") {
            header = true;
        } else if (!header) {
            return fail("not a Modeler3D scene file");
        } else if (key == "ambient") {
            c.v3(result.ambient);
        } else if (key == "object") {
            std::string name = c.rest();
            Object o;
            o.id = result.nextId++;
            o.name = name.empty() ? std::string("Object") : name;
            result.objects.push_back(std::move(o));
            parentIdx.push_back(-1);
            skinIdx.emplace_back();
            cur = &result.objects.back();
        } else if (!cur) {
            return fail("data outside of an object block");
        } else if (key == "v") {  // hot path first
            Vec3 v;
            if (!c.v3(v)) return fail("bad vertex");
            cur->mesh.verts.push_back(v);
        } else if (key == "f") {
            std::vector<int> face;
            int i;
            while (c.i(i)) {
                if (i < 0 || i >= (int)cur->mesh.verts.size()) return fail("face index out of range");
                face.push_back(i);
            }
            if (face.size() >= 3) cur->mesh.faces.push_back(std::move(face));
        } else if (key == "t") {
            std::vector<Vec2> uvs;
            Vec2 t;
            while (c.f(t.x) && c.f(t.y)) uvs.push_back(t);
            cur->mesh.uvs.push_back(std::move(uvs));
        } else if (key == "w") {
            int v = -1;
            c.i(v);
            if (v < 0 || v >= (int)cur->mesh.verts.size()) return fail("weight vertex out of range");
            cur->mesh.weights.resize(cur->mesh.verts.size());
            BoneWeights& w = cur->mesh.weights[v];
            for (int k = 0; k < 4; ++k) c.i(w.bone[k]) && c.f(w.w[k]);
        } else if (key == "kind") {
            std::string_view k = c.word();
            for (int i = 0; i < 5; ++i)
                if (k == kKindNames[i]) cur->kind = (ObjectKind)i;
        } else if (key == "parent") {
            c.i(parentIdx.back());
        } else if (key == "position" || key == "rotation" || key == "scale" || key == "color") {
            Vec3 v;
            if (!c.v3(v)) return fail("expected three numbers");
            if (key == "position") cur->position = v;
            else if (key == "rotation") cur->rotation = v;
            else if (key == "scale") cur->scale = v;
            else cur->color = v;
        } else if (key == "emission") {
            c.v3(cur->emission) && c.f(cur->emissionStrength);
        } else if (key == "gloss") {
            c.f(cur->gloss);
        } else if (key == "smooth") {
            int s = 1;
            c.i(s);
            cur->smooth = s != 0;
        } else if (key == "light") {
            int type = 0;
            LightSettings& L = cur->light;
            c.i(type) && c.v3(L.color) && c.f(L.intensity) && c.f(L.range) && c.f(L.spotAngle) && c.f(L.spotBlend);
            L.type = (LightType)std::max(0, std::min(2, type));
        } else if (key == "bone") {
            c.f(cur->boneLength);
        } else if (key == "rest") {
            Vec3 p, r, s;
            if (c.v3(p) && c.v3(r) && c.v3(s)) {
                cur->hasRest = true;
                cur->restPosition = p;
                cur->restRotation = r;
                cur->restScale = s;
            }
        } else if (key == "particles") {
            ParticleSettings& P = cur->particles;
            int additive = 1;
            c.f(P.rate) && c.f(P.lifetime) && c.f(P.speed) && c.f(P.spread) && c.f(P.gravity) && c.f(P.drag) &&
                c.f(P.startSize) && c.f(P.endSize) && c.f(P.radius) && c.v3(P.startColor) && c.v3(P.endColor) &&
                c.i(additive);
            P.additive = additive != 0;
        } else if (key == "param") {
            ParametricSpec& s = cur->param;
            bool ok = c.i(s.shape);
            for (float& v : s.p) ok = ok && c.f(v);
            ok = ok && c.i(s.subdivisions) && c.f(s.twist) && c.f(s.taper);
            if (!ok || !s.active()) s = ParametricSpec();
        } else if (key == "skin") {
            int n = 0;
            c.i(n);
            for (int i = 0; i < n; ++i) {
                int b = -1;
                c.i(b);
                skinIdx.back().push_back(b);
            }
        } else if (key == "bind") {
            int slot = -1;
            c.i(slot);
            if (slot < 0 || slot > 4096) return fail("bad bind slot");
            if ((int)cur->bindInverse.size() <= slot) cur->bindInverse.resize(slot + 1);
            for (float& v : cur->bindInverse[slot].m) c.f(v);
        } else if (key == "end") {
            cur = nullptr;
        }
        // Unknown keys are ignored for forward compatibility.
        return true;
    });
    if (failed) return false;
    if (!header) {
        err = "'" + path + "' is empty or not a Modeler3D scene";
        return false;
    }
    const int n = (int)result.objects.size();
    for (int i = 0; i < n; ++i) {
        Object& o = result.objects[i];
        o.mesh.validate();
        o.mesh.touch();
        int p = parentIdx[i];
        o.parent = (p >= 0 && p < n && p != i) ? result.objects[p].id : 0;
        for (int b : skinIdx[i])
            if (b >= 0 && b < n) o.skinBones.push_back(result.objects[b].id);
        if (o.bindInverse.size() != o.skinBones.size()) {
            o.skinBones.clear();
            o.bindInverse.clear();
        }
    }
    // Break any parent cycles a hand-edited file might contain.
    for (int i = 0; i < n; ++i)
        if (result.isAncestor(i, i)) result.objects[i].parent = 0;
    scene.objects = std::move(result.objects);
    scene.nextId = result.nextId;
    scene.ambient = result.ambient;
    scene.active = -1;
    return true;
}

// ---------------------------------------------------------------------------
// Wavefront OBJ
// ---------------------------------------------------------------------------
bool exportOBJ(const Scene& scene, const std::string& path, std::string& err, int* exportedCount) {
    std::string mtlPath = path;
    size_t dot = mtlPath.rfind('.');
    size_t slash = mtlPath.find_last_of("/\\");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) mtlPath.resize(dot);
    mtlPath += ".mtl";
    std::string mtlName = mtlPath.substr(slash == std::string::npos ? 0 : slash + 1);

    TextOut out, mtl;
    out.raw("# Exported by Modeler3D\nmtllib ");
    out.str(mtlName);
    out.ch('\n');
    mtl.raw("# Materials exported by Modeler3D\n");

    int base = 1, uvBase = 1, count = 0;
    std::vector<Vec3> scratch;
    for (int oi = 0; oi < (int)scene.objects.size(); ++oi) {
        const Object& o = scene.objects[oi];
        if (!o.isMesh() || o.mesh.faces.empty()) continue;
        std::string name = sanitize(o.name);
        Mat4 model;
        const std::vector<Vec3>& pos = evaluateMesh(scene, oi, false, scratch, model);
        out.buf.reserve(out.buf.size() + pos.size() * 40 + o.mesh.faces.size() * 50);
        out.raw("o ");
        out.str(name);
        out.ch('\n');
        for (const Vec3& v : pos) {
            Vec3 w = transformPoint(model, v);
            out.ch('v');
            out.nums({w.x, w.y, w.z});
            out.ch('\n');
        }
        const bool uvs = o.mesh.hasUVs();
        if (uvs)
            for (const auto& faceUVs : o.mesh.uvs)
                for (const Vec2& t : faceUVs) {
                    out.raw("vt");
                    out.nums({t.x, t.y});
                    out.ch('\n');
                }
        mtl.raw("\nnewmtl ");
        mtl.str(name);
        mtl.raw("_mat\nKa 0 0 0\nKd");
        mtl.nums({o.color.x, o.color.y, o.color.z});
        mtl.raw("\nKs 0.2 0.2 0.2\nNs");
        mtl.nums({8.0f + o.gloss * 120.0f});
        mtl.raw("\nd 1\nillum 2\n");
        if (o.emissionStrength > 0) {
            mtl.raw("Ke");
            mtl.nums({o.emission.x * o.emissionStrength, o.emission.y * o.emissionStrength,
                      o.emission.z * o.emissionStrength});
            mtl.ch('\n');
        }
        out.raw("usemtl ");
        out.str(name);
        out.raw(o.smooth ? "_mat\ns 1\n" : "_mat\ns off\n");
        int corner = uvBase;
        for (const auto& face : o.mesh.faces) {
            out.ch('f');
            for (int i : face) {
                out.ch(' ');
                out.num(i + base);
                if (uvs) {
                    out.ch('/');
                    out.num(corner++);
                }
            }
            out.ch('\n');
        }
        base += (int)pos.size();
        uvBase = corner;
        ++count;
    }
    if (!out.writeTo(path)) {
        err = "Cannot write '" + path + "'";
        return false;
    }
    mtl.writeTo(mtlPath);  // optional companion file
    if (exportedCount) *exportedCount = count;
    return true;
}

bool importOBJ(Scene& scene, const std::string& path, std::string& err, int* firstNewIndex) {
    std::string text;
    if (!readFile(path, text)) {
        err = "Cannot open '" + path + "'";
        return false;
    }
    struct Group {
        std::string name;
        std::vector<std::vector<int>> faces;    // global, 0-based
        std::vector<std::vector<int>> faceUVs;  // global vt indices, -1 if missing
    };
    std::vector<Vec3> verts;
    std::vector<Vec2> texcoords;
    std::vector<Group> groups(1);
    groups[0].name = baseName(path);

    auto resolve = [](int idx, int count) { return idx < 0 ? count + idx : idx - 1; };
    forEachLine(text, [&](Cursor& c) -> bool {
        std::string_view key = c.word();
        if (key == "v") {
            Vec3 v;
            if (c.v3(v)) verts.push_back(v);
        } else if (key == "vt") {
            Vec2 t;
            if (c.f(t.x) && c.f(t.y)) texcoords.push_back(t);
        } else if (key == "f") {
            std::vector<int> face, fuv;
            bool ok = true;
            while (!c.atEnd()) {  // "7", "7/2", "7//3", "7/2/3"
                std::string_view tok = c.word();
                int vi = 0;
                auto r = std::from_chars(tok.data(), tok.data() + tok.size(), vi);
                int idx = resolve(vi, (int)verts.size());
                if (r.ec != std::errc() || idx < 0 || idx >= (int)verts.size()) {
                    ok = false;
                    break;
                }
                face.push_back(idx);
                int t = -1;
                const char* s = r.ptr;
                const char* e = tok.data() + tok.size();
                if (s < e && *s == '/' && s + 1 < e && s[1] != '/') {
                    int ti = 0;
                    if (std::from_chars(s + 1, e, ti).ec == std::errc()) {
                        t = resolve(ti, (int)texcoords.size());
                        if (t < 0 || t >= (int)texcoords.size()) t = -1;
                    }
                }
                fuv.push_back(t);
            }
            if (ok && face.size() >= 3) {
                groups.back().faces.push_back(std::move(face));
                groups.back().faceUVs.push_back(std::move(fuv));
            }
        } else if (key == "o" || key == "g") {
            std::string name = c.rest();
            if (!name.empty()) {
                if (groups.back().faces.empty()) groups.back().name = name;
                else groups.push_back({name, {}, {}});
            }
        }
        return true;
    });

    int first = -1, added = 0;
    for (auto& g : groups) {
        if (g.faces.empty()) continue;
        Mesh mesh;
        std::unordered_map<int, int> local;
        local.reserve(g.faces.size() * 2);
        bool allUV = true;
        for (size_t fi = 0; fi < g.faces.size(); ++fi) {
            auto& face = g.faces[fi];
            std::vector<Vec2> uvs;
            for (size_t k = 0; k < face.size(); ++k) {
                int& idx = face[k];
                auto it = local.find(idx);
                if (it == local.end()) {
                    it = local.emplace(idx, (int)mesh.verts.size()).first;
                    mesh.verts.push_back(verts[idx]);
                }
                idx = it->second;
                int t = g.faceUVs[fi][k];
                if (t < 0) allUV = false;
                else uvs.push_back(texcoords[t]);
            }
            mesh.faces.push_back(std::move(face));
            mesh.uvs.push_back(std::move(uvs));
        }
        if (!allUV) mesh.uvs.clear();
        mesh.validate();
        mesh.touch();
        int i = scene.add(std::move(mesh), g.name, {0.8f, 0.8f, 0.8f});
        if (first < 0) first = i;
        ++added;
    }
    if (added == 0) {
        err = "No faces found in '" + path + "'";
        return false;
    }
    if (firstNewIndex) *firstNewIndex = first;
    return true;
}
