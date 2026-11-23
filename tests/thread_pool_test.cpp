#include <gtest/gtest.h>

#include <atomic>
#include <future>

#include "plgen/thread_pool.hpp"

using plgen::ThreadPool;

TEST(ThreadPool, RunsEveryTask) {
  std::atomic<int> count{0};
  {
    ThreadPool pool(4, 1000);
    for (int i = 0; i < 500; ++i) ASSERT_TRUE(pool.try_submit([&] { ++count; }));
  }  // destructor waits for queued tasks to finish
  EXPECT_EQ(count.load(), 500);
}

TEST(ThreadPool, RejectsWhenQueueIsFull) {
  std::promise<void> release;
  std::shared_future<void> gate = release.get_future().share();
  std::promise<void> started;
  {
    ThreadPool pool(1, 2);
    ASSERT_TRUE(pool.try_submit([&, gate] { started.set_value(); gate.wait(); }));  // occupies the worker
    started.get_future().wait();
    EXPECT_TRUE(pool.try_submit([] {}));   // queue: 1/2
    EXPECT_TRUE(pool.try_submit([] {}));   // queue: 2/2
    EXPECT_FALSE(pool.try_submit([] {}));  // full -> rejected (server would send 503)
    release.set_value();
  }
}

TEST(ThreadPool, SurvivesThrowingTasks) {
  std::atomic<int> count{0};
  {
    ThreadPool pool(1, 10);
    ASSERT_TRUE(pool.try_submit([] { throw std::runtime_error("boom"); }));
    ASSERT_TRUE(pool.try_submit([&] { ++count; }));
  }
  EXPECT_EQ(count.load(), 1);  // the worker kept going after the exception
}
