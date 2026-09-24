#include "net/channel.hpp"

#include <cassert>

#include "base/log.hpp"
#include "net/event_loop.hpp"

namespace gw::net {

Channel::Channel(EventLoop* loop, int fd) : loop_(loop), fd_(fd) {}

Channel::~Channel() {
    assert(!event_handling_);
    assert(!added_to_poller_);
}

void Channel::tie(const std::shared_ptr<void>& obj) {
    tie_ = obj;
    tied_ = true;
}

void Channel::update() {
    added_to_poller_ = true;
    loop_->updateChannel(this);
}

void Channel::remove() {
    assert(isNoneEvent());
    if (!added_to_poller_) return;
    added_to_poller_ = false;
    loop_->removeChannel(this);
}

void Channel::handleEvent(Timestamp receive_time) {
    std::shared_ptr<void> guard;
    if (tied_) {
        guard = tie_.lock();
        if (guard) handleEventWithGuard(receive_time);
    } else {
        handleEventWithGuard(receive_time);
    }
}

void Channel::handleEventWithGuard(Timestamp receive_time) {
    event_handling_ = true;

    if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN)) {
        if (close_callback_) close_callback_();
    }
    if (revents_ & EPOLLERR) {
        if (error_callback_) error_callback_();
    }
    if (revents_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) {
        if (read_callback_) read_callback_();
    }
    if (revents_ & EPOLLOUT) {
        if (write_callback_) write_callback_();
    }

    event_handling_ = false;
    (void)receive_time;
}

void Channel::enableReading() {
    events_ |= kReadEvent;
    update();
}

void Channel::disableReading() {
    events_ &= ~kReadEvent;
    update();
}

void Channel::enableWriting() {
    events_ |= kWriteEvent;
    update();
}

void Channel::disableWriting() {
    events_ &= ~kWriteEvent;
    update();
}

void Channel::disableAll() {
    events_ = kNoneEvent;
    update();
}

}  // namespace gw::net
