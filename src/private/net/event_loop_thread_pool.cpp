#include "net/event_loop_thread_pool.hpp"

#include <cassert>

#include "base/log.hpp"
#include "net/event_loop.hpp"

namespace gw::net {

EventLoopThreadPool::EventLoopThreadPool(EventLoop* base_loop, std::string name)
    : base_loop_(base_loop), name_(std::move(name)) {}

EventLoopThreadPool::~EventLoopThreadPool() = default;

void EventLoopThreadPool::start(const EventLoopThread::ThreadInitCallback& cb) {
    assert(!started_);
    base_loop_->assertInLoopThread();
    started_ = true;
    for (int i = 0; i < num_threads_; ++i) {
        auto thread = std::make_unique<EventLoopThread>(cb, name_ + std::to_string(i));
        loops_.push_back(thread->startLoop());
        threads_.push_back(std::move(thread));
    }
    if (num_threads_ == 0 && cb) {
        cb(base_loop_);
    }
}

EventLoop* EventLoopThreadPool::getNextLoop() {
    base_loop_->assertInLoopThread();
    if (loops_.empty()) return base_loop_;
    EventLoop* loop = loops_[static_cast<std::size_t>(next_)];
    if (++next_ >= static_cast<int>(loops_.size())) next_ = 0;
    return loop;
}

EventLoop* EventLoopThreadPool::getLoopForHash(std::size_t hash_code) {
    base_loop_->assertInLoopThread();
    if (loops_.empty()) return base_loop_;
    return loops_[hash_code % loops_.size()];
}

std::vector<EventLoop*> EventLoopThreadPool::allLoops() {
    base_loop_->assertInLoopThread();
    if (loops_.empty()) return {base_loop_};
    return loops_;
}

}  // namespace gw::net
