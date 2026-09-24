#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "base/noncopyable.hpp"
#include "net/channel.hpp"
#include "net/inet_address.hpp"
#include "net/timer_id.hpp"

namespace gw::net {

class EventLoop;

enum class ConnectErrorKind { resolve, refused, unreachable, timeout, cancelled };

struct ConnectError {
    ConnectErrorKind kind = ConnectErrorKind::unreachable;
    std::string message;
};

class Connector : noncopyable, public std::enable_shared_from_this<Connector> {
public:
    Connector(EventLoop* loop, std::vector<InetAddress> addrs, std::chrono::milliseconds timeout);
    ~Connector();

    void start(std::function<void(int fd, InetAddress peer)> on_connected, std::function<void(ConnectError)> on_failed);
    void cancel();
    bool finished() const { return finished_; }

private:
    void connectNext();
    void removeAndResetChannel();
    void handleWrite();
    void finishSuccess();
    void finishFailure(ConnectErrorKind kind, const std::string& message);

    EventLoop* loop_;
    std::vector<InetAddress> addrs_;
    std::size_t next_ = 0;
    std::chrono::milliseconds timeout_;
    std::unique_ptr<Channel> channel_;
    int fd_ = -1;
    TimerId timeout_timer_;
    bool cancelled_ = false;
    bool finished_ = false;
    ConnectError last_error_;
    std::function<void(int, InetAddress)> on_connected_;
    std::function<void(ConnectError)> on_failed_;
};

}  // namespace gw::net
