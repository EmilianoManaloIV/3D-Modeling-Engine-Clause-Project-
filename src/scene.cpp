#include "scene.h"

#include "skin.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace {
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

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

int Scene::indexOf(uint32_t id) const {
    if (id == 0) return -1;
    for (int i = 0; i < (int)objects.size(); ++i)
        if (objects[i].id == id) return i;
    return -1;
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
    for (int i = 0; i < (int)scene.objects.size(); ++i) {
        if (!scene.objects[i].isMesh()) continue;
        Mat4 model;
        const std::vector<Vec3>& pos = evaluateMesh(scene, i, false, scratch, model);
        // Transform the ray into the mesh's space instead of the mesh into
        // world space (the "instancing" trick from FoCG). The ray parameter t
        // is preserved by the affine map, so hits stay comparable.
        Mat4 inv = inverse(model);
        float t;
        if (raycastMesh(scene.objects[i].mesh, pos, transformPoint(inv, origin), transformDir(inv, dir), t) &&
            t < bestT) {
            bestT = t;
            best = i;
        }
    }
    if (tOut) *tOut = bestT;
    return best;
}

// ---------------------------------------------------------------------------
// Native format
// ---------------------------------------------------------------------------
bool saveScene(const Scene& scene, const std::string& path, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        err = "Cannot write '" + path + "'";
        return false;
    }
    std::unordered_map<uint32_t, int> fileIndex;
    for (int i = 0; i < (int)scene.objects.size(); ++i) fileIndex[scene.objects[i].id] = i;
    auto idx = [&](uint32_t id) {
        auto it = fileIndex.find(id);
        return it == fileIndex.end() ? -1 : it->second;
    };

    std::fprintf(f, "# Modeler3D scene\nmodeler3d 2\n");
    std::fprintf(f, "ambient %.4g %.4g %.4g\n", scene.ambient.x, scene.ambient.y, scene.ambient.z);
    for (const Object& o : scene.objects) {
        std::fprintf(f, "object %s\n", o.name.c_str());
        std::fprintf(f, "kind %s\n", kKindNames[(int)o.kind]);
        std::fprintf(f, "parent %d\n", idx(o.parent));
        std::fprintf(f, "position %.7g %.7g %.7g\n", o.position.x, o.position.y, o.position.z);
        std::fprintf(f, "rotation %.7g %.7g %.7g\n", o.rotation.x, o.rotation.y, o.rotation.z);
        std::fprintf(f, "scale %.7g %.7g %.7g\n", o.scale.x, o.scale.y, o.scale.z);
        std::fprintf(f, "color %.4g %.4g %.4g\n", o.color.x, o.color.y, o.color.z);
        std::fprintf(f, "emission %.4g %.4g %.4g %.4g\n", o.emission.x, o.emission.y, o.emission.z,
                     o.emissionStrength);
        std::fprintf(f, "gloss %.4g\n", o.gloss);
        std::fprintf(f, "smooth %d\n", o.smooth ? 1 : 0);
        const LightSettings& L = o.light;
        std::fprintf(f, "light %d %.4g %.4g %.4g %.6g %.6g %.6g %.6g\n", (int)L.type, L.color.x, L.color.y,
                     L.color.z, L.intensity, L.range, L.spotAngle, L.spotBlend);
        std::fprintf(f, "bone %.7g\n", o.boneLength);
        if (o.hasRest)
            std::fprintf(f, "rest %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g\n", o.restPosition.x,
                         o.restPosition.y, o.restPosition.z, o.restRotation.x, o.restRotation.y, o.restRotation.z,
                         o.restScale.x, o.restScale.y, o.restScale.z);
        const ParticleSettings& P = o.particles;
        std::fprintf(f, "particles %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %d\n", P.rate, P.lifetime, P.speed,
                     P.spread, P.gravity, P.drag, P.startSize, P.endSize, P.radius, P.startColor.x, P.startColor.y,
                     P.startColor.z, P.endColor.x, P.endColor.y, P.endColor.z, P.additive ? 1 : 0);
        if (o.param.active()) {
            std::fprintf(f, "param %d", o.param.shape);
            for (float v : o.param.p) std::fprintf(f, " %.7g", v);
            std::fprintf(f, " %d %.7g %.7g\n", o.param.subdivisions, o.param.twist, o.param.taper);
        }
        if (!o.skinBones.empty()) {
            std::fprintf(f, "skin %d", (int)o.skinBones.size());
            for (uint32_t b : o.skinBones) std::fprintf(f, " %d", idx(b));
            std::fputc('\n', f);
            for (size_t s = 0; s < o.bindInverse.size(); ++s) {
                std::fprintf(f, "bind %d", (int)s);
                for (float v : o.bindInverse[s].m) std::fprintf(f, " %.9g", v);
                std::fputc('\n', f);
            }
        }
        for (const Vec3& v : o.mesh.verts) std::fprintf(f, "v %.7g %.7g %.7g\n", v.x, v.y, v.z);
        for (const auto& face : o.mesh.faces) {
            std::fputc('f', f);
            for (int i : face) std::fprintf(f, " %d", i);
            std::fputc('\n', f);
        }
        if (o.mesh.hasUVs())
            for (const auto& uvs : o.mesh.uvs) {
                std::fputc('t', f);
                for (const Vec2& t : uvs) std::fprintf(f, " %.6g %.6g", t.x, t.y);
                std::fputc('\n', f);
            }
        if (o.mesh.hasWeights())
            for (size_t v = 0; v < o.mesh.weights.size(); ++v) {
                const BoneWeights& w = o.mesh.weights[v];
                if (w.total() <= 0) continue;
                std::fprintf(f, "w %d", (int)v);
                for (int k = 0; k < 4; ++k) std::fprintf(f, " %d %.5g", w.bone[k], w.w[k]);
                std::fputc('\n', f);
            }
        std::fprintf(f, "end\n");
    }
    bool ok = !std::ferror(f);
    std::fclose(f);
    if (!ok) err = "Error while writing '" + path + "'";
    return ok;
}

bool loadScene(Scene& scene, const std::string& path, std::string& err) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        err = "Cannot open '" + path + "'";
        return false;
    }
    Scene result;
    result.nextId = scene.nextId;
    std::vector<int> parentIdx;                 // per object: file index of the parent
    std::vector<std::vector<int>> skinIdx;      // per object: file indices of skin bones
    Object* cur = nullptr;
    bool header = false;
    std::string line;
    int lineNo = 0;
    auto fail = [&](const std::string& what) {
        err = path + ":" + std::to_string(lineNo) + ": " + what;
        return false;
    };
    while (std::getline(file, line)) {
        ++lineNo;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string key;
        ss >> key;
        if (key == "modeler3d") {
            header = true;
        } else if (!header) {
            return fail("not a Modeler3D scene file");
        } else if (key == "ambient") {
            ss >> result.ambient.x >> result.ambient.y >> result.ambient.z;
        } else if (key == "object") {
            std::string name = trim(line.substr(6));
            Object o;
            o.id = result.nextId++;
            o.name = name.empty() ? std::string("Object") : name;
            result.objects.push_back(std::move(o));
            parentIdx.push_back(-1);
            skinIdx.emplace_back();
            cur = &result.objects.back();
        } else if (!cur) {
            return fail("data outside of an object block");
        } else if (key == "kind") {
            std::string k;
            ss >> k;
            for (int i = 0; i < 5; ++i)
                if (k == kKindNames[i]) cur->kind = (ObjectKind)i;
        } else if (key == "parent") {
            ss >> parentIdx.back();
        } else if (key == "position" || key == "rotation" || key == "scale" || key == "color") {
            Vec3 v;
            if (!(ss >> v.x >> v.y >> v.z)) return fail("expected three numbers");
            if (key == "position") cur->position = v;
            else if (key == "rotation") cur->rotation = v;
            else if (key == "scale") cur->scale = v;
            else cur->color = v;
        } else if (key == "emission") {
            ss >> cur->emission.x >> cur->emission.y >> cur->emission.z >> cur->emissionStrength;
        } else if (key == "gloss") {
            ss >> cur->gloss;
        } else if (key == "smooth") {
            int s = 1;
            ss >> s;
            cur->smooth = s != 0;
        } else if (key == "light") {
            int type = 0;
            LightSettings& L = cur->light;
            ss >> type >> L.color.x >> L.color.y >> L.color.z >> L.intensity >> L.range >> L.spotAngle >> L.spotBlend;
            L.type = (LightType)std::max(0, std::min(2, type));
        } else if (key == "bone") {
            ss >> cur->boneLength;
        } else if (key == "rest") {
            Vec3 p, r, s;
            if (ss >> p.x >> p.y >> p.z >> r.x >> r.y >> r.z >> s.x >> s.y >> s.z) {
                cur->hasRest = true;
                cur->restPosition = p;
                cur->restRotation = r;
                cur->restScale = s;
            }
        } else if (key == "particles") {
            ParticleSettings& P = cur->particles;
            int additive = 1;
            ss >> P.rate >> P.lifetime >> P.speed >> P.spread >> P.gravity >> P.drag >> P.startSize >> P.endSize >>
                P.radius >> P.startColor.x >> P.startColor.y >> P.startColor.z >> P.endColor.x >> P.endColor.y >>
                P.endColor.z >> additive;
            P.additive = additive != 0;
        } else if (key == "param") {
            ParametricSpec& s = cur->param;
            ss >> s.shape;
            for (float& v : s.p) ss >> v;
            ss >> s.subdivisions >> s.twist >> s.taper;
            if (!ss || !s.active()) s = ParametricSpec();
        } else if (key == "skin") {
            int n = 0;
            ss >> n;
            for (int i = 0; i < n; ++i) {
                int b = -1;
                ss >> b;
                skinIdx.back().push_back(b);
            }
        } else if (key == "bind") {
            int slot = -1;
            ss >> slot;
            if (slot < 0 || slot > 4096) return fail("bad bind slot");
            if ((int)cur->bindInverse.size() <= slot) cur->bindInverse.resize(slot + 1);
            for (float& v : cur->bindInverse[slot].m) ss >> v;
        } else if (key == "v") {
            Vec3 v;
            if (!(ss >> v.x >> v.y >> v.z)) return fail("bad vertex");
            cur->mesh.verts.push_back(v);
        } else if (key == "f") {
            std::vector<int> face;
            int i;
            while (ss >> i) {
                if (i < 0 || i >= (int)cur->mesh.verts.size()) return fail("face index out of range");
                face.push_back(i);
            }
            if (face.size() >= 3) cur->mesh.faces.push_back(std::move(face));
        } else if (key == "t") {
            std::vector<Vec2> uvs;
            Vec2 t;
            while (ss >> t.x >> t.y) uvs.push_back(t);
            cur->mesh.uvs.push_back(std::move(uvs));
        } else if (key == "w") {
            int v = -1;
            ss >> v;
            if (v < 0 || v >= (int)cur->mesh.verts.size()) return fail("weight vertex out of range");
            cur->mesh.weights.resize(cur->mesh.verts.size());
            BoneWeights& w = cur->mesh.weights[v];
            for (int k = 0; k < 4; ++k) ss >> w.bone[k] >> w.w[k];
        } else if (key == "end") {
            cur->mesh.validate();
            cur->mesh.touch();
            cur = nullptr;
        }
        // Unknown keys are ignored for forward compatibility.
    }
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

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        err = "Cannot write '" + path + "'";
        return false;
    }
    FILE* mtl = std::fopen(mtlPath.c_str(), "wb");
    std::fprintf(f, "# Exported by Modeler3D\n");
    if (mtl) {
        std::fprintf(f, "mtllib %s\n", mtlName.c_str());
        std::fprintf(mtl, "# Materials exported by Modeler3D\n");
    }

    int base = 1, uvBase = 1, count = 0;
    std::vector<Vec3> scratch;
    for (int oi = 0; oi < (int)scene.objects.size(); ++oi) {
        const Object& o = scene.objects[oi];
        if (!o.isMesh() || o.mesh.faces.empty()) continue;
        std::string name = sanitize(o.name);
        Mat4 model;
        const std::vector<Vec3>& pos = evaluateMesh(scene, oi, false, scratch, model);
        std::fprintf(f, "o %s\n", name.c_str());
        for (const Vec3& v : pos) {
            Vec3 w = transformPoint(model, v);
            std::fprintf(f, "v %.6f %.6f %.6f\n", w.x, w.y, w.z);
        }
        const bool uvs = o.mesh.hasUVs();
        if (uvs)
            for (const auto& faceUVs : o.mesh.uvs)
                for (const Vec2& t : faceUVs) std::fprintf(f, "vt %.6f %.6f\n", t.x, t.y);
        if (mtl) {
            std::fprintf(mtl, "\nnewmtl %s_mat\nKa 0 0 0\nKd %.4f %.4f %.4f\nKs 0.2 0.2 0.2\nNs %.1f\nd 1\nillum 2\n",
                         name.c_str(), o.color.x, o.color.y, o.color.z, 8.0f + o.gloss * 120.0f);
            if (o.emissionStrength > 0)
                std::fprintf(mtl, "Ke %.4f %.4f %.4f\n", o.emission.x * o.emissionStrength,
                             o.emission.y * o.emissionStrength, o.emission.z * o.emissionStrength);
            std::fprintf(f, "usemtl %s_mat\n", name.c_str());
        }
        std::fprintf(f, "s %s\n", o.smooth ? "1" : "off");
        int corner = uvBase;
        for (const auto& face : o.mesh.faces) {
            std::fputc('f', f);
            for (int i : face) {
                if (uvs) std::fprintf(f, " %d/%d", i + base, corner++);
                else std::fprintf(f, " %d", i + base);
            }
            std::fputc('\n', f);
        }
        base += (int)pos.size();
        uvBase = corner;
        ++count;
    }
    bool ok = !std::ferror(f);
    std::fclose(f);
    if (mtl) std::fclose(mtl);
    if (!ok) err = "Error while writing '" + path + "'";
    if (exportedCount) *exportedCount = count;
    return ok;
}

bool importOBJ(Scene& scene, const std::string& path, std::string& err, int* firstNewIndex) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        err = "Cannot open '" + path + "'";
        return false;
    }
    struct Group {
        std::string name;
        std::vector<std::vector<int>> faces;     // global, 0-based
        std::vector<std::vector<int>> faceUVs;   // global vt indices, -1 if missing
    };
    std::vector<Vec3> verts;
    std::vector<Vec2> texcoords;
    std::vector<Group> groups(1);
    groups[0].name = baseName(path);

    auto resolve = [](int idx, int count) { return idx < 0 ? count + idx : idx - 1; };
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string key;
        ss >> key;
        if (key == "v") {
            Vec3 v;
            if (ss >> v.x >> v.y >> v.z) verts.push_back(v);
        } else if (key == "vt") {
            Vec2 t;
            if (ss >> t.x >> t.y) texcoords.push_back(t);
        } else if (key == "f") {
            std::vector<int> face, fuv;
            std::string tok;
            bool ok = true;
            while (ss >> tok) {  // "7", "7/2", "7//3", "7/2/3"
                int idx = resolve(std::atoi(tok.c_str()), (int)verts.size());
                if (idx < 0 || idx >= (int)verts.size()) { ok = false; break; }
                face.push_back(idx);
                int t = -1;
                size_t slashPos = tok.find('/');
                if (slashPos != std::string::npos && slashPos + 1 < tok.size() && tok[slashPos + 1] != '/') {
                    t = resolve(std::atoi(tok.c_str() + slashPos + 1), (int)texcoords.size());
                    if (t < 0 || t >= (int)texcoords.size()) t = -1;
                }
                fuv.push_back(t);
            }
            if (ok && face.size() >= 3) {
                groups.back().faces.push_back(std::move(face));
                groups.back().faceUVs.push_back(std::move(fuv));
            }
        } else if (key == "o" || key == "g") {
            std::string name = trim(line.substr(1));
            if (name.empty()) continue;
            if (groups.back().faces.empty()) groups.back().name = name;
            else groups.push_back({name, {}, {}});
        }
    }

    int first = -1, added = 0;
    for (auto& g : groups) {
        if (g.faces.empty()) continue;
        Mesh mesh;
        std::unordered_map<int, int> local;
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
