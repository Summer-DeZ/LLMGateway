#include "net/event_loop.hpp"

#include <sys/eventfd.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstring>
#include <csignal>

#include "base/current_thread.hpp"
#include "base/log.hpp"
#include "net/channel.hpp"
#include "net/poller.hpp"
#include "net/timer_queue.hpp"

namespace gw::net {

namespace {

thread_local EventLoop* t_loop_in_this_thread = nullptr;

constexpr int kPollTimeMs = 10000;

int createEventfd() {
    int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) {
        SPDLOG_CRITICAL("eventfd failed: {}", std::strerror(errno));
        abort();
    }
    return fd;
}

struct IgnoreSigPipe {
    IgnoreSigPipe() { ::signal(SIGPIPE, SIG_IGN); }
};
IgnoreSigPipe ignore_sigpipe;

}  // namespace

EventLoop::EventLoop()
    : poller_(Poller::newDefaultPoller(this)),
      wakeup_fd_(createEventfd()),
      timer_queue_(nullptr),
      thread_id_(CurrentThread::tid()) {
    if (t_loop_in_this_thread != nullptr) {
        SPDLOG_CRITICAL("another EventLoop {} exists in this thread {}", static_cast<const void*>(t_loop_in_this_thread),
                        thread_id_);
        abort();
    }
    t_loop_in_this_thread = this;
    wakeup_channel_ = std::make_unique<Channel>(this, wakeup_fd_);
    wakeup_channel_->setReadCallback([this] { handleWakeupRead(); });
    wakeup_channel_->enableReading();
    timer_queue_ = std::make_unique<TimerQueue>(this);
}

EventLoop::~EventLoop() {
    wakeup_channel_->disableAll();
    wakeup_channel_->remove();
    ::close(wakeup_fd_);
    t_loop_in_this_thread = nullptr;
}

EventLoop* EventLoop::current() {
    return t_loop_in_this_thread;
}

void EventLoop::loop() {
    assertInLoopThread();
    quit_.store(false);
    while (!quit_.load(std::memory_order_acquire)) {
        // 先执行本线程在 loop() 之前/上一轮 poll 之后排队的任务，避免它们等满一次 poll 超时
        doPendingFunctors();
        if (quit_.load(std::memory_order_acquire)) break;
        Poller::ChannelList active_channels;
        poll_return_time_ = poller_->poll(kPollTimeMs, &active_channels);
        for (Channel* channel : active_channels) {
            channel->handleEvent(poll_return_time_);
        }
    }
    doPendingFunctors();
}

void EventLoop::quit() {
    quit_.store(true, std::memory_order_release);
    if (!isInLoopThread()) {
        wakeup();
    }
}

void EventLoop::runInLoop(Functor cb) {
    if (isInLoopThread()) {
        cb();
    } else {
        queueInLoop(std::move(cb));
    }
}

void EventLoop::queueInLoop(Functor cb) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_functors_.push_back(std::move(cb));
    }
    if (!isInLoopThread() || calling_pending_functors_.load(std::memory_order_acquire)) {
        wakeup();
    }
}

TimerId EventLoop::runAt(Timestamp time, TimerCallback cb) {
    return timer_queue_->addTimer(std::move(cb), time, Duration::zero());
}

TimerId EventLoop::runAfter(Duration delay, TimerCallback cb) {
    Timestamp time = nowSteady() + delay;
    return timer_queue_->addTimer(std::move(cb), time, Duration::zero());
}

TimerId EventLoop::runEvery(Duration interval, TimerCallback cb) {
    Timestamp time = nowSteady() + interval;
    return timer_queue_->addTimer(std::move(cb), time, interval);
}

void EventLoop::cancel(TimerId timer_id) {
    if (isInLoopThread()) {
        timer_queue_->cancel(timer_id);
    } else {
        queueInLoop([this, timer_id] { timer_queue_->cancel(timer_id); });
    }
}

void EventLoop::updateChannel(Channel* channel) {
    assert(channel->ownerLoop() == this);
    assertInLoopThread();
    poller_->updateChannel(channel);
}

void EventLoop::removeChannel(Channel* channel) {
    assert(channel->ownerLoop() == this);
    assertInLoopThread();
    poller_->removeChannel(channel);
}

bool EventLoop::isInLoopThread() const {
    return thread_id_ == CurrentThread::tid();
}

void EventLoop::assertInLoopThread() const {
    if (!isInLoopThread()) {
        SPDLOG_CRITICAL("EventLoop::assertInLoopThread failed: loop thread {} current thread {}", thread_id_,
                        CurrentThread::tid());
        abort();
    }
}

void EventLoop::wakeup() {
    std::uint64_t one = 1;
    ssize_t n = ::write(wakeup_fd_, &one, sizeof(one));
    if (n != sizeof(one)) {
        if (errno != EAGAIN) SPDLOG_ERROR("EventLoop::wakeup write failed: {}", std::strerror(errno));
    }
}

void EventLoop::handleWakeupRead() {
    std::uint64_t one = 0;
    ssize_t n = ::read(wakeup_fd_, &one, sizeof(one));
    if (n != sizeof(one)) {
        if (errno != EAGAIN) SPDLOG_ERROR("EventLoop::handleWakeupRead failed: {}", std::strerror(errno));
    }
}

void EventLoop::doPendingFunctors() {
    std::vector<Functor> functors;
    calling_pending_functors_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        functors.swap(pending_functors_);
    }
    for (Functor& functor : functors) {
        functor();
    }
    calling_pending_functors_.store(false, std::memory_order_release);
}

}  // namespace gw::net
