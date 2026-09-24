#pragma once

#include <any>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "base/noncopyable.hpp"
#include "base/timestamp.hpp"
#include "net/callbacks.hpp"
#include "net/timer_id.hpp"

namespace gw::net {

class Channel;
class Poller;
class TimerQueue;

class EventLoop : noncopyable {
public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();

    void loop();
    void quit();

    void runInLoop(Functor cb);
    void queueInLoop(Functor cb);

    TimerId runAt(Timestamp time, TimerCallback cb);
    TimerId runAfter(Duration delay, TimerCallback cb);
    TimerId runEvery(Duration interval, TimerCallback cb);
    void cancel(TimerId timer_id);

    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);

    bool isInLoopThread() const;
    void assertInLoopThread() const;

    std::any& context() { return context_; }

    Timestamp pollReturnTime() const { return poll_return_time_; }

    void wakeup();

    static EventLoop* current();

private:
    void handleWakeupRead();
    void doPendingFunctors();

    std::atomic<bool> quit_{false};
    std::atomic<bool> calling_pending_functors_{false};
    std::unique_ptr<Poller> poller_;
    int wakeup_fd_;
    std::unique_ptr<Channel> wakeup_channel_;
    std::unique_ptr<TimerQueue> timer_queue_;
    const pid_t thread_id_;
    Timestamp poll_return_time_;
    std::mutex mutex_;
    std::vector<Functor> pending_functors_;
    std::any context_;
};

}  // namespace gw::net
