#pragma once

#include <sys/socket.h>

#include "base/noncopyable.hpp"

namespace gw::net {

class InetAddress;

class Socket : noncopyable {
public:
    explicit Socket(int sockfd) : sockfd_(sockfd) {}
    ~Socket();

    int fd() const { return sockfd_; }
    int release();

    void bindAddress(const InetAddress& local_addr);
    void listen();
    // accept4(SOCK_NONBLOCK|SOCK_CLOEXEC)；失败返回 -1，*saved_errno 有效
    int accept(InetAddress* peer, int* saved_errno);

    void shutdownWrite();

    void setTcpNoDelay(bool on);
    void setReuseAddr(bool on);
    void setReusePort(bool on);
    void setKeepAlive(bool on);

    static int createNonblocking(sa_family_t family);

private:
    const int sockfd_;
};

}  // namespace gw::net
