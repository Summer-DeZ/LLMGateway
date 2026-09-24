#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

#include "base/current_thread.hpp"
#include "net/event_loop.hpp"
#include "net/event_loop_thread.hpp"
#include "net/event_loop_thread_pool.hpp"

using namespace std::chrono_literals;
using gw::net::EventLoop;
using gw::net::EventLoopThread;
using gw::net::EventLoopThreadPool;
using gw::net::TimerId;

TEST(TimerQueueTest, RunAfterOrdering) {
    EventLoop loop;
    std::vector<int> order;
    loop.runAfter(60ms, [&] { order.push_back(1); });
    loop.runAfter(10ms, [&] { order.push_back(2); });
    loop.runAfter(30ms, [&] { order.push_back(3); });
    loop.runAfter(90ms, [&] {
        order.push_back(4);
        loop.quit();
    });
    loop.loop();
    EXPECT_EQ(order, (std::vector<int>{2, 3, 1, 4}));
}

TEST(TimerQueueTest, RunEveryCount) {
    EventLoop loop;
    std::atomic<int> count{0};
    loop.runEvery(5ms, [&] { count.fetch_add(1); });
    loop.runAfter(60ms, [&] { loop.quit(); });
    loop.loop();
    EXPECT_GE(count.load(), 4);
    EXPECT_LE(count.load(), 20);
}

TEST(TimerQueueTest, CancelSelfInCallback) {
    EventLoop loop;
    std::atomic<int> count{0};
    std::optional<TimerId> id;
    id = loop.runEvery(5ms, [&] {
        if (count.fetch_add(1) + 1 == 3) {
            loop.cancel(*id);
        }
    });
    loop.runAfter(50ms, [&] { loop.quit(); });
    loop.loop();
    EXPECT_EQ(count.load(), 3);
}

TEST(TimerQueueTest, CancelOtherPendingTimer) {
    EventLoop loop;
    bool other_fired = false;
    TimerId other = loop.runAfter(50ms, [&] { other_fired = true; });
    loop.cancel(other);
    loop.runAfter(80ms, [&] { loop.quit(); });
    loop.loop();
    EXPECT_FALSE(other_fired);
}

TEST(TimerQueueTest, CrossThreadRunInLoopAndQuit) {
    EventLoopThread thread;
    EventLoop* loop = thread.startLoop();
    constexpr int kCount = 10000;
    std::atomic<int> count{0};
    std::thread caller([&] {
        for (int i = 0; i < kCount; ++i) {
            loop->runInLoop([&] { count.fetch_add(1); });
        }
    });
    caller.join();

    std::atomic<bool> flushed{false};
    loop->runInLoop([&] { flushed.store(true); });
    while (!flushed.load()) std::this_thread::sleep_for(1ms);

    EXPECT_EQ(count.load(), kCount);
    loop->quit();
}

TEST(TimerQueueTest, EventLoopThreadPoolRoundRobin) {
    EventLoop base;
    EventLoopThreadPool pool(&base, "pool");
    pool.setThreadNum(4);
    std::mutex mutex;
    std::unordered_map<EventLoop*, pid_t> init_tids;
    std::unordered_map<EventLoop*, bool> init_in_thread;
    pool.start([&](EventLoop* loop) {
        std::lock_guard<std::mutex> lock(mutex);
        init_tids[loop] = gw::CurrentThread::tid();
        init_in_thread[loop] = loop->isInLoopThread();
    });
    ASSERT_EQ(pool.allLoops().size(), 4u);
    std::vector<EventLoop*> order;
    for (int i = 0; i < 8; ++i) order.push_back(pool.getNextLoop());
    EXPECT_EQ(order[0], order[4]);
    EXPECT_EQ(order[1], order[5]);
    EXPECT_EQ(order[2], order[6]);
    EXPECT_EQ(order[3], order[7]);
    EXPECT_NE(order[0], order[1]);

    {
        std::lock_guard<std::mutex> lock(mutex);
        EXPECT_EQ(init_tids.size(), 4u);
        pid_t main_tid = gw::CurrentThread::tid();
        for (auto& [loop, tid] : init_tids) {
            EXPECT_NE(tid, main_tid);
            EXPECT_TRUE(init_in_thread[loop]);
        }
    }
}

TEST(TimerQueueTest, QuitFromAnotherThread) {
    EventLoop loop;
    std::thread stopper([&] {
        std::this_thread::sleep_for(20ms);
        loop.quit();
    });
    loop.loop();
    stopper.join();
    SUCCEED();
}

TEST(TimerQueueTest, LongTimerAccuracy) {
    EventLoop loop;
    auto start = std::chrono::steady_clock::now();
    std::chrono::milliseconds elapsed{0};
    loop.runAfter(1000ms, [&] {
        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        loop.quit();
    });
    loop.loop();
    EXPECT_GE(elapsed.count(), 950);
    EXPECT_LE(elapsed.count(), 1400) << "elapsed=" << elapsed.count();
}

TEST(TimerQueueTest, TwoSecondTimerAccuracy) {
    EventLoop loop;
    auto start = std::chrono::steady_clock::now();
    std::chrono::milliseconds elapsed{0};
    loop.runAfter(2000ms, [&] {
        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        loop.quit();
    });
    loop.loop();
    EXPECT_GE(elapsed.count(), 1900);
    EXPECT_LE(elapsed.count(), 2600) << "elapsed=" << elapsed.count();
}
