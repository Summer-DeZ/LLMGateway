#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "base/noncopyable.hpp"
#include "net/event_loop_thread.hpp"

namespace gw::net {

class EventLoop;

class EventLoopThreadPool : noncopyable {
public:
    EventLoopThreadPool(EventLoop* base_loop, std::string name);
    ~EventLoopThreadPool();

    void setThreadNum(int num_threads) { num_threads_ = num_threads; }
    void start(const EventLoopThread::ThreadInitCallback& cb = {});

    EventLoop* getNextLoop();
    EventLoop* getLoopForHash(std::size_t hash_code);
    std::vector<EventLoop*> allLoops();

    bool started() const { return started_; }
    const std::string& name() const { return name_; }

private:
    EventLoop* base_loop_;
    std::string name_;
    bool started_ = false;
    int num_threads_ = 0;
    int next_ = 0;
    std::vector<std::unique_ptr<EventLoopThread>> threads_;
    std::vector<EventLoop*> loops_;
};

}  // namespace gw::net
