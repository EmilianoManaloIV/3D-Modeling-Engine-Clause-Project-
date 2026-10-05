#include "skin.h"

#include "jobs.h"
#include "profiler.h"

#include <cmath>
#include <cstring>

bool isSkinned(const Object& o) {
    return o.isMesh() && !o.skinBones.empty() && o.bindInverse.size() == o.skinBones.size() && o.mesh.hasWeights();
}

void skinMatrices(const Scene& s, const Object& o, std::vector<Mat4>& out) {
    out.resize(o.skinBones.size());
    for (size_t k = 0; k < o.skinBones.size(); ++k) {
        int b = s.indexOf(o.skinBones[k]);
        // A deleted bone simply stops influencing the mesh.
        out[k] = b >= 0 ? s.world(b) * o.bindInverse[k] : Mat4();
    }
}

const std::vector<Vec3>& evaluateMesh(const Scene& s, int i, bool restPose, std::vector<Vec3>& scratch, Mat4& model) {
    const Object& o = s.objects[i];
    model = s.world(i);
    if (restPose || !isSkinned(o)) return o.mesh.verts;

    PROF_SCOPE("skinning (CPU)");
    std::vector<Mat4> palette;
    skinMatrices(s, o, palette);
    const Mat4 W = model;
    model = Mat4();
    scratch.resize(o.mesh.verts.size());
    // Vertices are independent: spread them over the job system's threads.
    jobs::parallelFor(0, (int)o.mesh.verts.size(), 4096, [&](int vb, int ve) {
    for (int v = vb; v < ve; ++v) {
        Vec3 p = transformPoint(W, o.mesh.verts[v]);
        const BoneWeights& bw = o.mesh.weights[v];
        Vec3 acc;
        float total = 0;
        for (int k = 0; k < 4; ++k) {
            int b = bw.bone[k];
            if (b < 0 || b >= (int)palette.size() || bw.w[k] <= 0) continue;
            acc += transformPoint(palette[b], p) * bw.w[k];
            total += bw.w[k];
        }
        // Under-weighted vertices keep the remainder in place; over-weighted
        // ones are normalized.
        if (total > 1.0f) scratch[v] = acc / total;
        else scratch[v] = acc + p * (1.0f - total);
    }
    });
    return scratch;
}

uint64_t poseHash(const Scene& s, int i) {
    const Object& o = s.objects[i];
    if (!isSkinned(o)) return 0;
    std::vector<Mat4> palette;
    skinMatrices(s, o, palette);
    uint64_t h = 1469598103934665603ull;
    for (const Mat4& m : palette)
        for (float f : m.m) {
            uint32_t bits;
            std::memcpy(&bits, &f, 4);
            h = (h ^ bits) * 1099511628211ull;
        }
    // Moving the mesh object itself also changes the posed result.
    Mat4 w = s.world(i);
    for (float f : w.m) {
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        h = (h ^ bits) * 1099511628211ull;
    }
    return h;
}

Vec3 boneHead(const Scene& s, int i) { return transformPoint(s.world(i), Vec3()); }
Vec3 boneTail(const Scene& s, int i) { return transformPoint(s.world(i), Vec3(0, s.objects[i].boneLength, 0)); }

namespace {
float distanceToSegment(Vec3 p, Vec3 a, Vec3 b) {
    Vec3 ab = b - a;
    float len2 = dot(ab, ab);
    float t = len2 > 1e-12f ? clampf(dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    return length(p - (a + ab * t));
}
}  // namespace

void autoWeights(Scene& s, int meshIndex) {
    Object& o = s.objects[meshIndex];
    const Mat4 W = s.world(meshIndex);
    std::vector<Vec3> heads, tails;
    float avgLen = 0;
    for (uint32_t id : o.skinBones) {
        int b = s.indexOf(id);
        heads.push_back(b >= 0 ? boneHead(s, b) : Vec3());
        tails.push_back(b >= 0 ? boneTail(s, b) : Vec3());
        avgLen += length(tails.back() - heads.back());
    }
    avgLen = std::max(1e-3f, avgLen / std::max<size_t>(1, heads.size()));
    const float eps = 0.05f * avgLen;
    o.mesh.weights.assign(o.mesh.verts.size(), BoneWeights());
    jobs::parallelFor(0, (int)o.mesh.verts.size(), 1024, [&](int vb, int ve) {
    for (int v = vb; v < ve; ++v) {
        Vec3 p = transformPoint(W, o.mesh.verts[v]);
        BoneWeights bw;
        for (size_t k = 0; k < heads.size(); ++k) {
            float d = distanceToSegment(p, heads[k], tails[k]) + eps;
            bw.add((int)k, 1.0f / (d * d * d * d));
        }
        bw.normalize();
        for (int k = 0; k < 4; ++k)
            if (bw.bone[k] >= 0 && bw.w[k] < 0.02f) bw.remove(bw.bone[k]);
        bw.normalize();
        o.mesh.weights[v] = bw;
    }
    });
    o.mesh.touch();
}

bool bindSkin(Scene& s, int meshIndex, const std::vector<int>& bones, std::string& error) {
    Object& o = s.objects[meshIndex];
    if (!o.isMesh()) {
        error = o.name + " is not a mesh";
        return false;
    }
    if (bones.empty()) {
        error = "No bones to bind to";
        return false;
    }
    o.skinBones.clear();
    o.bindInverse.clear();
    for (int b : bones) {
        Object& bone = s.objects[b];
        o.skinBones.push_back(bone.id);
        o.bindInverse.push_back(inverse(s.world(b)));
        // The bind pose becomes the bone's rest pose ("Reset Pose" target).
        bone.hasRest = true;
        bone.restPosition = bone.position;
        bone.restRotation = bone.rotation;
        bone.restScale = bone.scale;
    }
    autoWeights(s, meshIndex);
    return true;
}

void unbindSkin(Object& o) {
    o.skinBones.clear();
    o.bindInverse.clear();
    o.mesh.weights.clear();
    o.mesh.touch();
}
