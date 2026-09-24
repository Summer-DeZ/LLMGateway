#pragma once

#include <sys/epoll.h>

#include <map>
#include <vector>

#include "base/noncopyable.hpp"
#include "base/timestamp.hpp"

namespace gw::net {

class Channel;
class EventLoop;

class Poller : noncopyable {
public:
    using ChannelList = std::vector<Channel*>;

    explicit Poller(EventLoop* loop);
    virtual ~Poller();

    virtual Timestamp poll(int timeout_ms, ChannelList* active_channels) = 0;
    virtual void updateChannel(Channel* channel) = 0;
    virtual void removeChannel(Channel* channel) = 0;

    bool hasChannel(Channel* channel) const;
    void assertInLoopThread() const;

    static Poller* newDefaultPoller(EventLoop* loop);

protected:
    using ChannelMap = std::map<int, Channel*>;
    ChannelMap channels_;

private:
    EventLoop* owner_loop_;
};

class EpollPoller : public Poller {
public:
    explicit EpollPoller(EventLoop* loop);
    ~EpollPoller() override;

    Timestamp poll(int timeout_ms, ChannelList* active_channels) override;
    void updateChannel(Channel* channel) override;
    void removeChannel(Channel* channel) override;

private:
    static constexpr int kInitEventListSize = 16;
    static constexpr int kNew = -1;
    static constexpr int kAdded = 1;
    static constexpr int kDeleted = 2;

    void fillActiveChannels(int num_events, ChannelList* active_channels) const;
    void update(int operation, Channel* channel);

    int epoll_fd_;
    std::vector<struct epoll_event> events_;
};

}  // namespace gw::net
