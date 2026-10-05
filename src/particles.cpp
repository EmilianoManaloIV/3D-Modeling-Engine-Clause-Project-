#include "particles.h"

#include <cmath>

namespace {
float rand01(uint32_t& s) {  // xorshift32
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return (s >> 8) * (1.0f / 16777216.0f);
}
}  // namespace

void stepParticles(ParticleSystemState& st, const ParticleSettings& ps, const Mat4& W, float dt) {
    dt = clampf(dt, 0.0f, 0.1f);

    // Integrate and retire.
    const float dragFactor = std::max(0.0f, 1.0f - ps.drag * dt);
    for (size_t i = 0; i < st.particles.size();) {
        Particle& p = st.particles[i];
        p.age += dt;
        if (p.age >= p.life) {
            p = st.particles.back();
            st.particles.pop_back();
            continue;
        }
        p.vel.y += ps.gravity * dt;
        p.vel *= dragFactor;
        p.pos += p.vel * dt;
        ++i;
    }

    // Spawn.
    st.spawnAccumulator += std::max(0.0f, ps.rate) * dt;
    const float cosSpread = std::cos(toRadians(clampf(ps.spread, 0.0f, 180.0f)));
    while (st.spawnAccumulator >= 1.0f) {
        st.spawnAccumulator -= 1.0f;
        if (st.particles.size() >= kMaxParticlesPerEmitter) continue;
        // Uniform point in a sphere (rejection sampling).
        Vec3 offset;
        for (int tries = 0; tries < 8; ++tries) {
            offset = {rand01(st.rng) * 2 - 1, rand01(st.rng) * 2 - 1, rand01(st.rng) * 2 - 1};
            if (dot(offset, offset) <= 1.0f) break;
        }
        // Uniform direction inside the cone around local +Y.
        float cz = 1.0f - rand01(st.rng) * (1.0f - cosSpread);
        float sz = std::sqrt(std::max(0.0f, 1.0f - cz * cz));
        float phi = rand01(st.rng) * 2.0f * kPi;
        Vec3 dirLocal{sz * std::cos(phi), cz, sz * std::sin(phi)};
        Particle p;
        p.pos = transformPoint(W, offset * ps.radius);
        p.vel = normalize(transformDir(W, dirLocal)) * (ps.speed * (0.8f + 0.4f * rand01(st.rng)));
        p.age = 0;
        p.life = std::max(0.01f, ps.lifetime * (0.75f + 0.5f * rand01(st.rng)));
        st.particles.push_back(p);
    }
}

void appendParticleVertices(const ParticleSystemState& st, const ParticleSettings& ps, std::vector<ParticleVertex>& out) {
    for (const Particle& p : st.particles) {
        float t = clampf(p.age / p.life, 0.0f, 1.0f);
        Vec3 c = lerp(ps.startColor, ps.endColor, t);
        float alpha = std::min(1.0f, t * 10.0f) * (1.0f - t);  // quick fade in, slow fade out
        float size = ps.startSize + (ps.endSize - ps.startSize) * t;
        out.push_back({p.pos, c.x, c.y, c.z, alpha, std::max(0.0f, size)});
    }
}
