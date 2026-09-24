#include "net/timer_queue.hpp"

#include <sys/timerfd.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstring>
#include <iterator>

#include "base/current_thread.hpp"
#include "base/log.hpp"
#include "net/channel.hpp"
#include "net/event_loop.hpp"

namespace gw::net {

namespace {

timespec toTimespec(Timestamp t) {
    auto d = t.time_since_epoch();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(d);
    auto nsecs = std::chrono::duration_cast<std::chrono::nanoseconds>(d - secs);
    timespec ts{};
    ts.tv_sec = static_cast<time_t>(secs.count());
    ts.tv_nsec = static_cast<long>(nsecs.count());
    return ts;
}

int createTimerFd() {
    int fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) {
        SPDLOG_CRITICAL("timerfd_create failed: {}", std::strerror(errno));
        abort();
    }
    return fd;
}

void readTimerFd(int fd) {
    std::uint64_t howmany = 0;
    ssize_t n = ::read(fd, &howmany, sizeof(howmany));
    if (n != sizeof(howmany)) {
        if (errno != EAGAIN) SPDLOG_ERROR("timerfd read failed: {}", std::strerror(errno));
    }
}

void resetTimerFd(int fd, Timestamp expiration) {
    itimerspec new_value{};
    new_value.it_value = toTimespec(expiration);
    if (::timerfd_settime(fd, TFD_TIMER_ABSTIME, &new_value, nullptr) < 0) {
        SPDLOG_ERROR("timerfd_settime failed: {}", std::strerror(errno));
    }
}

}  // namespace

TimerQueue::TimerQueue(EventLoop* loop) : loop_(loop), timer_fd_(createTimerFd()), channel_(nullptr) {
    channel_ = std::make_unique<Channel>(loop, timer_fd_);
    channel_->setReadCallback([this] { handleRead(); });
    channel_->enableReading();
}

TimerQueue::~TimerQueue() {
    channel_->disableAll();
    channel_->remove();
    ::close(timer_fd_);
    for (auto& entry : timers_) {
        delete entry.second;
    }
}

TimerId TimerQueue::addTimer(TimerCallback cb, Timestamp when, Duration interval) {
    auto* timer = new Timer{std::move(cb), when, interval, interval.count() > 0, ++next_sequence_};
    loop_->assertInLoopThread();
    bool earliest_changed = insert(timer);
    if (earliest_changed) {
        resetTimerFd(timer_fd_, timer->expiration);
    }
    return TimerId(timer, timer->sequence);
}

bool TimerQueue::insert(Timer* timer) {
    loop_->assertInLoopThread();
    bool earliest_changed = false;
    auto it = timers_.begin();
    if (it == timers_.end() || timer->expiration < it->first) {
        earliest_changed = true;
    }
    timers_.insert(Entry(timer->expiration, timer));
    active_timers_.insert(std::make_pair(timer, timer->sequence));
    return earliest_changed;
}

void TimerQueue::cancel(TimerId timer_id) {
    loop_->assertInLoopThread();
    auto it = active_timers_.find(std::make_pair(timer_id.timer_, timer_id.sequence_));
    if (it != active_timers_.end()) {
        Timer* timer = it->first;
        timers_.erase(Entry(timer->expiration, timer));
        active_timers_.erase(it);
        delete timer;
    } else if (calling_expired_timers_) {
        // 定时器已经到期、正在回调中：只标记，等 reset 时不再重新插入并删除
        canceling_timers_.insert(timer_id.timer_);
    }
}

void TimerQueue::handleRead() {
    loop_->assertInLoopThread();
    Timestamp now = nowSteady();
    readTimerFd(timer_fd_);

    std::vector<Entry> expired = getExpired(now);

    calling_expired_timers_ = true;
    canceling_timers_.clear();
    for (const Entry& entry : expired) {
        entry.second->cb();
    }
    calling_expired_timers_ = false;

    reset(expired, now);
}

std::vector<TimerQueue::Entry> TimerQueue::getExpired(Timestamp now) {
    std::vector<Entry> expired;
    Entry sentry(now, reinterpret_cast<Timer*>(UINTPTR_MAX));
    auto end = timers_.lower_bound(sentry);
    assert(end == timers_.end() || now < end->first);
    std::copy(timers_.begin(), end, std::back_inserter(expired));
    timers_.erase(timers_.begin(), end);

    for (const Entry& entry : expired) {
        active_timers_.erase(std::make_pair(entry.second, entry.second->sequence));
    }
    return expired;
}

void TimerQueue::reset(const std::vector<Entry>& expired, Timestamp now) {
    Timestamp next_expire;
    for (const Entry& entry : expired) {
        Timer* timer = entry.second;
        if (timer->repeat && canceling_timers_.find(timer) == canceling_timers_.end()) {
            timer->expiration = now + timer->interval;
            insert(timer);
        } else {
            delete timer;
        }
    }
    if (!timers_.empty()) {
        next_expire = timers_.begin()->second->expiration;
    }
    if (next_expire != Timestamp{}) {
        resetTimerFd(timer_fd_, next_expire);
    }
}

}  // namespace gw::net
