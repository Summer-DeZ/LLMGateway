#pragma once

#include <sys/epoll.h>

#include <functional>
#include <memory>

#include "base/noncopyable.hpp"
#include "base/timestamp.hpp"

namespace gw::net {

class EventLoop;

class Channel : noncopyable {
public:
    using EventCallback = std::function<void()>;

    Channel(EventLoop* loop, int fd);
    ~Channel();

    void handleEvent(Timestamp receive_time);

    void setReadCallback(EventCallback cb) { read_callback_ = std::move(cb); }
    void setWriteCallback(EventCallback cb) { write_callback_ = std::move(cb); }
    void setCloseCallback(EventCallback cb) { close_callback_ = std::move(cb); }
    void setErrorCallback(EventCallback cb) { error_callback_ = std::move(cb); }

    // 防止 handleEvent 期间对象被析构
    void tie(const std::shared_ptr<void>& obj);

    int fd() const { return fd_; }
    int events() const { return events_; }
    void setRevents(int revents) { revents_ = revents; }
    bool isNoneEvent() const { return events_ == kNoneEvent; }

    void enableReading();
    void disableReading();
    void enableWriting();
    void disableWriting();
    void disableAll();

    bool isWriting() const { return (events_ & kWriteEvent) != 0; }
    bool isReading() const { return (events_ & kReadEvent) != 0; }

    int index() const { return index_; }
    void setIndex(int index) { index_ = index; }

    EventLoop* ownerLoop() const { return loop_; }
    void remove();

private:
    void update();
    void handleEventWithGuard(Timestamp receive_time);

    static constexpr int kNoneEvent = 0;
    static constexpr int kReadEvent = EPOLLIN | EPOLLPRI | EPOLLRDHUP;
    static constexpr int kWriteEvent = EPOLLOUT;

    EventLoop* loop_;
    const int fd_;
    int events_ = kNoneEvent;
    int revents_ = 0;
    int index_ = -1;
    bool event_handling_ = false;
    bool added_to_poller_ = false;
    std::weak_ptr<void> tie_;
    bool tied_ = false;

    EventCallback read_callback_;
    EventCallback write_callback_;
    EventCallback close_callback_;
    EventCallback error_callback_;
};

}  // namespace gw::net
