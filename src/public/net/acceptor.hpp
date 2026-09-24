#pragma once

#include <functional>

#include "base/noncopyable.hpp"
#include "net/channel.hpp"
#include "net/inet_address.hpp"
#include "net/socket.hpp"

namespace gw::net {

class EventLoop;

class Acceptor : noncopyable {
public:
    using NewConnectionCallback = std::function<void(int sockfd, const InetAddress& peer)>;

    Acceptor(EventLoop* loop, const InetAddress& listen_addr, bool reuse_port);
    ~Acceptor();

    void setNewConnectionCallback(NewConnectionCallback cb) { new_connection_callback_ = std::move(cb); }
    bool listening() const { return listening_; }
    void listen();
    void stop();

private:
    void handleRead();

    EventLoop* loop_;
    Socket accept_socket_;
    Channel accept_channel_;
    NewConnectionCallback new_connection_callback_;
    bool listening_;
    int idle_fd_;
};

}  // namespace gw::net
