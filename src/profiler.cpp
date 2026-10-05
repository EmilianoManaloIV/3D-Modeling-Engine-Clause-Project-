#include "profiler.h"

#include <algorithm>

namespace prof {
namespace {
constexpr int kHistory = 60;

struct Entry {
    const char* name;
    double frame = 0;              // this frame
    double history[kHistory] = {};
    double total = 0;
    long long calls = 0;
    bool isCounter = false;
};

std::vector<Entry> g_entries;
int g_frame = 0;
int g_framesSinceReset = 0;

Entry& entry(const char* name, bool counter) {
    for (Entry& e : g_entries)
        if (e.name == name) return e;
    g_entries.push_back(Entry());
    g_entries.back().name = name;
    g_entries.back().isCounter = counter;
    return g_entries.back();
}
}  // namespace

void beginFrame() {
    for (Entry& e : g_entries) e.frame = 0;
}

void endFrame() {
    const int slot = g_frame % kHistory;
    for (Entry& e : g_entries) {
        e.history[slot] = e.frame;
        e.total += e.frame;
    }
    ++g_frame;
    ++g_framesSinceReset;
}

void add(const char* name, double ms) {
    Entry& e = entry(name, false);
    e.frame += ms;
    e.calls++;
}

void count(const char* name, double value) { entry(name, true).frame += value; }

std::vector<Stat> stats() {
    std::vector<Stat> out;
    const int n = std::min(g_frame, kHistory);
    for (const Entry& e : g_entries) {
        if (e.isCounter) continue;
        double sum = 0, mx = 0;
        for (int i = 0; i < n; ++i) {
            sum += e.history[i];
            mx = std::max(mx, e.history[i]);
        }
        out.push_back({e.name, n ? sum / n : 0, mx, e.total, e.calls});
    }
    return out;
}

std::vector<Counter> counters() {
    std::vector<Counter> out;
    const int n = std::min(g_frame, kHistory);
    for (const Entry& e : g_entries) {
        if (!e.isCounter) continue;
        double sum = 0;
        for (int i = 0; i < n; ++i) sum += e.history[i];
        out.push_back({e.name, n ? sum / n : 0, e.total});
    }
    return out;
}

void resetTotals() {
    for (Entry& e : g_entries) {
        e.total = 0;
        e.calls = 0;
    }
    g_framesSinceReset = 0;
}

int framesSinceReset() { return g_framesSinceReset; }

}  // namespace prof
