#include "plgen/thread_pool.hpp"

#include <algorithm>
#include <iostream>

namespace plgen {

ThreadPool::ThreadPool(std::size_t threads, std::size_t max_queue)
    : max_queue_(std::max<std::size_t>(max_queue, 1)) {
  threads = std::max<std::size_t>(threads, 1);
  workers_.reserve(threads);
  for (std::size_t i = 0; i < threads; ++i) workers_.emplace_back([this] { worker_loop(); });
}

ThreadPool::~ThreadPool() {
  {
    const std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  ready_.notify_all();  // wake every sleeping worker so it can see stopping_
  workers_.clear();     // std::jthread joins in its destructor
}

bool ThreadPool::try_submit(std::function<void()> task) {
  {
    const std::lock_guard lock(mutex_);
    if (stopping_ || tasks_.size() >= max_queue_) return false;
    tasks_.push(std::move(task));
  }
  ready_.notify_one();  // notify after unlocking so the woken worker doesn't block on the mutex
  return true;
}

void ThreadPool::worker_loop() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock lock(mutex_);
      // Sleep (using no CPU) until there's work or we're shutting down.
      // The predicate guards against spurious wake-ups.
      ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
      if (tasks_.empty()) return;  // stopping and nothing left to do
      task = std::move(tasks_.front());
      tasks_.pop();
    }  // unlock BEFORE running the task, so other workers can grab tasks meanwhile
    try {
      task();
    } catch (const std::exception& e) {
      std::cerr << "worker: task threw: " << e.what() << '\n';  // never let one request kill a worker
    } catch (...) {
      std::cerr << "worker: task threw an unknown exception\n";
    }
  }
}

}  // namespace plgen
