#pragma once

#include <cstdint>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "base/noncopyable.hpp"
#include "base/timestamp.hpp"
#include "net/callbacks.hpp"
#include "net/timer_id.hpp"

namespace gw::net {

class Channel;
class EventLoop;

struct Timer {
    TimerCallback cb;
    Timestamp expiration;
    Duration interval;
    bool repeat = false;
    std::int64_t sequence = 0;
};

class TimerQueue : noncopyable {
public:
    explicit TimerQueue(EventLoop* loop);
    ~TimerQueue();

    TimerId addTimer(TimerCallback cb, Timestamp when, Duration interval);
    void cancel(TimerId timer_id);

private:
    using Entry = std::pair<Timestamp, Timer*>;
    using TimerList = std::set<Entry>;

    void handleRead();
    std::vector<Entry> getExpired(Timestamp now);
    void reset(const std::vector<Entry>& expired, Timestamp now);
    bool insert(Timer* timer);

    EventLoop* loop_;
    int timer_fd_;
    std::unique_ptr<Channel> channel_;
    TimerList timers_;
    std::set<std::pair<Timer*, std::int64_t>> active_timers_;
    std::set<Timer*> canceling_timers_;
    bool calling_expired_timers_ = false;
    std::int64_t next_sequence_ = 0;
};

}  // namespace gw::net
