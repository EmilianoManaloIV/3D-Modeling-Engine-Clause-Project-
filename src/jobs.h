#pragma once
// Small job system (GEA Vol. I ch. 4, parallelism and concurrent programming;
// sec. 8.6, multiprocessor game loops / job systems).
//
// A fixed pool of worker threads pulls tasks from one queue.
//   parallelFor   - fork/join over an index range; the calling thread helps,
//                   and its chunks jump the queue so interactive work is never
//                   stuck behind long background tasks.
//   TaskGroup     - fire-and-forget background tasks (e.g. path-tracing tiles)
//                   that the caller polls with done() / waits for.
// Everything degrades gracefully to a single thread (threads = 1).
#include <atomic>
#include <functional>

namespace jobs {

void init(int threads = 0);  // 0 = hardware concurrency
void shutdown();
int threadCount();           // workers + the calling thread
int hardwareThreads();
void setThreadCount(int threads);  // restarts the pool

// fn(begin, end) is called for consecutive chunks of [begin, end). Chunks are
// at least `grain` long. Blocks until all chunks are done.
void parallelFor(int begin, int end, int grain, const std::function<void(int, int)>& fn);

struct TaskGroup {
    std::atomic<int> pending{0};
    std::atomic<bool> cancel{false};
    bool done() const { return pending.load() == 0; }
};
void run(TaskGroup& group, std::function<void()> task);  // asynchronous
void wait(TaskGroup& group);                              // helps until the group is done

}  // namespace jobs
