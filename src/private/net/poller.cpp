#include "net/poller.hpp"

#include <cassert>
#include <cerrno>
#include <cstring>

#include "base/log.hpp"
#include "net/channel.hpp"
#include "net/event_loop.hpp"

namespace gw::net {

Poller::Poller(EventLoop* loop) : owner_loop_(loop) {}

Poller::~Poller() = default;

bool Poller::hasChannel(Channel* channel) const {
    auto it = channels_.find(channel->fd());
    return it != channels_.end() && it->second == channel;
}

void Poller::assertInLoopThread() const {
    owner_loop_->assertInLoopThread();
}

Poller* Poller::newDefaultPoller(EventLoop* loop) {
    return new EpollPoller(loop);
}

EpollPoller::EpollPoller(EventLoop* loop)
    : Poller(loop), epoll_fd_(::epoll_create1(EPOLL_CLOEXEC)), events_(kInitEventListSize) {
    if (epoll_fd_ < 0) {
        SPDLOG_CRITICAL("epoll_create1 failed: {}", std::strerror(errno));
        abort();
    }
}

EpollPoller::~EpollPoller() {
    ::close(epoll_fd_);
}

Timestamp EpollPoller::poll(int timeout_ms, ChannelList* active_channels) {
    int num_events = ::epoll_wait(epoll_fd_, events_.data(), static_cast<int>(events_.size()), timeout_ms);
    int saved_errno = errno;
    Timestamp now = nowSteady();
    if (num_events > 0) {
        fillActiveChannels(num_events, active_channels);
        if (static_cast<std::size_t>(num_events) == events_.size()) {
            events_.resize(events_.size() * 2);
        }
    } else if (num_events == 0) {
        // 超时
    } else if (saved_errno != EINTR) {
        errno = saved_errno;
        SPDLOG_ERROR("epoll_wait failed: {}", std::strerror(errno));
    }
    return now;
}

void EpollPoller::fillActiveChannels(int num_events, ChannelList* active_channels) const {
    for (int i = 0; i < num_events; ++i) {
        auto* channel = static_cast<Channel*>(events_[static_cast<std::size_t>(i)].data.ptr);
        channel->setRevents(events_[static_cast<std::size_t>(i)].events);
        active_channels->push_back(channel);
    }
}

void EpollPoller::updateChannel(Channel* channel) {
    assertInLoopThread();
    const int index = channel->index();
    if (index == kNew || index == kDeleted) {
        if (index == kNew) {
            int fd = channel->fd();
            channels_[fd] = channel;
        }
        channel->setIndex(kAdded);
        update(EPOLL_CTL_ADD, channel);
    } else {
        if (channel->isNoneEvent()) {
            update(EPOLL_CTL_DEL, channel);
            channel->setIndex(kDeleted);
        } else {
            update(EPOLL_CTL_MOD, channel);
        }
    }
}

void EpollPoller::removeChannel(Channel* channel) {
    assertInLoopThread();
    int fd = channel->fd();
    auto it = channels_.find(fd);
    assert(it != channels_.end() && it->second == channel);
    channels_.erase(it);
    int index = channel->index();
    if (index == kAdded) {
        update(EPOLL_CTL_DEL, channel);
    }
    channel->setIndex(kNew);
}

void EpollPoller::update(int operation, Channel* channel) {
    struct epoll_event event;
    std::memset(&event, 0, sizeof(event));
    event.events = static_cast<std::uint32_t>(channel->events());
    event.data.ptr = channel;
    int fd = channel->fd();
    if (::epoll_ctl(epoll_fd_, operation, fd, &event) < 0) {
        if (operation == EPOLL_CTL_DEL) {
            SPDLOG_ERROR("epoll_ctl DEL failed fd={}: {}", fd, std::strerror(errno));
        } else {
            SPDLOG_ERROR("epoll_ctl {} failed fd={}: {}", operation == EPOLL_CTL_ADD ? "ADD" : "MOD", fd,
                         std::strerror(errno));
        }
    }
}

}  // namespace gw::net
