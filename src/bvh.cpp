#include "bvh.h"

#include "jobs.h"

#include <algorithm>
#include <cmath>

namespace {

struct Box {
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    void grow(Vec3 p) {
        lo = vmin(lo, p);
        hi = vmax(hi, p);
    }
    void grow(const Box& b) {
        lo = vmin(lo, b.lo);
        hi = vmax(hi, b.hi);
    }
    float area() const {
        Vec3 e = hi - lo;
        if (e.x < 0) return 0;
        return 2.0f * (e.x * e.y + e.y * e.z + e.z * e.x);
    }
};

constexpr int kBins = 16;

// Moller-Trumbore ray / triangle test (FoCG 5e sec. 4.4.2).
inline bool hitTriangle(Vec3 o, Vec3 d, Vec3 a, Vec3 b, Vec3 c, float tMin, float tMax, float& t, float& u,
                        float& v) {
    Vec3 e1 = b - a, e2 = c - a;
    Vec3 p = cross(d, e2);
    float det = dot(e1, p);
    if (std::fabs(det) < 1e-12f) return false;
    float inv = 1.0f / det;
    Vec3 s = o - a;
    u = dot(s, p) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    Vec3 q = cross(s, e1);
    v = dot(d, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    t = dot(e2, q) * inv;
    return t > tMin && t < tMax;
}

inline bool hitBox(const BvhNode& n, Vec3 o, Vec3 invD, float tMax, float& tEnter) {
    float t0 = 0.0f, t1 = tMax;
    for (int a = 0; a < 3; ++a) {
        float tn = (n.lo[a] - o[a]) * invD[a];
        float tf = (n.hi[a] - o[a]) * invD[a];
        if (tn > tf) std::swap(tn, tf);
        t0 = tn > t0 ? tn : t0;
        t1 = tf < t1 ? tf : t1;
        if (t0 > t1) return false;
    }
    tEnter = t0;
    return true;
}

}  // namespace

namespace {
struct BuildTask {
    int node, first, count, depth;
};
struct BuildContext {
    const std::vector<Box>& boxes;
    const std::vector<Vec3>& centers;
    std::vector<int32_t>& order;
    int maxLeafSize;
};

// Top-down binned-SAH build of the subtree in `nodes[root]` over
// order[first, first + count). Subtrees no larger than `deferBelow` (but
// still big) are left as reserved nodes in `deferred` for parallel builds.
int buildNodes(const BuildContext& c, std::vector<BvhNode>& nodes, BuildTask rootTask, int deferBelow,
               std::vector<BuildTask>* deferred) {
    int maxDepth = 0;
    std::vector<BuildTask> stack;
    stack.push_back(rootTask);
    while (!stack.empty()) {
        BuildTask t = stack.back();
        stack.pop_back();
        if (deferred && t.count <= deferBelow && t.node != rootTask.node) {
            deferred->push_back(t);
            continue;
        }
        maxDepth = std::max(maxDepth, t.depth);
        Box bounds, cbounds;
        for (int i = t.first; i < t.first + t.count; ++i) {
            bounds.grow(c.boxes[c.order[i]]);
            cbounds.grow(c.centers[c.order[i]]);
        }
        nodes[t.node].lo = bounds.lo;
        nodes[t.node].hi = bounds.hi;
        auto makeLeaf = [&]() {
            nodes[t.node].leftOrFirst = t.first;
            nodes[t.node].count = t.count;
        };
        if (t.count <= c.maxLeafSize || t.depth >= 60) {
            makeLeaf();
            continue;
        }
        // Binned SAH: cost = A_left * N_left + A_right * N_right.
        float bestCost = 1e30f;
        int bestAxis = -1, bestSplit = 0;
        for (int axis = 0; axis < 3; ++axis) {
            float lo = cbounds.lo[axis], extent = cbounds.hi[axis] - lo;
            if (extent <= 1e-9f) continue;
            Box bins[kBins];
            int counts[kBins] = {};
            float scale = kBins / extent;
            for (int i = t.first; i < t.first + t.count; ++i) {
                int b = std::min(kBins - 1, (int)((c.centers[c.order[i]][axis] - lo) * scale));
                bins[b].grow(c.boxes[c.order[i]]);
                counts[b]++;
            }
            float leftArea[kBins - 1];
            int leftCount[kBins - 1];
            Box acc;
            int n = 0;
            for (int b = 0; b < kBins - 1; ++b) {
                acc.grow(bins[b]);
                n += counts[b];
                leftArea[b] = acc.area();
                leftCount[b] = n;
            }
            acc = Box();
            n = 0;
            for (int b = kBins - 1; b > 0; --b) {
                acc.grow(bins[b]);
                n += counts[b];
                float cost = leftArea[b - 1] * leftCount[b - 1] + acc.area() * n;
                if (leftCount[b - 1] > 0 && n > 0 && cost < bestCost) {
                    bestCost = cost;
                    bestAxis = axis;
                    bestSplit = b;
                }
            }
        }
        float leafCost = bounds.area() * t.count;
        int mid;
        if (bestAxis < 0 || (bestCost >= leafCost && t.count <= 16)) {
            if (bestAxis < 0) {
                // All centroids coincide: split in the middle of the list.
                if (t.count <= 16) {
                    makeLeaf();
                    continue;
                }
                mid = t.first + t.count / 2;
            } else {
                makeLeaf();
                continue;
            }
        } else {
            float lo = cbounds.lo[bestAxis], scale = kBins / (cbounds.hi[bestAxis] - lo);
            auto it = std::partition(c.order.begin() + t.first, c.order.begin() + t.first + t.count, [&](int tri) {
                int b = std::min(kBins - 1, (int)((c.centers[tri][bestAxis] - lo) * scale));
                return b < bestSplit;
            });
            mid = (int)(it - c.order.begin());
            if (mid == t.first || mid == t.first + t.count) mid = t.first + t.count / 2;
        }
        int left = (int)nodes.size();
        nodes.emplace_back();
        nodes.emplace_back();
        nodes[t.node].leftOrFirst = left;
        nodes[t.node].count = 0;
        stack.push_back({left, t.first, mid - t.first, t.depth + 1});
        stack.push_back({left + 1, mid, t.first + t.count - mid, t.depth + 1});
    }
    return maxDepth;
}
}  // namespace

void Bvh::build(const std::vector<Vec3>& v, int maxLeafSize) {
    const int triCount = (int)(v.size() / 3);
    nodes.clear();
    order.resize(triCount);
    maxDepth = 0;
    if (triCount == 0) return;
    std::vector<Box> boxes(triCount);
    std::vector<Vec3> centers(triCount);
    jobs::parallelFor(0, triCount, 8192, [&](int b, int e) {
        for (int i = b; i < e; ++i) {
            boxes[i] = Box();
            boxes[i].grow(v[3 * i]);
            boxes[i].grow(v[3 * i + 1]);
            boxes[i].grow(v[3 * i + 2]);
            centers[i] = (boxes[i].lo + boxes[i].hi) * 0.5f;
            order[i] = i;
        }
    });
    nodes.reserve(triCount * 2);
    nodes.emplace_back();
    BuildContext ctx{boxes, centers, order, maxLeafSize};
    // The top levels are built serially; below them, independent subtrees
    // (disjoint ranges of `order`) are built in parallel into local arrays
    // and appended (GEA Vol. I ch. 4: fork/join parallelism).
    const int threads = jobs::threadCount();
    const int deferBelow = (threads > 1 && triCount > 20000) ? std::max(4096, triCount / (threads * 4)) : 0;
    std::vector<BuildTask> deferred;
    maxDepth = buildNodes(ctx, nodes, {0, 0, triCount, 0}, deferBelow, deferBelow ? &deferred : nullptr);
    if (deferred.empty()) return;
    std::vector<std::vector<BvhNode>> local(deferred.size());
    std::vector<int> depths(deferred.size(), 0);
    jobs::parallelFor(0, (int)deferred.size(), 1, [&](int b, int e) {
        for (int k = b; k < e; ++k) {
            local[k].reserve(deferred[k].count * 2);
            local[k].emplace_back();
            BuildTask t = deferred[k];
            t.node = 0;
            depths[k] = buildNodes(ctx, local[k], t, 0, nullptr);
        }
    });
    for (size_t k = 0; k < deferred.size(); ++k) {
        const int base = (int)nodes.size() - 1;  // local index i >= 1 -> base + i
        auto remap = [&](BvhNode n) {
            if (n.count == 0) n.leftOrFirst += base;
            return n;
        };
        nodes[deferred[k].node] = remap(local[k][0]);
        for (size_t i = 1; i < local[k].size(); ++i) nodes.push_back(remap(local[k][i]));
        maxDepth = std::max(maxDepth, depths[k]);
    }
}

bool intersectBvh(const Bvh& bvh, const std::vector<Vec3>& v, Vec3 o, Vec3 d, float tMin, Hit& hit) {
    if (bvh.nodes.empty()) return false;
    Vec3 invD(1.0f / (std::fabs(d.x) > 1e-20f ? d.x : 1e-20f), 1.0f / (std::fabs(d.y) > 1e-20f ? d.y : 1e-20f),
              1.0f / (std::fabs(d.z) > 1e-20f ? d.z : 1e-20f));
    int stack[64];
    int sp = 0;
    stack[sp++] = 0;
    bool found = false;
    float tEnter;
    while (sp > 0) {
        const BvhNode& n = bvh.nodes[stack[--sp]];
        if (!hitBox(n, o, invD, hit.t, tEnter)) continue;
        if (n.count > 0) {
            for (int i = n.leftOrFirst; i < n.leftOrFirst + n.count; ++i) {
                int tri = bvh.order[i];
                float t, u, w;
                if (hitTriangle(o, d, v[3 * tri], v[3 * tri + 1], v[3 * tri + 2], tMin, hit.t, t, u, w)) {
                    hit.t = t;
                    hit.tri = tri;
                    hit.u = u;
                    hit.v = w;
                    found = true;
                }
            }
        } else if (sp < 62) {
            // Visit the nearer child first.
            const BvhNode &a = bvh.nodes[n.leftOrFirst], &b = bvh.nodes[n.leftOrFirst + 1];
            float ta = 1e30f, tb = 1e30f;
            bool ha = hitBox(a, o, invD, hit.t, ta), hb = hitBox(b, o, invD, hit.t, tb);
            if (ha && hb) {
                if (ta < tb) {
                    stack[sp++] = n.leftOrFirst + 1;
                    stack[sp++] = n.leftOrFirst;
                } else {
                    stack[sp++] = n.leftOrFirst;
                    stack[sp++] = n.leftOrFirst + 1;
                }
            } else if (ha) {
                stack[sp++] = n.leftOrFirst;
            } else if (hb) {
                stack[sp++] = n.leftOrFirst + 1;
            }
        }
    }
    return found;
}

bool occludedBvh(const Bvh& bvh, const std::vector<Vec3>& v, Vec3 o, Vec3 d, float tMin, float tMax,
                 const std::vector<uint8_t>* skip) {
    if (bvh.nodes.empty()) return false;
    Vec3 invD(1.0f / (std::fabs(d.x) > 1e-20f ? d.x : 1e-20f), 1.0f / (std::fabs(d.y) > 1e-20f ? d.y : 1e-20f),
              1.0f / (std::fabs(d.z) > 1e-20f ? d.z : 1e-20f));
    int stack[64];
    int sp = 0;
    stack[sp++] = 0;
    float tEnter;
    while (sp > 0) {
        const BvhNode& n = bvh.nodes[stack[--sp]];
        if (!hitBox(n, o, invD, tMax, tEnter)) continue;
        if (n.count > 0) {
            for (int i = n.leftOrFirst; i < n.leftOrFirst + n.count; ++i) {
                int tri = bvh.order[i];
                if (skip && (*skip)[tri]) continue;
                float t, u, w;
                if (hitTriangle(o, d, v[3 * tri], v[3 * tri + 1], v[3 * tri + 2], tMin, tMax, t, u, w)) return true;
            }
        } else if (sp < 62) {
            stack[sp++] = n.leftOrFirst;
            stack[sp++] = n.leftOrFirst + 1;
        }
    }
    return false;
}
