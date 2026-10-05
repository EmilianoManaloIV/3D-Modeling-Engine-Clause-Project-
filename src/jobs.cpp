#include "jobs.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace jobs {
namespace {

struct Pool {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> queue;
    std::vector<std::thread> workers;
    bool stop = false;
};

Pool* g_pool = nullptr;
int g_threads = 1;

bool popTask(Pool& p, std::function<void()>& out) {
    std::lock_guard<std::mutex> lock(p.mutex);
    if (p.queue.empty()) return false;
    out = std::move(p.queue.front());
    p.queue.pop_front();
    return true;
}

void workerLoop(Pool* p) {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(p->mutex);
            p->wake.wait(lock, [&] { return p->stop || !p->queue.empty(); });
            if (p->stop && p->queue.empty()) return;
            task = std::move(p->queue.front());
            p->queue.pop_front();
        }
        task();
    }
}

void push(std::function<void()> task, bool front) {
    {
        std::lock_guard<std::mutex> lock(g_pool->mutex);
        if (front) g_pool->queue.push_front(std::move(task));
        else g_pool->queue.push_back(std::move(task));
    }
    g_pool->wake.notify_one();
}

}  // namespace

int hardwareThreads() { return std::max(1u, std::thread::hardware_concurrency()); }

void init(int threads) {
    if (g_pool) return;
    g_threads = threads > 0 ? threads : hardwareThreads();
    g_pool = new Pool();
    for (int i = 0; i < g_threads - 1; ++i) g_pool->workers.emplace_back(workerLoop, g_pool);
}

void shutdown() {
    if (!g_pool) return;
    {
        std::lock_guard<std::mutex> lock(g_pool->mutex);
        g_pool->stop = true;
    }
    g_pool->wake.notify_all();
    for (auto& t : g_pool->workers) t.join();
    delete g_pool;
    g_pool = nullptr;
}

int threadCount() { return g_pool ? g_threads : 1; }

void setThreadCount(int threads) {
    threads = std::max(1, std::min(threads, 256));
    if (g_pool && threads == g_threads) return;
    shutdown();
    init(threads);
}

void parallelFor(int begin, int end, int grain, const std::function<void(int, int)>& fn) {
    const int n = end - begin;
    if (n <= 0) return;
    grain = std::max(1, grain);
    const int threads = threadCount();
    if (!g_pool || threads == 1 || n <= grain) {
        fn(begin, end);
        return;
    }
    // ~4 chunks per thread for load balance, but never below the grain size.
    int chunk = std::max(grain, (n + threads * 4 - 1) / (threads * 4));
    int chunks = (n + chunk - 1) / chunk;
    std::atomic<int> remaining{chunks};
    for (int c = 1; c < chunks; ++c) {
        int b = begin + c * chunk, e = std::min(end, b + chunk);
        push([&fn, &remaining, b, e] {
            fn(b, e);
            remaining.fetch_sub(1);
        }, true);
    }
    fn(begin, std::min(end, begin + chunk));
    remaining.fetch_sub(1);
    // Help with queued work until our chunks are finished.
    while (remaining.load() > 0) {
        std::function<void()> task;
        if (popTask(*g_pool, task)) task();
        else std::this_thread::yield();
    }
}

void run(TaskGroup& group, std::function<void()> task) {
    group.pending.fetch_add(1);
    if (!g_pool || threadCount() == 1) {  // no workers: run inline
        task();
        group.pending.fetch_sub(1);
        return;
    }
    push([&group, t = std::move(task)] {
        t();
        group.pending.fetch_sub(1);
    }, false);
}

void wait(TaskGroup& group) {
    while (!group.done()) {
        std::function<void()> task;
        if (g_pool && popTask(*g_pool, task)) task();
        else std::this_thread::yield();
    }
}

}  // namespace jobs
