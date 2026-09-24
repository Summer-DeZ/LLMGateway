#include "net/event_loop_thread.hpp"

#include <cassert>

#include "base/current_thread.hpp"
#include "net/event_loop.hpp"

namespace gw::net {

EventLoopThread::EventLoopThread(ThreadInitCallback cb, std::string name)
    : loop_(nullptr), exiting_(false), callback_(std::move(cb)), name_(std::move(name)) {}

EventLoopThread::~EventLoopThread() {
    exiting_ = true;
    if (loop_ != nullptr) {
        loop_->quit();
        if (thread_.joinable()) thread_.join();
    }
}

EventLoop* EventLoopThread::startLoop() {
    assert(!thread_.joinable());
    thread_ = std::thread([this] { threadFunc(); });

    EventLoop* loop = nullptr;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this] { return loop_ != nullptr; });
        loop = loop_;
    }
    return loop;
}

void EventLoopThread::threadFunc() {
    EventLoop loop;
    if (!name_.empty()) CurrentThread::setName(name_);
    if (callback_) callback_(&loop);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        loop_ = &loop;
        cond_.notify_one();
    }

    loop.loop();

    std::lock_guard<std::mutex> lock(mutex_);
    loop_ = nullptr;
}

}  // namespace gw::net
