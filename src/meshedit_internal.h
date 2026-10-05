#pragma once
// Helpers shared by meshedit.cpp and meshedit_tools.cpp.
#include "meshedit.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace meshedit {
namespace detail {

inline uint64_t directedKey(int a, int b) { return (uint64_t(uint32_t(a)) << 32) | uint32_t(b); }
inline uint64_t edgeKey(int a, int b) { return a < b ? directedKey(a, b) : directedKey(b, a); }
inline uint64_t edgeKey(const Edge& e) { return edgeKey(e.first, e.second); }

// Linear map from positions in a face's plane to its UVs, from the face's
// first corner and the two edges spanning the largest area.
struct UvMap {
    bool ok = false;
    Vec3 du, dv;  // uv change per unit of world offset (gradients)
    Vec2 apply(Vec2 uv, Vec3 offset) const { return ok ? uv + Vec2(dot(du, offset), dot(dv, offset)) : uv; }
};
UvMap uvMapFor(const Mesh& m, int f);

// (1 - t) * a + t * b, keeping the 4 strongest influences.
BoneWeights blendWeights(const BoneWeights& a, const BoneWeights& b, float t);

// Open-addressing hash map from 64-bit keys to ints (linear probing). Several
// times faster to build than std::unordered_map for the half-edge tables of
// dense meshes (one allocation, no per-node memory).
class FlatMap {
public:
    void reserve(size_t n) {
        size_t cap = 16;
        while (cap < n * 2) cap <<= 1;
        keys_.assign(cap, kEmpty);
        vals_.assign(cap, 0);
        mask_ = cap - 1;
    }
    // Inserts unless the key exists (the first value wins).
    void emplace(uint64_t k, int v) {
        size_t i = slot(k);
        if (keys_[i] == kEmpty) keys_[i] = k, vals_[i] = v;
    }
    int find(uint64_t k) const {
        if (keys_.empty()) return -1;
        size_t i = slot(k);
        return keys_[i] == kEmpty ? -1 : vals_[i];
    }

private:
    static constexpr uint64_t kEmpty = ~0ull;
    size_t slot(uint64_t k) const {
        uint64_t h = k * 0x9E3779B97F4A7C15ull;
        size_t i = (size_t)(h >> 20) & mask_;
        while (keys_[i] != kEmpty && keys_[i] != k) i = (i + 1) & mask_;
        return i;
    }
    std::vector<uint64_t> keys_;
    std::vector<int> vals_;
    size_t mask_ = 0;
};

// Half-edge view of a polygon mesh: one half-edge per face corner, running
// from that corner to the next one in the face's winding order. Built once
// per tool invocation; the mesh must not change while it is in use.
struct HalfEdges {
    const Mesh* m;
    std::vector<int> start;   // first half-edge of each face (size F + 1)
    std::vector<int> faceOf;  // face of each half-edge
    FlatMap byDirected;

    explicit HalfEdges(const Mesh& mesh);
    int count() const { return (int)faceOf.size(); }
    int face(int h) const { return faceOf[h]; }
    int corner(int h) const { return h - start[faceOf[h]]; }
    int size(int h) const { return start[faceOf[h] + 1] - start[faceOf[h]]; }
    int from(int h) const { return m->faces[faceOf[h]][corner(h)]; }
    int to(int h) const { return m->faces[faceOf[h]][(corner(h) + 1) % size(h)]; }
    int next(int h) const { return start[faceOf[h]] + (corner(h) + 1) % size(h); }
    int prev(int h) const { return start[faceOf[h]] + (corner(h) + size(h) - 1) % size(h); }
    int find(int a, int b) const { return byDirected.find(directedKey(a, b)); }
    int twin(int h) const { return find(to(h), from(h)); }
};

}  // namespace detail
}  // namespace meshedit
