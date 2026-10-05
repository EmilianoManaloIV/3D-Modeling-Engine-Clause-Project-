#pragma once
// Tiny flat CPU profiler (in the spirit of the in-engine
// profiling tools of GEA Vol. I sec. 2.3 / ch. 10). Code marks sections with
// PROF_SCOPE("name"); each frame's totals are folded into a rolling history
// (for the F3 overlay) and into run totals (for --benchmark reports).
// Section names must be string literals (they are compared by pointer).
#include <chrono>
#include <cstdint>
#include <vector>

namespace prof {

struct Stat {
    const char* name;
    double avgMs;    // rolling average over the last frames
    double maxMs;    // max over the same window
    double totalMs;  // since resetTotals()
    long long calls; // since resetTotals()
};

struct Counter {
    const char* name;
    double avg;      // rolling average per frame
    double total;    // since resetTotals()
};

void beginFrame();
void endFrame();
void add(const char* name, double ms);
void count(const char* name, double value);  // per-frame counters (draw calls, uploads...)
std::vector<Stat> stats();
std::vector<Counter> counters();
void resetTotals();
int framesSinceReset();

class Scope {
public:
    explicit Scope(const char* name) : name_(name), start_(std::chrono::steady_clock::now()) {}
    ~Scope() {
        add(name_, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count());
    }

private:
    const char* name_;
    std::chrono::steady_clock::time_point start_;
};

}  // namespace prof

#define PROF_CONCAT2(a, b) a##b
#define PROF_CONCAT(a, b) PROF_CONCAT2(a, b)
#define PROF_SCOPE(name) prof::Scope PROF_CONCAT(profScope_, __LINE__)(name)
