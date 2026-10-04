#include "scene.h"

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
}  // namespace

int Scene::add(Mesh mesh, const std::string& base, Vec3 color) {
    Object o;
    o.id = nextId++;
    o.name = uniqueName(base);
    o.mesh = std::move(mesh);
    if (o.mesh.version == 0) o.mesh.touch();
    o.color = color;
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
    for (const auto& o : objects) n += o.mesh.triangleCount();
    return n;
}

int pickObject(const Scene& scene, Vec3 origin, Vec3 dir, float* tOut) {
    int best = -1;
    float bestT = 1e30f;
    for (int i = 0; i < (int)scene.objects.size(); ++i) {
        const Object& o = scene.objects[i];
        // Transform the ray into object space instead of the mesh into world
        // space (the "instancing" trick from FoCG). The ray parameter t is
        // preserved by the affine map, so hits stay comparable.
        Mat4 inv = inverse(o.matrix());
        float t;
        if (raycastMesh(o.mesh, transformPoint(inv, origin), transformDir(inv, dir), t) && t < bestT) {
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
    std::fprintf(f, "# Modeler3D scene\nmodeler3d 1\n");
    for (const Object& o : scene.objects) {
        std::fprintf(f, "object %s\n", o.name.c_str());
        std::fprintf(f, "position %.6g %.6g %.6g\n", o.position.x, o.position.y, o.position.z);
        std::fprintf(f, "rotation %.6g %.6g %.6g\n", o.rotation.x, o.rotation.y, o.rotation.z);
        std::fprintf(f, "scale %.6g %.6g %.6g\n", o.scale.x, o.scale.y, o.scale.z);
        std::fprintf(f, "color %.4g %.4g %.4g\n", o.color.x, o.color.y, o.color.z);
        std::fprintf(f, "smooth %d\n", o.smooth ? 1 : 0);
        for (const Vec3& v : o.mesh.verts) std::fprintf(f, "v %.7g %.7g %.7g\n", v.x, v.y, v.z);
        for (const auto& face : o.mesh.faces) {
            std::fputc('f', f);
            for (int i : face) std::fprintf(f, " %d", i);
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
        } else if (key == "object") {
            std::string name = trim(line.substr(6));
            Object o;
            o.id = result.nextId++;
            o.name = name.empty() ? std::string("Object") : name;
            result.objects.push_back(std::move(o));
            cur = &result.objects.back();
        } else if (!cur) {
            return fail("data outside of an object block");
        } else if (key == "position" || key == "rotation" || key == "scale" || key == "color") {
            Vec3 v;
            if (!(ss >> v.x >> v.y >> v.z)) return fail("expected three numbers");
            if (key == "position") cur->position = v;
            else if (key == "rotation") cur->rotation = v;
            else if (key == "scale") cur->scale = v;
            else cur->color = v;
        } else if (key == "smooth") {
            int s = 1;
            ss >> s;
            cur->smooth = s != 0;
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
        } else if (key == "end") {
            cur->mesh.touch();
            cur = nullptr;
        }
        // Unknown keys are ignored for forward compatibility.
    }
    if (!header) {
        err = "'" + path + "' is empty or not a Modeler3D scene";
        return false;
    }
    for (auto& o : result.objects) o.mesh.touch();
    scene.objects = std::move(result.objects);
    scene.nextId = result.nextId;
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

    int base = 1, count = 0;
    for (const Object& o : scene.objects) {
        if (o.mesh.faces.empty()) continue;
        std::string name = sanitize(o.name);
        Mat4 m = o.matrix();
        std::fprintf(f, "o %s\n", name.c_str());
        for (const Vec3& v : o.mesh.verts) {
            Vec3 w = transformPoint(m, v);
            std::fprintf(f, "v %.6f %.6f %.6f\n", w.x, w.y, w.z);
        }
        if (mtl) {
            std::fprintf(mtl, "\nnewmtl %s_mat\nKa 0 0 0\nKd %.4f %.4f %.4f\nKs 0.2 0.2 0.2\nNs 40\nd 1\nillum 2\n",
                         name.c_str(), o.color.x, o.color.y, o.color.z);
            std::fprintf(f, "usemtl %s_mat\n", name.c_str());
        }
        std::fprintf(f, "s %s\n", o.smooth ? "1" : "off");
        for (const auto& face : o.mesh.faces) {
            std::fputc('f', f);
            for (int i : face) std::fprintf(f, " %d", i + base);
            std::fputc('\n', f);
        }
        base += (int)o.mesh.verts.size();
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
        std::vector<std::vector<int>> faces;  // global, 0-based
    };
    std::vector<Vec3> verts;
    std::vector<Group> groups(1);
    groups[0].name = baseName(path);

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
        } else if (key == "f") {
            std::vector<int> face;
            std::string tok;
            bool ok = true;
            while (ss >> tok) {
                int idx = std::atoi(tok.c_str());  // "7", "7/2", "7//3", "7/2/3"
                if (idx < 0) idx = (int)verts.size() + idx;
                else idx -= 1;
                if (idx < 0 || idx >= (int)verts.size()) { ok = false; break; }
                face.push_back(idx);
            }
            if (ok && face.size() >= 3) groups.back().faces.push_back(std::move(face));
        } else if (key == "o" || key == "g") {
            std::string name = trim(line.substr(1));
            if (name.empty()) continue;
            if (groups.back().faces.empty()) groups.back().name = name;
            else groups.push_back({name, {}});
        }
    }

    int first = -1, added = 0;
    for (auto& g : groups) {
        if (g.faces.empty()) continue;
        Mesh mesh;
        std::unordered_map<int, int> local;
        for (auto& face : g.faces) {
            for (int& idx : face) {
                auto it = local.find(idx);
                if (it == local.end()) {
                    it = local.emplace(idx, (int)mesh.verts.size()).first;
                    mesh.verts.push_back(verts[idx]);
                }
                idx = it->second;
            }
            mesh.faces.push_back(std::move(face));
        }
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
