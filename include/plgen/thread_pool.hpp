#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace plgen {

// A fixed set of worker threads pulling tasks from a shared queue.
//
// Why not one thread per request? Creating a thread costs tens of microseconds
// and memory for its stack; under a traffic spike you'd create thousands and
// the machine would thrash. A pool caps concurrency at a sensible number
// (usually the CPU core count) and reuses threads.
//
// Backpressure: the queue has a maximum size. When it's full, try_submit()
// returns false and the server answers 503 immediately instead of piling up
// work it can't finish. Failing fast beats timing out slowly.
class ThreadPool {
 public:
  ThreadPool(std::size_t threads, std::size_t max_queue);
  ~ThreadPool();  // finishes queued tasks, then joins all workers

  ThreadPool(const ThreadPool&) = delete;  // owns threads: not copyable
  ThreadPool& operator=(const ThreadPool&) = delete;

  // Returns false (and does not run the task) if the queue is full or shutting down.
  [[nodiscard]] bool try_submit(std::function<void()> task);

  [[nodiscard]] std::size_t thread_count() const noexcept { return workers_.size(); }

 private:
  void worker_loop();

  std::mutex mutex_;              // guards everything below
  std::condition_variable ready_; // signalled when a task arrives or we're stopping
  std::queue<std::function<void()>> tasks_;
  std::size_t max_queue_;
  bool stopping_ = false;
  std::vector<std::jthread> workers_;  // declared last: threads start after the rest is ready
};

}  // namespace plgen
