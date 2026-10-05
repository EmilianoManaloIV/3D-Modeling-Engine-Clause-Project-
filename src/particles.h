#pragma once
// Simple CPU particle system (FoCG 5e sec. 16.7 "Groups of Objects"; GEA
// Vol. II sec. 11.6 visual effects), integrated with explicit Euler steps
// (GEA Vol. II sec. 14.4). An emitter spawns particles at a
// constant rate inside a small sphere, shooting them along its local +Y axis
// within a cone; they fall under gravity, slow down with drag, and fade /
// shrink / change color over their lifetime. Emitters are hierarchy objects,
// so parenting one to a bone or a moving object carries the effect along.
#include "scene.h"

#include <cstdint>
#include <vector>

struct Particle {
    Vec3 pos, vel;
    float age, life;
};

struct ParticleSystemState {
    std::vector<Particle> particles;
    float spawnAccumulator = 0;
    uint32_t rng = 0x9E3779B9u;
};

struct ParticleVertex {
    Vec3 pos;
    float r, g, b, a;
    float size;  // world-space diameter
};

constexpr size_t kMaxParticlesPerEmitter = 20000;

void stepParticles(ParticleSystemState& state, const ParticleSettings& settings, const Mat4& emitterWorld, float dt);
void appendParticleVertices(const ParticleSystemState& state, const ParticleSettings& settings,
                            std::vector<ParticleVertex>& out);
